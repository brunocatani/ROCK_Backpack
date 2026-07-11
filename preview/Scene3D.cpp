#include "Scene3D.h"
#include <d3dcompiler.h>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace preview
{
    using namespace DirectX;
    using Microsoft::WRL::ComPtr;
    namespace {
        XMVECTOR vector(math::Vec3 p, float w=1) { return XMVectorSet(p.x,p.y,p.z,w); }
        math::Vec3 value(FXMVECTOR v) { XMFLOAT3 p; XMStoreFloat3(&p,v); return {p.x,p.y,p.z}; }
        const char* shader = R"(
cbuffer Camera : register(b0) { row_major float4x4 vp; row_major float4x4 world; float4 options; float4 tint; };
struct Input { float3 p:POSITION; float3 n:NORMAL; float2 uv:TEXCOORD0; };
struct Varying { float4 p:SV_POSITION; float3 n:NORMAL; float2 uv:TEXCOORD0; };
Varying vs(Input i) { Varying o; o.p=mul(mul(float4(i.p,1),world),vp); o.n=mul(float4(i.n,0),world).xyz; o.uv=i.uv; return o; }
Texture2D image : register(t0); SamplerState filtering : register(s0);
float4 ps(Varying i):SV_TARGET {
 float4 color=image.Sample(filtering,i.uv)*tint;
 if(options.y>=0) clip(color.a-options.y);
 float light=options.x>0 ? 1 : .35+.65*saturate(dot(normalize(i.n),normalize(float3(-.4,-.6,1))));
 return float4(lerp(color.rgb*light,float3(.02,1,.08),options.z),color.a);
})";
        struct Constants { XMMATRIX vp, world; XMFLOAT4 options, tint; };
    }
    std::optional<Hit> intersect(const Ray& ray, const math::PanelPose& pose, float width, float height)
    {
        const float denominator=math::dot(ray.direction,pose.front);
        if (std::abs(denominator)<1e-5f) return {};
        const float distance=math::dot(pose.position-ray.origin,pose.front)/denominator;
        if(distance<0) return {};
        const auto local=ray.origin+ray.direction*distance-pose.position;
        const float x=math::dot(local,pose.right)/width+.5f, y=.5f-math::dot(local,pose.up)/height;
        return x>=0 && x<=1 && y>=0 && y<=1 ? std::optional<Hit>{{distance,x,y}} : std::nullopt;
    }
    std::optional<float> intersectBox(const Ray& ray, math::Vec3 center, math::Vec3 half)
    {
        float entryDistance=0, exitDistance=10000;
        const float origin[]{ray.origin.x-center.x,ray.origin.y-center.y,ray.origin.z-center.z};
        const float direction[]{ray.direction.x,ray.direction.y,ray.direction.z}, extent[]{half.x,half.y,half.z};
        for(unsigned axis=0;axis<3;++axis) {
            if(std::abs(direction[axis])<1e-6f) { if(std::abs(origin[axis])>extent[axis]) return {}; continue; }
            float a=(-extent[axis]-origin[axis])/direction[axis], b=(extent[axis]-origin[axis])/direction[axis];
            if(a>b) std::swap(a,b); entryDistance=(std::max)(entryDistance,a); exitDistance=(std::min)(exitDistance,b); if(entryDistance>exitDistance) return {};
        }
        return entryDistance;
    }
    bool Scene3D::initialize(ID3D11Device* device,const std::filesystem::path& assets,unsigned width,unsigned height) try
    {
        _width=width; _height=height;
        if(!loadModel(device,assets/"backpack.glb",_backpack,true) ||
            !loadModel(device,assets/"weapons/hunting-rifle.glb",_weapons[0]) ||
            !loadModel(device,assets/"weapons/combat-rifle.glb",_weapons[1]) ||
            !loadModel(device,assets/"weapons/10mm.glb",_weapons[2])) return false;
        const auto buffer=[&](unsigned bytes, unsigned bind, const void* initial, ComPtr<ID3D11Buffer>& out) {
            D3D11_BUFFER_DESC desc{}; desc.ByteWidth=bytes; desc.BindFlags=bind; desc.Usage=D3D11_USAGE_DEFAULT;
            D3D11_SUBRESOURCE_DATA input{}; input.pSysMem=initial;
            return SUCCEEDED(device->CreateBuffer(&desc,initial?&input:nullptr,&out));
        };
        if(!buffer(6*sizeof(Vertex),D3D11_BIND_VERTEX_BUFFER,nullptr,_quad) ||
            !buffer(sizeof(Constants),D3D11_BIND_CONSTANT_BUFFER,nullptr,_constants)) return false;
        ComPtr<ID3DBlob> vs,ps,error;
        if(FAILED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&error)) ||
            FAILED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&error))) return false;
        if(FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&_vs)) ||
            FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&_ps))) return false;
        const D3D11_INPUT_ELEMENT_DESC layout[]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
        if(FAILED(device->CreateInputLayout(layout,3,vs->GetBufferPointer(),vs->GetBufferSize(),&_layout))) return false;
        if (!resize(device,width,height)) return false;
        D3D11_DEPTH_STENCIL_DESC z{}; z.DepthEnable=TRUE; z.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; z.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
        if(FAILED(device->CreateDepthStencilState(&z,&_depthState))) return false;
        z.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        if(FAILED(device->CreateDepthStencilState(&z,&_overlayDepth))) return false;
        D3D11_BLEND_DESC blend{}; auto& target=blend.RenderTarget[0]; target.BlendEnable=TRUE;
        target.SrcBlend=target.SrcBlendAlpha=D3D11_BLEND_ONE;
        target.DestBlend=target.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp=target.BlendOpAlpha=D3D11_BLEND_OP_ADD; target.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(FAILED(device->CreateBlendState(&blend,&_overlayBlend))) return false;
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE;
        if(FAILED(device->CreateRasterizerState(&raster,&_rasterizer))) return false;
        D3D11_SAMPLER_DESC sampling{}; sampling.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU=sampling.AddressV=sampling.AddressW=D3D11_TEXTURE_ADDRESS_WRAP; sampling.MaxLOD=D3D11_FLOAT32_MAX;
        if(FAILED(device->CreateSamplerState(&sampling,&_sampler))) return false;
        camera(.25f,.15f,140); return true;
    } catch(...) { return false; }
    bool Scene3D::resize(ID3D11Device* device,unsigned width,unsigned height)
    {
        if (!device || !width || !height) return false;
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT; desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11DepthStencilView> depth;
        if (FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) || FAILED(device->CreateDepthStencilView(texture.Get(),nullptr,&depth))) return false;
        _depth=std::move(depth); _width=width; _height=height; return true;
    }
    void Scene3D::camera(float yaw,float pitch,float distance,bool panelOnLeft)
    {
        const auto target=XMVectorSet(panelOnLeft?14.f:-14.f,-16,8,1);
        const auto eye=target+XMVectorSet(std::sin(yaw)*std::cos(pitch)*distance,-std::sin(pitch)*distance,std::cos(yaw)*std::cos(pitch)*distance,0);
        _viewProjection=XMMatrixLookAtRH(eye,target,XMVectorSet(0,-1,0,0))*XMMatrixPerspectiveFovRH(.55f,float(_width)/_height,.5f,1000.f);
        _inverse=XMMatrixInverse(nullptr,_viewProjection);
    }
    math::Vec3 Scene3D::screen(math::Vec3 point) const
    { const auto p=value(XMVector3TransformCoord(vector(point),_viewProjection)); return {(p.x+1)*.5f*_width,(1-p.y)*.5f*_height,p.z}; }
    Ray Scene3D::ray(float x,float y) const
    {
        const float nx=x*2/_width-1,ny=1-y*2/_height;
        const auto a=XMVector3TransformCoord(XMVectorSet(nx,ny,0,1),_inverse), b=XMVector3TransformCoord(XMVectorSet(nx,ny,1,1),_inverse);
        return {value(a),value(XMVector3Normalize(b-a))};
    }
    void Scene3D::begin(ID3D11DeviceContext* c,ID3D11RenderTargetView* target)
    {
        c->OMSetRenderTargets(1,&target,_depth.Get()); c->ClearDepthStencilView(_depth.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
        c->OMSetDepthStencilState(_depthState.Get(),0); c->OMSetBlendState(nullptr,nullptr,~0u);
        c->RSSetState(_rasterizer.Get()); const D3D11_VIEWPORT viewport{0,0,float(_width),float(_height),0,1}; c->RSSetViewports(1,&viewport);
        c->IASetInputLayout(_layout.Get()); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(_vs.Get(),nullptr,0); c->PSSetShader(_ps.Get(),nullptr,0);
        auto* constant=_constants.Get(); c->VSSetConstantBuffers(0,1,&constant); c->PSSetConstantBuffers(0,1,&constant);
        auto* sampler=_sampler.Get(); c->PSSetSamplers(0,1,&sampler);
    }
    void Scene3D::drawModel(ID3D11DeviceContext* c,const Model& model,const XMMATRIX& world,bool selected)
    {
        for(const auto& mesh:model.meshes) {
            const Constants constants{_viewProjection,world,{0,mesh.cutoff,selected?.55f:0.f,0},mesh.color};
            c->UpdateSubresource(_constants.Get(),0,nullptr,&constants,0,0);
            unsigned stride=sizeof(Vertex),offset=0; auto* vertices=mesh.vertices.Get();
            c->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
            c->IASetIndexBuffer(mesh.indices.Get(),DXGI_FORMAT_R32_UINT,0);
            auto* image=model.textures.at(mesh.texture).view.Get(); c->PSSetShaderResources(0,1,&image);
            if(mesh.blend) c->OMSetBlendState(_overlayBlend.Get(),nullptr,~0u);
            c->DrawIndexed(mesh.count,0,0);
            if(mesh.blend) c->OMSetBlendState(nullptr,nullptr,~0u);
        }
    }
    void Scene3D::backpack(ID3D11DeviceContext* c)
    { drawModel(c,_backpack,XMMatrixIdentity()); }
    void Scene3D::weapon(ID3D11DeviceContext* c,rock_backpack::storage::Location location,const rock_backpack::zones::Layout& placement,bool selected)
    {
        const unsigned slot=unsigned(location)-1;
        if(slot>=_weapons.size()) return;
        const auto position=rock_backpack::zones::mount(location,placement);
        const auto r=rock_backpack::zones::visualRotation(location,placement);
        const auto rotation=XMMatrixSet(r[0],r[1],r[2],0,r[3],r[4],r[5],0,r[6],r[7],r[8],0,0,0,0,1);
        drawModel(c,_weapons[slot],rotation*XMMatrixTranslation(position.x,position.y,position.z),selected);
    }
    void Scene3D::panel(ID3D11DeviceContext* c,ID3D11ShaderResourceView* image,const math::PanelPose& pose,float width,float height,bool transparent)
    {
        const auto r=pose.right*(width*.5f), u=pose.up*(height*.5f);
        const math::Vec3 points[]{pose.position-r+u,pose.position+r+u,pose.position+r-u,pose.position-r-u};
        const float uv[4][2]{{0,0},{1,0},{1,1},{0,1}}; const unsigned order[]{0,1,2,0,2,3};
        Vertex vertices[6]{};
        for(unsigned i=0;i<6;++i) { const auto p=points[order[i]]; vertices[i]={{p.x,p.y,p.z},{pose.front.x,pose.front.y,pose.front.z},{uv[order[i]][0],uv[order[i]][1]}}; }
        c->UpdateSubresource(_quad.Get(),0,nullptr,vertices,0,0);
        const Constants constants{_viewProjection,XMMatrixIdentity(),{1,-1,0,0},{1,1,1,1}}; c->UpdateSubresource(_constants.Get(),0,nullptr,&constants,0,0);
        unsigned stride=sizeof(Vertex),offset=0; auto* buffer=_quad.Get(); c->IASetVertexBuffers(0,1,&buffer,&stride,&offset);
        if(transparent) { c->OMSetDepthStencilState(_overlayDepth.Get(),0); c->OMSetBlendState(_overlayBlend.Get(),nullptr,~0u); }
        c->PSSetShaderResources(0,1,&image); c->Draw(6,0);
        if(transparent) { c->OMSetDepthStencilState(_depthState.Get(),0); c->OMSetBlendState(nullptr,nullptr,~0u); }
    }
}
