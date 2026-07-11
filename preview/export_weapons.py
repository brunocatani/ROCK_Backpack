"""Build desktop-only static weapon GLBs through nif_mcp (requires mcp).

Inputs are the extracted vanilla Meshes/Materials/Textures resource tree.
Each assembly uses its NIF connect points, with the top-level transform
replaced just as BackpackScene replaces a mounted reference's root pose.
The result uses NIF axes in meters; Scene3D converts meters to game units.
"""
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import struct
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


ASSEMBLIES = {
    "10mm": [
        ("10mmPistol/10mmPistol.nif", None),
        ("10mmPistol/10mmGrip.nif", "P-Grip"),
        ("10mmPistol/10mmMag01.nif", "P-Mag"),
        ("10mmPistol/10mmPistolHeavyBarrel.nif", "P-Barrel"),
    ],
    "hunting-rifle": [
        ("HuntingRifle/HuntingRifleReceiver.nif", None),
        ("HuntingRifle/HuntingRifleStock.nif", "P-Grip"),
        ("HuntingRifle/HuntingRifleBarrel.nif", "P-Barrel"),
        ("HuntingRifle/308SmallMag.nif", "P-Mag"),
        ("HuntingRifle/HuntingRifleIronSights.nif", "P-Scope"),
    ],
    "combat-rifle": [
        ("CombatShotgun/CombatRifle.nif", None),
        ("CombatShotgun/Stock.nif", "P-Grip"),
        ("CombatShotgun/CombatRifleBarrel.nif", "P-Barrel"),
        ("CombatShotgun/CombatRifleMagSmall.nif", "P-Mag"),
        ("CombatShotgun/CombatRifleSightsIron.nif", "P-Scope"),
        ("CombatShotgun/CombatRifleSightsPin.nif", "P-FrontSight"),
    ],
}


def read_glb(path):
    data = path.read_bytes()
    assert struct.unpack_from("<III", data) == (0x46546C67, 2, len(data))
    length, kind = struct.unpack_from("<II", data, 12)
    assert kind == 0x4E4F534A
    doc = json.loads(data[20:20 + length])
    size, kind = struct.unpack_from("<II", data, 20 + length)
    assert kind == 0x004E4942 and len(data) == 28 + length + size
    return doc, data[28 + length:]


def write_glb(path, doc, binary):
    # Shared receiver textures recur in several parts. Store their exact bytes
    # once without resampling or changing material references.
    packed, offsets = bytearray(), {}
    for view in doc["bufferViews"]:
        offset, length = view.get("byteOffset", 0), view["byteLength"]
        payload = binary[offset:offset + length]
        assert len(payload) == length
        digest = hashlib.sha256(payload).digest()
        if digest not in offsets:
            packed.extend(b"\0" * (-len(packed) % 4))
            offsets[digest] = len(packed)
            packed.extend(payload)
        view["byteOffset"] = offsets[digest]
    binary = bytes(packed)
    binary += b"\0" * (-len(binary) % 4)
    doc["buffers"] = [{"byteLength": len(binary)}]
    text = json.dumps(doc, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    path.write_bytes(struct.pack("<IIIII", 0x46546C67, 2, 28 + len(text) + len(binary),
                                len(text), 0x4E4F534A) + text +
                     struct.pack("<II", len(binary), 0x004E4942) + binary)


def append_part(dest, binary, part, blob):
    """Rebase all glTF references without changing geometry or materials."""
    keys = ("nodes", "meshes", "accessors", "bufferViews", "materials", "textures", "images", "samplers")
    offsets = {k: len(dest.setdefault(k, [])) for k in keys}
    assert not part.get("skins") and not part.get("animations"), "Static rigid models required"
    binary += b"\0" * (-len(binary) % 4)
    for view in part.get("bufferViews", []):
        assert view["buffer"] == 0
        view["byteOffset"] = view.get("byteOffset", 0) + len(binary)
    for accessor in part.get("accessors", []):
        assert "sparse" not in accessor
        accessor["bufferView"] += offsets["bufferViews"]
    for image in part.get("images", []):
        image["bufferView"] += offsets["bufferViews"]
    for texture in part.get("textures", []):
        texture["source"] += offsets["images"]
        if "sampler" in texture:
            texture["sampler"] += offsets["samplers"]
    for mat in part.get("materials", []):
        slots = [mat.get(k) for k in ("normalTexture", "occlusionTexture", "emissiveTexture")]
        slots += [mat.get("pbrMetallicRoughness", {}).get(k) for k in ("baseColorTexture", "metallicRoughnessTexture")]
        for slot in slots:
            if slot:
                slot["index"] += offsets["textures"]
    for mesh in part.get("meshes", []):
        for primitive in mesh["primitives"]:
            assert primitive.get("mode", 4) == 4 and not primitive.get("targets")
            primitive["attributes"] = {k: v + offsets["accessors"] for k, v in primitive["attributes"].items()}
            primitive["indices"] += offsets["accessors"]
            if "material" in primitive:
                primitive["material"] += offsets["materials"]
    for node in part["nodes"]:
        if "mesh" in node:
            node["mesh"] += offsets["meshes"]
        if "children" in node:
            node["children"] = [i + offsets["nodes"] for i in node["children"]]
    roots = part["scenes"][part.get("scene", 0)]["nodes"]
    assert len(roots) == 1
    for key in keys:
        dest[key].extend(part.get(key, []))
    return binary + blob, roots[0] + offsets["nodes"]


async def build(args):
    args.work.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    provenance = {"coordinateFrame": "NIF root-local axes, meters; root pose replaced by mount pose",
                  "sourceArchive": "Fallout4 - Meshes.ba2", "weapons": {}}
    params = StdioServerParameters(command=sys.executable, args=[str(args.server)])
    async with stdio_client(params) as (r, w):
        async with ClientSession(r, w) as session:
            await session.initialize()
            for name, parts in ASSEMBLIES.items():
                doc = {"asset": {"version": "2.0", "generator": "ROCK Backpack static NIF assembly"},
                       "scene": 0, "scenes": [{"nodes": [0]}]}
                binary = b""
                sources = []
                for relative, connection in parts:
                    source = args.input / "Meshes/Weapons" / relative
                    intermediate = args.work / (source.stem + ".glb")
                    digest = hashlib.sha256(source.read_bytes()).hexdigest()
                    result = await session.call_tool("nif_export_gltf", {
                        "path": str(source), "output_path": str(intermediate), "data_path": str(args.input),
                        "overwrite": True,
                    })
                    result = json.loads(result.content[0].text)
                    if not result.get("ok") or result.get("data", {}).get("warning"):
                        raise RuntimeError(f"{relative}: {result}")
                    assert hashlib.sha256(source.read_bytes()).hexdigest() == digest, "Source changed"
                    part, blob = read_glb(intermediate)
                    parent, point = None, None
                    if connection:
                        matches = [(i, p) for i, n in enumerate(doc["nodes"])
                                   for p in n.get("extras", {}).get("NIF Connect Points", []) if p["name"] == connection]
                        if len(matches) != 1:
                            available = [p["name"] for n in doc["nodes"] for p in n.get("extras", {}).get("NIF Connect Points", [])]
                            raise RuntimeError(f"{name}: ambiguous/missing {connection}; available {available}")
                        owner, point = matches[0]
                        parents = ([i for i, n in enumerate(doc["nodes"]) if n.get("name") == point["parent"]]
                                   if point["parent"] else [owner])
                        assert len(parents) == 1, point
                        parent = parents[0]
                    binary, root = append_part(doc, binary, part, blob)
                    node = doc["nodes"][root]
                    for key in ("matrix", "translation", "rotation", "scale"):
                        node.pop(key, None)
                    if point:
                        node["translation"] = [v * 0.9144 / 64 for v in point["translation"]]
                        node["rotation"] = point["rotation"]
                        node["scale"] = [point["scale"]] * 3
                        doc["nodes"][parent].setdefault("children", []).append(root)
                    sources.append({"path": relative, "sha256": digest, "connection": connection})
                output = args.output / (name + ".glb")
                write_glb(output, doc, binary)
                provenance["weapons"][name] = {"sources": sources, "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                    "meshes": len(doc["meshes"]), "materials": len(doc["materials"])}
                print(f"Built {output} ({len(binary)} bytes)", flush=True)
    (args.output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("server", "input", "work", "output"):
        parser.add_argument("--" + flag, type=Path, required=True)
    asyncio.run(build(parser.parse_args()))
