#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>

// Uncalibrated book-local reference, derived from submitted frame2069 geometry.
// Existing page calibration is applied AFTER this matrix, without changing it.
static const float kJournalBookLocalPage[16] = {
	0.00023406987f, -1.07521182e-05f, -0.171876013f, 0.079085879f,
	-2.7451626e-05f, -2.5249532e-05f, 1.13511157f, -0.0617124066f,
	1.075199e-05f, -0.000235869898f, 0.158091277f, -0.10220369f,
	0.0f, 0.0f, 0.0f, 1.0f
};
static const char kJournalBookVSSource[] = R"hlsl(
cbuffer PageLocal : register(b11) { row_major float4x4 page; };
cbuffer PageBones : register(b12) { float4 bones[241]; };
cbuffer PageCamera : register(b13) { float4 camera[22]; };
StructuredBuffer<float4> instanceRows : register(t15);
struct In { float4 position : POSITION0; float4 color : COLOR0; float2 uv : TEXCOORD0; };
struct Out { float4 position : SV_Position; float2 uv : TEXCOORD0; float4 color : TEXCOORD1; };
Out main(In v) {
	Out o;
	float4 local = mul(page, v.position);
	// Bone base3 => float4 rows4..6, after the step/header row.
	float4 native = float4(dot(bones[4], local), dot(bones[5], local), dot(bones[6], local), 1);
	// Match the book shader: instance row3 contains metadata, not position.
	float4 view = float4(dot(instanceRows[0], native), dot(instanceRows[1], native), dot(instanceRows[2], native), 1);
	o.position = float4(dot(camera[6], view), dot(camera[7], view), dot(camera[8], view), dot(camera[9], view));
	o.uv = v.uv; o.color = v.color; return o;
}
)hlsl";

// Same-frame GPU snapshots of the submitted book. No staging/CPU readbacks.
// New bindings belong only to the text draw, and Restore returns all of them.
class JournalBookPage {
	template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
	struct Eye {
		Ptr<ID3D11Buffer> instance, bones, camera;
		Ptr<ID3D11ShaderResourceView> instanceView;
		unsigned frame = ~0u;
	};
	Eye eyes[2];
	Ptr<ID3D11Device> device;
	Ptr<ID3D11DeviceContext> sourceContext;
	Ptr<ID3D11VertexShader> shader, savedShader;
	Ptr<ID3D11Buffer> local, savedCB[3];
	Ptr<ID3D11ShaderResourceView> savedView;
	bool attempted = false, active = false;
	bool CloneCB(ID3D11Buffer *source, Ptr<ID3D11Buffer>& target) {
		D3D11_BUFFER_DESC desc = {}; source->GetDesc(&desc);
		if (target) {
			D3D11_BUFFER_DESC prior = {}; target->GetDesc(&prior);
			if (prior.ByteWidth == desc.ByteWidth) return true;
			target.Reset();
		}
		desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = desc.MiscFlags = desc.StructureByteStride = 0;
		return SUCCEEDED(device->CreateBuffer(&desc, NULL, target.GetAddressOf()));
	}
	bool Ensure(ID3D11DeviceContext *ctx) {
		Ptr<ID3D11Device> current; ctx->GetDevice(current.GetAddressOf());
		if (device.Get() != current.Get()) {
			if (active) return false;
			eyes[0] = Eye(); eyes[1] = Eye(); shader.Reset(); local.Reset();
			sourceContext.Reset(); device = current; attempted = false;
		}
		if (attempted) return shader && local;
		attempted = true;
		Ptr<ID3DBlob> code, errors;
		HRESULT hr = D3DCompile(kJournalBookVSSource, std::strlen(kJournalBookVSSource),
			"journal_book_page", NULL, NULL, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
			0, code.GetAddressOf(), errors.GetAddressOf());
		if (FAILED(hr)) return false;
		if (FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
			NULL, shader.GetAddressOf()))) return false;
		D3D11_BUFFER_DESC desc = {}; desc.ByteWidth = 64;
		desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		return SUCCEEDED(device->CreateBuffer(&desc, NULL, local.GetAddressOf()));
	}
public:
	bool Ready(ID3D11DeviceContext *ctx, unsigned frame) const {
		return sourceContext.Get() == ctx && shader && local
			&& eyes[0].frame == frame && eyes[1].frame == frame;
	}
	bool Capture(ID3D11DeviceContext *ctx, unsigned frame, unsigned eye) {
		eye &= 1;
		if (!Ensure(ctx)) return false;
		if (sourceContext.Get() != ctx) {
			eyes[0].frame = eyes[1].frame = ~0u; sourceContext = ctx;
		}
		Eye &e = eyes[eye]; e.frame = ~0u;
		Ptr<ID3D11Buffer> instance, bones, camera;
		UINT stride = 0, offset = 0;
		ctx->IAGetVertexBuffers(1, 1, instance.GetAddressOf(), &stride, &offset);
		ctx->VSGetConstantBuffers(8, 1, bones.GetAddressOf());
		ctx->VSGetConstantBuffers(1, 1, camera.GetAddressOf());
		if (!instance || !bones || !camera || stride != 64) return false;
		D3D11_BUFFER_DESC id = {}, bd = {}, cd = {};
		instance->GetDesc(&id); bones->GetDesc(&bd); camera->GetDesc(&cd);
		if (offset > id.ByteWidth || id.ByteWidth - offset < 64
			|| bd.ByteWidth < 3856 || bd.ByteWidth > 4096 || cd.ByteWidth < 352) return false;
		if (!e.instance) {
			D3D11_BUFFER_DESC desc = {}; desc.ByteWidth = 64; desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = 16;
			if (FAILED(device->CreateBuffer(&desc, NULL, e.instance.GetAddressOf()))) return false;
		}
		if (!e.instanceView) {
			D3D11_SHADER_RESOURCE_VIEW_DESC desc = {}; desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; desc.Buffer.NumElements = 4;
			if (FAILED(device->CreateShaderResourceView(e.instance.Get(), &desc, e.instanceView.GetAddressOf()))) return false;
		}
		if (!CloneCB(bones.Get(), e.bones) || !CloneCB(camera.Get(), e.camera)) return false;
		const D3D11_BOX box = { offset, 0, 0, offset + 64, 1, 1 };
		ctx->CopySubresourceRegion(e.instance.Get(), 0, 0, 0, 0, instance.Get(), 0, &box);
		ctx->CopyResource(e.bones.Get(), bones.Get()); ctx->CopyResource(e.camera.Get(), camera.Get());
		e.frame = frame; return true;
	}
	bool Bind(ID3D11DeviceContext *ctx, unsigned frame, unsigned eye, const float page[16]) {
		if (!Ready(ctx, frame)) return false;
		if (!active) {
			ctx->VSGetShader(savedShader.GetAddressOf(), NULL, NULL);
			ID3D11Buffer *buffers[3] = {}; ctx->VSGetConstantBuffers(11, 3, buffers);
			for (unsigned i = 0; i < 3; ++i) savedCB[i].Attach(buffers[i]);
			ctx->VSGetShaderResources(15, 1, savedView.GetAddressOf()); active = true;
		}
		ctx->UpdateSubresource(local.Get(), 0, NULL, page, 0, 0);
		ID3D11Buffer *buffers[] = { local.Get(), eyes[eye&1].bones.Get(), eyes[eye&1].camera.Get() };
		ID3D11ShaderResourceView *view = eyes[eye&1].instanceView.Get();
		ctx->VSSetConstantBuffers(11, 3, buffers); ctx->VSSetShaderResources(15, 1, &view);
		ctx->VSSetShader(shader.Get(), NULL, 0); return true;
	}
	void Restore(ID3D11DeviceContext *ctx) {
		if (!active) return;
		ID3D11Buffer *buffers[] = { savedCB[0].Get(), savedCB[1].Get(), savedCB[2].Get() };
		ID3D11ShaderResourceView *view = savedView.Get();
		ctx->VSSetShader(savedShader.Get(), NULL, 0); ctx->VSSetConstantBuffers(11, 3, buffers);
		ctx->VSSetShaderResources(15, 1, &view);
		savedShader.Reset(); savedView.Reset(); for (auto &buffer : savedCB) buffer.Reset(); active = false;
	}
};
