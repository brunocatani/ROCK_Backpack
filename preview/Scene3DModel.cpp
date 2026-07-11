#include "Scene3D.h"
#include <nlohmann/json.hpp>
#include <wincodec.h>
#include <fstream>
#include <functional>
#include <cstring>
#include <cmath>
#include <unordered_map>

namespace preview
{
    using namespace DirectX;
    using Microsoft::WRL::ComPtr;
    using Json = nlohmann::json;

    bool Scene3D::loadModel(ID3D11Device* device,const std::filesystem::path& path,Model& model,bool preparedBackpack)
    {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        const auto size=file.tellg(); if(size<28 || size>128*1024*1024) return false;
        std::vector<char> bytes(static_cast<std::size_t>(size)); file.seekg(0);
        if(!file.read(bytes.data(),bytes.size())) return false;
        const auto word=[&](std::size_t offset) {
            std::uint32_t n{}; if(offset+4>bytes.size()) throw std::runtime_error("GLB bounds");
            std::memcpy(&n,bytes.data()+offset,4); return n;
        };
        if(word(0)!=0x46546c67 || word(4)!=2 || word(8)!=bytes.size() || word(16)!=0x4e4f534a) return false;
        const std::size_t jsonSize=word(12), binary=28+jsonSize;
        if(binary>bytes.size() || word(24+jsonSize)!=0x004e4942 || word(20+jsonSize)!=bytes.size()-binary) return false;
        const auto doc=Json::parse(bytes.data()+20,bytes.data()+20+jsonSize);
        if(doc.contains("skins") && !doc.at("skins").empty()) return false;
        const auto viewBytes=[&](unsigned id) -> std::pair<const char*,std::size_t> {
            const auto& view=doc.at("bufferViews").at(id);
            const std::size_t offset=view.value("byteOffset",0u), length=view.at("byteLength");
            if(view.at("buffer")!=0 || offset>bytes.size()-binary || length>bytes.size()-binary-offset)
                throw std::runtime_error("GLB view bounds");
            return {bytes.data()+binary+offset,length};
        };
        const auto accessorBytes=[&](unsigned id,unsigned element,const char* type) {
            const auto& a=doc.at("accessors").at(id);
            if(a.contains("sparse") || a.at("type")!=type) throw std::runtime_error("Unsupported accessor");
            const auto viewID=a.at("bufferView").get<unsigned>();
            const auto [data,length]=viewBytes(viewID);
            const std::size_t count=a.at("count"), offset=a.value("byteOffset",0u);
            const std::size_t stride=doc.at("bufferViews").at(viewID).value("byteStride",element);
            if(!count || count>3000000 || stride<element || offset>length || (count-1)*stride+element>length-offset)
                throw std::runtime_error("GLB accessor bounds");
            std::vector<char> out(count*element);
            for(std::size_t i=0;i<count;++i) std::memcpy(out.data()+i*element,data+offset+i*stride,element);
            return out;
        };
        const auto floats=[&](unsigned id,unsigned components,const char* type) {
            if(doc.at("accessors").at(id).at("componentType")!=5126) throw std::runtime_error("Expected float vertices");
            const auto raw=accessorBytes(id,components*4,type);
            std::vector<float> values(raw.size()/4); std::memcpy(values.data(),raw.data(),raw.size());
            for(float f:values) if(!std::isfinite(f)) throw std::runtime_error("Nonfinite vertex data");
            return values;
        };
        const auto buffer=[&](unsigned size,unsigned bind,const void* data,ComPtr<ID3D11Buffer>& out) {
            D3D11_BUFFER_DESC desc{}; desc.ByteWidth=size; desc.BindFlags=bind; desc.Usage=D3D11_USAGE_IMMUTABLE;
            D3D11_SUBRESOURCE_DATA initial{data,0,0};
            if(FAILED(device->CreateBuffer(&desc,&initial,&out))) throw std::runtime_error("Model buffer creation failed");
        };
        ComPtr<IWICImagingFactory> wic;
        if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)))) return false;
        std::unordered_map<unsigned,unsigned> textureIndices;
        rock_backpack::ui::ItemImage white;
        if(!white.upload(device,{1,1,4,DXGI_FORMAT_R8G8B8A8_UNORM,{255,255,255,255}})) return false;
        model.textures.push_back(std::move(white));
        if(preparedBackpack) {
            rock_backpack::ui::ItemImage image;
            if(!image.load(device,path.parent_path()/"backpack.png")) return false;
            model.textures.push_back(std::move(image));
        }
        const auto texture=[&](unsigned textureID) {
            const unsigned imageID=doc.at("textures").at(textureID).at("source");
            if(const auto found=textureIndices.find(imageID);found!=textureIndices.end()) return found->second;
            const auto& image=doc.at("images").at(imageID);
            if(image.at("mimeType")!="image/png") throw std::runtime_error("Expected embedded PNG");
            const auto [data,length]=viewBytes(image.at("bufferView"));
            ComPtr<IWICStream> stream; ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> convert;
            if(FAILED(wic->CreateStream(&stream)) ||
               FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(data)),DWORD(length))) ||
               FAILED(wic->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder)) ||
               FAILED(decoder->GetFrame(0,&frame)) || FAILED(wic->CreateFormatConverter(&convert)) ||
               FAILED(convert->Initialize(frame.Get(),GUID_WICPixelFormat32bppPRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))
                throw std::runtime_error("Model PNG decode failed");
            rock_backpack::ui::ImagePixels pixels;
            if(FAILED(convert->GetSize(&pixels.width,&pixels.height)) || !pixels.width || !pixels.height || pixels.width>4096 || pixels.height>4096)
                throw std::runtime_error("Model PNG dimensions");
            pixels.stride=pixels.width*4; pixels.format=DXGI_FORMAT_R8G8B8A8_UNORM;
            pixels.bytes.resize(std::size_t(pixels.stride)*pixels.height);
            if(FAILED(convert->CopyPixels(nullptr,pixels.stride,UINT(pixels.bytes.size()),pixels.bytes.data())))
                throw std::runtime_error("Model PNG copy failed");
            rock_backpack::ui::ItemImage loaded;
            if(!loaded.upload(device,pixels)) throw std::runtime_error("Model texture upload failed");
            const auto index=unsigned(model.textures.size()); model.textures.push_back(std::move(loaded));
            textureIndices.emplace(imageID,index); return index;
        };
        const auto addMesh=[&](unsigned meshID,const XMMATRIX& transform) {
            const auto normalTransform=XMMatrixTranspose(XMMatrixInverse(nullptr,transform));
            for(const auto& primitive:doc.at("meshes").at(meshID).at("primitives")) {
                if(primitive.value("mode",4)!=4 || primitive.contains("targets")) throw std::runtime_error("Static triangles required");
                const auto& attrs=primitive.at("attributes");
                const auto positions=floats(attrs.at("POSITION"),3,"VEC3");
                const auto normals=floats(attrs.at("NORMAL"),3,"VEC3");
                const auto uv=floats(attrs.at("TEXCOORD_0"),2,"VEC2");
                if(positions.size()!=normals.size() || positions.size()/3!=uv.size()/2) throw std::runtime_error("Vertex attribute count mismatch");
                std::vector<Vertex> vertices(positions.size()/3);
                for(std::size_t i=0;i<vertices.size();++i) {
                    XMFLOAT3 p,n;
                    XMStoreFloat3(&p,XMVector3TransformCoord(XMVectorSet(positions[i*3],positions[i*3+1],positions[i*3+2],1),transform)*69.99125f);
                    XMStoreFloat3(&n,XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(normals[i*3],normals[i*3+1],normals[i*3+2],0),normalTransform)));
                    vertices[i]={{p.x,p.y,p.z},{n.x,n.y,n.z},{uv[i*2],uv[i*2+1]}};
                }
                const unsigned indexID=primitive.at("indices"), component=doc.at("accessors").at(indexID).at("componentType");
                const unsigned element=component==5123?2:component==5125?4:0;
                if(!element) throw std::runtime_error("Unsupported triangle indices");
                const auto raw=accessorBytes(indexID,element,"SCALAR");
                std::vector<std::uint32_t> indices(raw.size()/element);
                if(indices.size()%3) throw std::runtime_error("Incomplete triangles");
                for(std::size_t i=0;i<indices.size();++i) {
                    std::memcpy(&indices[i],raw.data()+i*element,element);
                    if(indices[i]>=vertices.size()) throw std::runtime_error("Triangle index out of bounds");
                }
                Mesh mesh; mesh.count=unsigned(indices.size());
                if(preparedBackpack) mesh.texture=1;
                else if(primitive.contains("material")) {
                    const auto& material=doc.at("materials").at(primitive.at("material").get<unsigned>());
                    const auto& pbr=material.value("pbrMetallicRoughness",Json::object());
                    if(pbr.contains("baseColorTexture")) mesh.texture=texture(pbr.at("baseColorTexture").at("index"));
                    if(pbr.contains("baseColorFactor")) {
                        const auto& c=pbr.at("baseColorFactor"); mesh.color={c.at(0),c.at(1),c.at(2),c.at(3)};
                    }
                    const auto alpha=material.value("alphaMode",std::string("OPAQUE"));
                    if(alpha=="MASK") mesh.cutoff=material.value("alphaCutoff",.5f);
                    mesh.blend=alpha=="BLEND";
                }
                buffer(unsigned(vertices.size()*sizeof(Vertex)),D3D11_BIND_VERTEX_BUFFER,vertices.data(),mesh.vertices);
                buffer(unsigned(indices.size()*4),D3D11_BIND_INDEX_BUFFER,indices.data(),mesh.indices);
                model.meshes.push_back(std::move(mesh));
            }
        };
        // The backpack source's presentation node is not a NIF-local transform.
        // Weapon assets instead contain the assembled NIF hierarchy explicitly.
        if(preparedBackpack) addMesh(0,XMMatrixIdentity());
        else {
            const auto& nodes=doc.at("nodes"); if(nodes.size()>4096) return false;
            std::vector<bool> visited(nodes.size());
            std::function<void(unsigned,const XMMATRIX&,unsigned)> visit;
            visit=[&](unsigned id,const XMMATRIX& parent,unsigned depth) {
                if(id>=nodes.size() || visited[id] || depth>128) throw std::runtime_error("Invalid model hierarchy");
                visited[id]=true; const auto& node=nodes.at(id); auto local=XMMatrixIdentity();
                if(node.contains("matrix")) {
                    XMFLOAT4X4 m; for(unsigned i=0;i<16;++i) m.m[i/4][i%4]=node.at("matrix").at(i);
                    local=XMLoadFloat4x4(&m);
                } else {
                    const auto s=node.value("scale",std::vector<float>{1,1,1});
                    const auto r=node.value("rotation",std::vector<float>{0,0,0,1});
                    const auto t=node.value("translation",std::vector<float>{0,0,0});
                    local=XMMatrixScaling(s.at(0),s.at(1),s.at(2))*XMMatrixRotationQuaternion(XMVectorSet(r.at(0),r.at(1),r.at(2),r.at(3)))*XMMatrixTranslation(t.at(0),t.at(1),t.at(2));
                }
                const auto transform=local*parent;
                if(node.contains("mesh")) addMesh(node.at("mesh"),transform);
                for(unsigned child:node.value("children",std::vector<unsigned>{})) visit(child,transform,depth+1);
            };
            for(unsigned root:doc.at("scenes").at(doc.value("scene",0)).at("nodes")) visit(root,XMMatrixIdentity(),0);
        }
        return !model.meshes.empty();
    }
}
