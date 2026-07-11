from __future__ import annotations

import argparse
import asyncio
import json
import math
import os
import struct
import sys
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, AsyncIterator

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def decode_nested_json(value: Any) -> Any:
    for _ in range(4):
        if not isinstance(value, str):
            break
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            break
    if isinstance(value, dict) and set(value) == {"result"}:
        return decode_nested_json(value["result"])
    return value


def extract_payload(result: Any) -> Any:
    dumped = result.model_dump(mode="json")
    if dumped.get("isError"):
        raise RuntimeError("MCP marked the tool call as an error")

    structured = dumped.get("structuredContent")
    if structured is not None:
        decoded = decode_nested_json(structured)
        if decoded not in ({}, None):
            return decoded

    for item in dumped.get("content", []):
        if item.get("type") != "text":
            continue
        decoded = decode_nested_json(item.get("text", ""))
        if decoded != "":
            return decoded
    return None


@asynccontextmanager
async def open_session(server: Path) -> AsyncIterator[ClientSession]:
    require(server.is_file(), f"MCP server was not found: {server}")
    parameters = StdioServerParameters(
        command=sys.executable,
        args=[str(server)],
        cwd=server.parent,
        env=dict(os.environ),
    )
    async with stdio_client(parameters) as (read_stream, write_stream):
        async with ClientSession(read_stream, write_stream) as session:
            await session.initialize()
            yield session


async def call_tool(
    session: ClientSession,
    name: str,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    payload = extract_payload(await session.call_tool(name, arguments))
    require(isinstance(payload, dict), f"{name} returned no object payload")
    require(payload.get("ok") is not False, f"{name} failed: {payload}")
    status = str(payload.get("status", "")).lower()
    require(status not in {"error", "failed", "failure"}, f"{name} failed: {payload}")
    if "error" in payload:
        raise RuntimeError(f"{name} failed: {payload['error']}")
    return payload


def tool_data(payload: dict[str, Any]) -> Any:
    return payload.get("data", payload)


def close_vectors(left: list[float], right: list[float], tolerance: float = 1e-4) -> bool:
    return len(left) == len(right) and all(
        math.isclose(float(a), float(b), abs_tol=tolerance)
        for a, b in zip(left, right, strict=True)
    )


def validate_dds(runtime_root: Path, manifest: dict[str, Any]) -> None:
    texture_root = runtime_root / "Textures" / "military_backapack"
    files = {
        "diffuse": ("military_backpack_02_d.dds", {"DXGI_98"}),
        "normal": ("military_backpack_02_n.dds", {"BC5U", "ATI2", "DXGI_83"}),
        "specular": ("military_backpack_02_s.dds", {"DXGI_98"}),
        "metalness": ("military_backpack_02_m.dds", {"BC4U", "ATI1", "DXGI_80"}),
    }

    for role, (name, allowed_formats) in files.items():
        path = texture_root / name
        require(path.is_file(), f"Bundled DDS is missing: {path}")
        raw = path.read_bytes()
        require(len(raw) >= 128 and raw[:4] == b"DDS ", f"DDS header is invalid: {path}")
        height = struct.unpack_from("<I", raw, 12)[0]
        width = struct.unpack_from("<I", raw, 16)[0]
        mip_count = struct.unpack_from("<I", raw, 28)[0]
        four_cc = raw[84:88].decode("ascii").rstrip("\0")
        if four_cc == "DX10":
            require(len(raw) >= 148, f"DDS DX10 header is truncated: {path}")
            format_name = f"DXGI_{struct.unpack_from('<I', raw, 128)[0]}"
        else:
            format_name = four_cc

        expected = manifest["dds"][role]
        require([width, height] == expected["dimensions"], f"DDS dimensions drifted: {path}")
        require(mip_count == expected["mip_levels"], f"DDS mip chain is incomplete: {path}")
        require(format_name in allowed_formats, f"DDS format is wrong for {path}: {format_name}")


async def validate_nifs(
    runtime_root: Path,
    manifest: dict[str, Any],
    nif_server: Path,
) -> None:
    mesh_root = runtime_root / "Meshes" / "military_backapack"
    wearable = mesh_root / "military_backpack_02.nif"
    world = mesh_root / "military_backpack_02_go.nif"
    material = runtime_root / "Materials" / "military_backapack" / "military_backpack_02.bgsm"
    contract = manifest["nif_contract"]

    async with open_session(nif_server) as session:
        for label, path in (("wearable", wearable), ("world", world)):
            validated = tool_data(await call_tool(session, "nif_validate", {"path": str(path)}))
            require(
                isinstance(validated, list) and validated and all(item.get("valid") for item in validated),
                f"Bundled {label} NIF failed link validation",
            )
            await call_tool(session, "nif_check", {"path": str(path)})

        wearable_info = tool_data(await call_tool(session, "nif_info", {"path": str(wearable)}))
        wearable_geometry = tool_data(
            await call_tool(session, "nif_geometry_stats", {"path": str(wearable)})
        )
        wearable_blocks = tool_data(await call_tool(session, "nif_blocks", {"path": str(wearable)}))
        require(wearable_info["bsVersion"] == contract["bs_version"], "Wearable NIF is not FO4 stream 130")
        require(
            wearable_info["blockTypes"]
            == {
                "BSLightingShaderProperty": 1,
                "BSShaderTextureSet": 1,
                "BSSkin::BoneData": 1,
                "BSSkin::Instance": 1,
                "BSTriShape": 1,
                "NiNode": 2,
            },
            f"Wearable NIF block contract drifted: {wearable_info['blockTypes']}",
        )
        wearable_shape = wearable_geometry["shapes"][0]
        require(wearable_geometry["shapeCount"] == 1, "Wearable NIF must contain one render shape")
        require(
            wearable_shape["vertices"] == contract["vertices"]
            and wearable_shape["triangles"] == contract["triangles"],
            "Wearable NIF geometry counts drifted",
        )
        require(
            wearable_shape["isSkinned"]
            and wearable_shape["hasUV"]
            and wearable_shape["hasNormals"]
            and wearable_shape["hasTangents"],
            "Wearable NIF lost skinning or required vertex attributes",
        )
        require(
            close_vectors(wearable_shape["minimum"], contract["wearable_bounds"]["minimum"])
            and close_vectors(wearable_shape["maximum"], contract["wearable_bounds"]["maximum"]),
            "Wearable NIF bounds drifted",
        )
        block_names = {str(block.get("name", "")) for block in wearable_blocks}
        require(contract["wearable_bone"] in block_names, "Wearable NIF lost Chest_Rear_Skin")
        require(
            "Materials\\military_backapack\\military_backpack_02.bgsm" in block_names,
            "Wearable NIF material path drifted",
        )

        world_info = tool_data(await call_tool(session, "nif_info", {"path": str(world)}))
        world_geometry = tool_data(await call_tool(session, "nif_geometry_stats", {"path": str(world)}))
        world_blocks = tool_data(await call_tool(session, "nif_blocks", {"path": str(world)}))
        require(world_info["bsVersion"] == contract["bs_version"], "World NIF is not FO4 stream 130")
        require(
            world_info["blockTypes"]
            == {
                "BSLightingShaderProperty": 1,
                "BSShaderTextureSet": 1,
                "BSTriShape": 1,
                "BSXFlags": 1,
                "NiNode": 1,
                "bhkNPCollisionObject": 1,
                "bhkPhysicsSystem": 1,
            },
            f"World NIF collision contract drifted: {world_info['blockTypes']}",
        )
        world_shape = world_geometry["shapes"][0]
        require(world_geometry["shapeCount"] == 1, "World NIF must contain one render shape")
        require(
            world_shape["vertices"] == contract["vertices"]
            and world_shape["triangles"] == contract["triangles"],
            "World NIF geometry counts drifted",
        )
        require(
            not world_shape["isSkinned"]
            and world_shape["hasUV"]
            and world_shape["hasNormals"]
            and world_shape["hasTangents"],
            "World NIF skin or vertex-attribute contract drifted",
        )
        require(
            close_vectors(world_shape["minimum"], contract["world_bounds"]["minimum"])
            and close_vectors(world_shape["maximum"], contract["world_bounds"]["maximum"]),
            "World NIF bounds drifted",
        )
        world_block_names = {str(block.get("name", "")) for block in world_blocks}
        require(
            "Materials\\military_backapack\\military_backpack_02.bgsm" in world_block_names,
            "World NIF material path drifted",
        )
        binary = tool_data(await call_tool(session, "nif_binary_info", {"path": str(world)}))
        require(
            contract["minimum_physics_payload_bytes"]
            <= binary["size"]
            <= contract["maximum_physics_payload_bytes"],
            f"World NIF physics payload size is out of bounds: {binary['size']}",
        )
        prefix = bytes.fromhex(binary["prefixHex"])
        require(
            contract["physics_version"].encode("ascii") in prefix,
            "World NIF physics payload lost the expected Havok version marker",
        )

        material_info = tool_data(await call_tool(session, "material_info", {"path": str(material)}))
        material_contract = manifest["material_contract"]
        require(material_info["type"] == material_contract["type"], "Bundled material type drifted")
        require(material_info["version"] == material_contract["version"], "Bundled material version drifted")
        require(material_info["twoSided"] == material_contract["two_sided"], "Bundled material sidedness drifted")
        require(
            material_info["environmentMapping"] == material_contract["environment_mapping"]
            and math.isclose(material_info["environmentMapScale"], material_contract["environment_map_scale"]),
            "Bundled environment-map contract drifted",
        )
        require(
            math.isclose(material_info["specularStrength"], material_contract["specular_strength"])
            and math.isclose(material_info["smoothness"], material_contract["smoothness"]),
            "Bundled material scalar contract drifted",
        )
        require(material_info["textures"] == material_contract["textures"], "Bundled material texture paths drifted")

        scan = tool_data(
            await call_tool(session, "nif_scan", {"directory": str(runtime_root), "report_all": True})
        )
        require(scan["scanned"] == 2 and scan["filesWithErrors"] == 0, "Bundled NIF scan failed")


def decode_hex_string(value: str) -> str:
    try:
        decoded = bytes.fromhex(value)
    except ValueError:
        return value
    return decoded.rstrip(b"\0").decode("utf-8")


def subrecord_values(record: dict[str, Any], subrecord_type: str) -> list[Any]:
    return [
        item.get("value")
        for item in record.get("subrecords", [])
        if item.get("type") == subrecord_type
    ]


async def validate_plugin(plugin: Path, fo4_server: Path) -> None:
    async with open_session(fo4_server) as session:
        info = await call_tool(session, "fo4_plugin_info", {"params": {"plugin_path": str(plugin)}})
        masters = [decode_hex_string(value) for value in info["masters"]]
        require(masters == ["Fallout4.esm"], f"ROCK_Backpack.esp has unexpected masters: {masters}")
        require("bundled Military Backpack 2" in info["description"], "Plugin description lost bundled-asset ownership")
        groups = {item["label"]: item["record_count"] for item in info["group_summary"]}
        require(groups == {"ARMO": 1, "ARMA": 1, "FLST": 1, "CONT": 1, "STAT": 1}, f"Unexpected plugin groups: {groups}")

        census = await call_tool(
            session,
            "fo4_list_record_types",
            {"params": {"plugin_path": str(plugin)}},
        )
        require(
            census["record_type_counts"] == {"ARMO": 1, "ARMA": 1, "FLST": 1, "CONT": 1, "STAT": 1},
            f"Unexpected plugin record census: {census['record_type_counts']}",
        )

        records: dict[str, dict[str, Any]] = {}
        for form_id in ("01000800", "01000802", "01000803", "01000805", "01000806"):
            records[form_id] = await call_tool(
                session,
                "fo4_get_record_by_formid",
                {"params": {"plugin_path": str(plugin), "form_id": form_id}},
            )

        storage = records["01000805"]
        cube = records["01000806"]
        require(cube["type"] == "STAT", "Opening cube must be presentation-only static geometry")
        require(not subrecord_values(cube, "MODL"), "Retired opening cube must have no model")
        require(storage["type"] == "CONT", "Storage backing must be a native container")
        require(subrecord_values(storage, "EDID") == ["ROCKBackpackStorage"], "Storage container identity drifted")
        require(not subrecord_values(storage, "CNTO"), "New bags must start empty")
        require(not subrecord_values(storage, "MODL"), "Hidden storage must not instantiate visible geometry")

        form_list = records["01000800"]
        require(form_list["type"] == "FLST", "Bundled anchor registry is not an FLST")
        require(subrecord_values(form_list, "EDID") == ["ROCK_BackpackAnchorForms"], "Anchor FLST EDID drifted")
        require(subrecord_values(form_list, "LNAM") == ["0x01000803"], "Anchor FLST does not own bundled ARMO")

        armor_addon = records["01000802"]
        require(armor_addon["type"] == "ARMA", "Bundled armor addon is not an ARMA")
        require(subrecord_values(armor_addon, "EDID") == ["MilitaryBackpack02AA"], "Bundled ARMA EDID drifted")
        require(subrecord_values(armor_addon, "BOD2") == ["0x01000000"], "Bundled ARMA slot drifted")
        require(
            [decode_hex_string(value) for value in subrecord_values(armor_addon, "MOD2")]
            == ["military_backapack\\military_backpack_02.nif"],
            "Bundled ARMA male model path drifted",
        )
        require(
            [decode_hex_string(value) for value in subrecord_values(armor_addon, "MOD3")]
            == ["military_backapack\\military_backpack_02.nif"],
            "Bundled ARMA female model path drifted",
        )

        armor = records["01000803"]
        require(armor["type"] == "ARMO", "Bundled inventory item is not an ARMO")
        require(subrecord_values(armor, "EDID") == ["MilitaryBackpack02"], "Bundled ARMO EDID drifted")
        require(subrecord_values(armor, "FULL") == ["Military Backpack 2"], "Bundled ARMO name drifted")
        require(subrecord_values(armor, "BOD2") == ["0x01000000"], "Bundled ARMO slot drifted")
        require(subrecord_values(armor, "MODL") == ["0x01000802"], "Bundled ARMO-to-ARMA link drifted")
        require(
            [decode_hex_string(value) for value in subrecord_values(armor, "MOD2")]
            == ["military_backapack\\military_backpack_02_go.nif"],
            "Bundled ARMO world model path drifted",
        )


async def validate(source_root: Path, runtime_root: Path, plugin: Path) -> None:
    manifest_path = source_root / "assets" / "provenance" / "military_backpack_02.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    expected_files = {str(path).replace("\\", "/") for path in manifest["runtime_files"]}
    require(
        expected_files
        == {
            "Meshes/military_backapack/military_backpack_02.nif",
            "Meshes/military_backapack/military_backpack_02_go.nif",
            "Materials/military_backapack/military_backpack_02.bgsm",
            "Textures/military_backapack/military_backpack_02_d.dds",
            "Textures/military_backapack/military_backpack_02_n.dds",
            "Textures/military_backapack/military_backpack_02_s.dds",
            "Textures/military_backapack/military_backpack_02_m.dds",
        },
        "Bundled runtime-file manifest drifted",
    )
    for relative in expected_files:
        require((runtime_root / relative).is_file(), f"Bundled runtime file is missing: {relative}")
    require(
        manifest["redistribution"]["release_gate"],
        "Bundled asset provenance must retain its redistribution-rights release gate",
    )

    development_root = source_root.parents[1]
    nif_server = Path(os.environ.get("NIF_MCP_SERVER", development_root / "devtools" / "nif_mcp" / "server.py"))
    fo4_server = Path(os.environ.get("FO4_MCP_SERVER", development_root / "devtools" / "fo4_mcp" / "server.py"))

    validate_dds(runtime_root, manifest)
    await validate_nifs(runtime_root, manifest, nif_server)
    await validate_plugin(plugin, fo4_server)


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate ROCK Backpack's bundled Military Backpack 2 package.")
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--runtime-root", type=Path)
    parser.add_argument("--plugin", type=Path, required=True)
    options = parser.parse_args()
    source_root = options.source_root.resolve()
    runtime_root = (
        options.runtime_root.resolve()
        if options.runtime_root is not None
        else source_root / "assets" / "runtime"
    )
    plugin = options.plugin.resolve()
    require(plugin.is_file(), f"Generated ROCK_Backpack.esp is missing: {plugin}")
    asyncio.run(validate(source_root, runtime_root, plugin))
    print("ROCK Backpack bundled Military Backpack 2 validation passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
