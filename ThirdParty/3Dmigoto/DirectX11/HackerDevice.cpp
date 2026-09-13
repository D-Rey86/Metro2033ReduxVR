// Wrapper for the ID3D11Device.
// This gives us access to every D3D11 call for a device, and override the pieces needed.

// Object			OS				D3D11 version	Feature level
// ID3D11Device		Win7			11.0			11.0
// ID3D11Device1	Platform update	11.1			11.1
// ID3D11Device2	Win8.1			11.2
// ID3D11Device3	Win10			11.3
// ID3D11Device4					11.4

// Include before util.h (or any header that includes util.h) to get pretty
// version of LockResourceCreationMode:
#include "lock.h"

#include "HackerDevice.h"
#include "HookedDevice.h"
#include "FrameAnalysis.h"
#include "DLLMainHook.h"

#include <D3Dcompiler.h>
#include <codecvt>
#include <cstring>

#include "nvapi.h"
#include "log.h"
#include "util.h"
#include "shader.h"
#include "DecompileHLSL.h"
#include "HackerContext.h"
#include "HackerDXGI.h"
#include "VRPose.h"
#include "VRMenu.h"
#include "StereoTwin.h"
#include "StereoSinglePass.h"

#include "D3D11Wrapper.h"
#include "SpriteFont.h"
#include "D3D_Shaders\stdafx.h"
#include "ResourceHash.h"
#include "ShaderRegex.h"
#include "CommandList.h"
#include "Hunting.h"

// A map to look up the HackerDevice from an IUnknown. The reason for using an
// IUnknown as the key is that an ID3D11Device and IDXGIDevice are actually two
// different interfaces to the same object, which means that QueryInterface()
// can be used to traverse between them. They do not however inherit from each
// other and using C style casting between them will not work. We need to be
// able to find our HackerDevice from either interface, including hooked
// versions, so we need to find a common handle to use as a key between them.
//
// We could probably get away with calling QueryInterface(IID_ID3D11Device),
// however COM does not guarantee that pointers returned to the same interface
// will be identical (they can be "tear-off" interfaces independently
// refcounted from the main object and potentially from each other, or they
// could just be implemented in the main object with shared refcounting - we
// shouldn't assume which is in use for a given interface, because it's an
// implementation detail that could change).
//
// COM does however offer a guarantee that calling QueryInterface(IID_IUnknown)
// will return a consistent pointer for all interfaces to the same object, so
// we can safely use that. Note that it is important we use QueryInterface() to
// get this pointer, not C/C++ style casting, as using the later on pointers is
// really just a noop, and will return the same pointer we pass into them.
//
// In practice we see the consequences of ID3D11Device and IDXGIDevice being
// the same object in UE4 games (in all versions since the source was
// released), that call ID3D11Device::QueryInterface(IID_IDXGIDevice), and pass
// the returned pointer to CreateSwapChain. Since we no longer wrap the
// IDXGIDevice interface we can't directly get back to our HackerDevice, and so
// we use this map to look it up instead.
//
// Note that there is a real possibility that a game could then call
// QueryInterface on the IDXGIDevice to get back to the ID3D11Device, but since
// we aren't intercepting that call it would get the real ID3D11Device and
// could effectively unhook us. If that becomes a problem in practice, we will
// have to rethink this - either bringing back our IDXGIDevice wrapper (or a
// simplified version of it, that respects the relationship to ID3D11Device),
// hooking the QueryInterface on the returned object (but beware that DX itself
// could potentially then call into us), or denying the game from recieving the
// IDXGIDevice in the first place and hoping that it has a fallback path (it
// won't).
typedef std::unordered_map<IUnknown *, HackerDevice *> DeviceMap;
static DeviceMap device_map;
static void InstallMetroResolutionSetterDiagnostic();
static const bool kResolutionDiagnosticsEnabled = false;

// This will look up a HackerDevice corresponding to some unknown device object
// (ID3D11Device*, IDXGIDevice*, etc). It will bump the refcount on the
// returned interface.
HackerDevice* lookup_hacker_device(IUnknown *unknown)
{
	HackerDevice *ret = NULL;
	IUnknown *real_unknown = NULL;
	IDXGIObject *dxgi_obj = NULL;
	DeviceMap::iterator i;

	// First, check if this is already a HackerDevice. This is a fast path,
	// but is also kind of important in case we ever make
	// HackerDevice::QueryInterface(IID_IUnknown) return the HackerDevice
	// (which is conceivable we might need to do some day if we find a game
	// that uses that to get back to the real DX interfaces), since doing
	// so would break the COM guarantee we rely on below.
	//
	// HookedDevices will also follow this path, since they hook
	// QueryInterface and will return the corresponding HackerDevice here,
	// but even if they didn't they would still be looked up in the map, so
	// either way we no longer need to call lookup_hooked_device.
	if (SUCCEEDED(unknown->QueryInterface(IID_HackerDevice, (void**)&ret))) {
		LogInfo("lookup_hacker_device(%p): Supports HackerDevice\n", unknown);
		return ret;
	}

	// We've been passed an IUnknown, but it may not have been gained via
	// QueryInterface (and for convenience it's probably just been cast
	// with C style casting), but we need the real IUnknown pointer with
	// the COM guarantee that it will match for all interfaces of the same
	// object, so we call QueryInterface on it again to get this:
	if (FAILED(unknown->QueryInterface(IID_IUnknown, (void**)&real_unknown))) {
		// ... ehh, what? Shouldn't happen. Fatal.
		LogInfo("lookup_hacker_device: QueryInterface(IID_Unknown) failed\n");
		DoubleBeepExit();
	}

	EnterCriticalSectionPretty(&G->mCriticalSection);
	i = device_map.find(real_unknown);
	if (i != device_map.end()) {
		ret = i->second;
		ret->AddRef();
	}
	LeaveCriticalSection(&G->mCriticalSection);

	real_unknown->Release();

	if (!ret) {
		// Either not a d3d11 device, or something has handed us an
		// unwrapped device *and also* violated the COM identity rule.
		// This is known to happen with ReShade in certain games (e.g.
		// Resident Evil 2), though it appears that DirectX itself
		// violates the COM identity rule in some cases (Device4/5 +
		// Multithread interfaces)
		//
		// We have a few more tricks up our sleeve to try to find our
		// HackerDevice - the first would be to look up the
		// ID3D11Device interface and use it as a key to look up our
		// device_map. That would work for the ReShade case as it
		// stands today, but is not foolproof since e.g. that device
		// may itself be wrapped. We could try other interfaces that
		// may not be wrapped or use them to find the DirectX COM
		// identity and look up the map by that, but again if there is
		// a third party tool messing with us than all bets are off.
		//
		// Instead, let's try to do this in a fool proof manner that
		// will hopefully be impervious to anything that a third party
		// tool may do. When we created the device we stored a pointer
		// to our HackerDevice in the device's private data that we
		// should be able to retrieve. We can access that from either
		// the D3D11Device interface, or the DXGIObject interface. For
		// the sake of a possible future DX12 port (or DX10 backport)
		// I'm using the DXGI interface that's supposed to be version
		// agnostic. XXX: It might be worthwhile considering dropping
		// the above device_map lookup which relies on the COM identity
		// rule in favour of this, since we expect this to always work:
		if (SUCCEEDED(unknown->QueryInterface(IID_IDXGIObject, (void**)&dxgi_obj))) {
			UINT size;
			if (SUCCEEDED(dxgi_obj->GetPrivateData(IID_HackerDevice, &size, &ret))) {
				LogInfo("Notice: Unwrapped device and COM Identity violation, Found HackerDevice via GetPrivateData strategy\n");
				ret->AddRef();
			}
			dxgi_obj->Release();
		}
	}

	LogInfo("lookup_hacker_device(%p) IUnknown: %p HackerDevice: %p\n",
			unknown, real_unknown, ret);

	return ret;
}

static IUnknown* register_hacker_device(HackerDevice *hacker_device)
{
	IUnknown *real_unknown = NULL;

	// As above, our key is the real IUnknown gained through QueryInterface
	if (FAILED(hacker_device->GetPassThroughOrigDevice1()->QueryInterface(IID_IUnknown, (void**)&real_unknown))) {
		LogInfo("register_hacker_device: QueryInterface(IID_Unknown) failed\n");
		DoubleBeepExit();
	}

	LogInfo("register_hacker_device: Registering IUnknown: %p -> HackerDevice: %p\n",
			real_unknown, hacker_device);

	EnterCriticalSectionPretty(&G->mCriticalSection);
	device_map[real_unknown] = hacker_device;
	LeaveCriticalSection(&G->mCriticalSection);

	real_unknown->Release();

	// We return the IUnknown for convenience, since the HackerDevice needs
	// to store it so it can later unregister it after the real Device has
	// been Released and we will no longer be able to find it through
	// QueryInterface. We have dropped the refcount on this - dangerous I
	// know, but otherwise it will never be released.
	return real_unknown;
}

static void unregister_hacker_device(HackerDevice *hacker_device)
{
	IUnknown *real_unknown;
	DeviceMap::iterator i;

	// We can't do a QueryInterface() here to get the real IUnknown,
	// because the device has already been released. Instead, we use the
	// real IUnknown pointer saved in the HackerDevice.
	real_unknown = hacker_device->GetIUnknown();

	// I have some concerns about our HackerDevice refcounting, and suspect
	// there are cases where our HackerDevice wrapper won't be released
	// along with the wrapped object (because COM refcounting is
	// complicated, and there are several different models it could be
	// using, and our wrapper relies on the ID3D11Device::Release as being
	// the final Release, and not say, IDXGIDevice::Release), and there is
	// a small chance that the handle could have already been reused.
	//
	// Now there is an obvious race here that this critical section should
	// really be held around the original Release() call as well in case it
	// gets reused by another thread before we get here, but I think we
	// have bigger issues than just that, and it doesn't really matter
	// anyway if it does hit, so I'd rather not expand that lock if we
	// don't need to. Just detect if the handle has been reused and print
	// out a message - we know that the HackerDevice won't have been reused
	// yet, so this is safe.
	EnterCriticalSectionPretty(&G->mCriticalSection);
	i = device_map.find(real_unknown);
	if (i != device_map.end()) {
		if (i->second == hacker_device) {
			LogInfo("unregister_hacker_device: Unregistering IUnknown %p -> HackerDevice %p\n",
			        real_unknown, hacker_device);
			device_map.erase(i);
		} else {
			LogInfo("BUG: Removing HackerDevice from device_map"
			        "     IUnknown %p expected to map to %p, actually %p\n",
			        real_unknown, hacker_device, i->second);
		}
	}
	LeaveCriticalSection(&G->mCriticalSection);
}

// -----------------------------------------------------------------------------------------------

HackerDevice::HackerDevice(ID3D11Device1 *pDevice1, ID3D11DeviceContext1 *pContext1) :
	mStereoHandle(0), mStereoResourceView(0), mStereoTexture(0),
	mIniResourceView(0), mIniTexture(0),
	mZBufferResourceView(0),
	mVRInstanceXformReadbackStaging(0),
	mVRWeaponPrivateVB(0),
	mVRWeaponCS(0), mVRWeaponUAV(0), mVRWeaponParamsCB(0)
{
	mOrigDevice1 = pDevice1;
	mRealOrigDevice1 = pDevice1;
	mOrigContext1 = pContext1;
	// Must be done after mOrigDevice1 is set:
	mUnknown = register_hacker_device(this);
	// Install before D3D11CreateDevice returns to Metro. The engine establishes
	// its scene dimensions immediately afterward, before creating scene targets.
	InstallMetroResolutionSetterDiagnostic();
}

HRESULT HackerDevice::CreateStereoParamResources()
{
	HRESULT hr;
	NvAPI_Status nvret;

	// We use the original device here. Functionally it should not matter
	// if we use the HackerDevice, but it does result in a lot of noise in
	// the frame analysis log as every call into nvapi using the
	// mStereoHandle calls Begin() and End() on the immediate context.

	// Todo: This call will fail if stereo is disabled. Proper notification?
	nvret = NvAPI_Stereo_CreateHandleFromIUnknown(mOrigDevice1, &mStereoHandle);
	if (nvret != NVAPI_OK)
	{
		mStereoHandle = 0;
		LogInfo("HackerDevice::CreateStereoParamResources NvAPI_Stereo_CreateHandleFromIUnknown failed: %d\n", nvret);
		return nvret;
	}
	mParamTextureManager.mStereoHandle = mStereoHandle;
	LogInfo("  created NVAPI stereo handle. Handle = %p\n", mStereoHandle);

	// Create stereo parameter texture.
	LogInfo("  creating stereo parameter texture.\n");

	D3D11_TEXTURE2D_DESC desc;
	memset(&desc, 0, sizeof(D3D11_TEXTURE2D_DESC));
	desc.Width = nv::stereo::ParamTextureManagerD3D11::Parms::StereoTexWidth;
	desc.Height = nv::stereo::ParamTextureManagerD3D11::Parms::StereoTexHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = nv::stereo::ParamTextureManagerD3D11::Parms::StereoTexFormat;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;
	hr = mOrigDevice1->CreateTexture2D(&desc, 0, &mStereoTexture);
	if (FAILED(hr))
	{
		LogInfo("    call failed with result = %x.\n", hr);
		return hr;
	}
	LogInfo("    stereo texture created, handle = %p\n", mStereoTexture);

	// Since we need to bind the texture to a shader input, we also need a resource view.
	LogInfo("  creating stereo parameter resource view.\n");

	D3D11_SHADER_RESOURCE_VIEW_DESC descRV;
	memset(&descRV, 0, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));
	descRV.Format = desc.Format;
	descRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	descRV.Texture2D.MostDetailedMip = 0;
	descRV.Texture2D.MipLevels = -1;
	hr = mOrigDevice1->CreateShaderResourceView(mStereoTexture, &descRV, &mStereoResourceView);
	if (FAILED(hr))
	{
		LogInfo("    call failed with result = %x.\n", hr);
		return hr;
	}

	LogInfo("    stereo texture resource view created, handle = %p.\n", mStereoResourceView);
	return S_OK;
}

// Metro2033ReduxVR: a private per-instance vertex buffer for the weapon.
//
// DEFAULT usage rather than DYNAMIC, deliberately: UpdateSubresource is legal
// on DEFAULT and illegal on DYNAMIC (where it silently no-ops, which cost a
// whole debugging session in Notes/09). Since only we ever write this, there
// is no ring-buffer discipline to respect and no neighbouring data to corrupt
// - which is the entire reason for owning a separate buffer instead of
// patching the game's shared one.
//
// One instance is enough: it is re-filled and re-bound per weapon draw.
// Metro2033ReduxVR: one readback buffer PER viewmodel mesh.
//
// The async scheme reads the copy issued a frame earlier, which only works if
// each mesh has its own buffer - a single shared one would hand back whichever
// mesh happened to be copied last. They are 64 bytes each and there are a few
// dozen, so the memory is irrelevant next to avoiding a pipeline stall per
// draw.
// Metro2033ReduxVR: the compute shader that transforms the weapon's instance
// matrix on the GPU, plus its UAV and parameter buffer.
//
// This replaces a synchronous readback per viewmodel draw - copy to staging,
// then Map(READ), which makes the CPU wait for the GPU. That was measured at
// 50-69 stalls per frame, peaking at 364, and it scaled with the number of
// meshes, so it would only get worse as more weapons were supported.
//
// Doing the maths where the data already lives removes the round trip
// entirely: copy GPU-to-GPU, dispatch, bind. Nothing is read back.
//
// The shader also does the cluster test. Without a readback the CPU no longer
// knows where an individual draw sits, and that test is what stops a world
// prop sharing an index count with the weapon from being dragged around by
// the controller. Doing it here keeps that protection.
static const char *kWeaponTransformHLSL = R"(
cbuffer Params : register(b0)
{
    float4 rowD0;      // controller rotation delta, row 0
    float4 rowD1;
    float4 rowD2;
    float4 pivotRadius; // xyz = cluster centre, w = max distance from it
    float4 translate;   // xyz = hand movement + fixed offset
    float4 rotPivot;    // xyz = point the weapon rotates about, w = uniform scale
};

RWByteAddressBuffer Inst : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    float4 r0 = asfloat(Inst.Load4(0));
    float4 r1 = asfloat(Inst.Load4(16));
    float4 r2 = asfloat(Inst.Load4(32));

    float3 t = float3(r0.w, r1.w, r2.w);

    // Cluster test: anything outside the weapon's neighbourhood is a
    // different object that merely shares this index count. Leave it alone.
    float d = abs(t.x - pivotRadius.x) + abs(t.y - pivotRadius.y) + abs(t.z - pivotRadius.z);
    if (d > pivotRadius.w)
        return;

    // Written out longhand rather than with float3x3 and mul().
    //
    // The CPU was confirmed to be producing real rotations (off-diagonal
    // magnitudes up to 3.67) while the weapon stubbornly refused to turn -
    // so the data was right and the matrix maths here was wrong. Rather than
    // reason about how HLSL orders rows in a float3x3 constructor and which
    // operand mul() treats as row-major, this spells out D * R with explicit
    // dot products. There is nothing left to get wrong.
    float3 R0 = r0.xyz, R1 = r1.xyz, R2 = r2.xyz;
    float3 D0 = rowD0.xyz, D1 = rowD1.xyz, D2 = rowD2.xyz;

    // TEMPORARY: ignore the supplied rotation and apply a fixed 45 degrees.
    //
    // Translation works and rotation does not, and the CPU has been confirmed
    // to be sending genuine rotation matrices - so either D is not arriving in
    // the constant buffer, or the rotation rows we write are not what the
    // vertex shader reads for orientation. Those need different fixes and look
    // identical from the headset, so this forces the question: if the weapon
    // comes out visibly rotated by a fixed amount, the write path is sound and
    // the constant buffer is at fault. If it still does not rotate, the
    // orientation is not coming from these rows at all.
    if (translate.w > 0.5)
    {
        const float c = 0.7071, s = 0.7071;
        D0 = float3( c, 0,  s);
        D1 = float3( 0, 1,  0);
        D2 = float3(-s, 0,  c);
    }

    // nR = D * R, where each row of the result is D's row combining R's rows.
    float3 n0 = D0.x * R0 + D0.y * R1 + D0.z * R2;
    float3 n1 = D1.x * R0 + D1.y * R1 + D1.z * R2;
    float3 n2 = D2.x * R0 + D2.y * R1 + D2.z * R2;

    // Uniform scale, about the same pivot the rotation uses.
    //
    // There was no scale term here at all, which is worth stating plainly:
    // nothing in this shader could ever have changed how big the weapon is,
    // so every earlier attempt at the "too small" half of this problem was
    // moving it around rather than resizing it.
    //
    // Scaling the rotation rows resizes the mesh about its own origin; the
    // extra term on the translation moves that origin so the pivot - the grip
    // - is what stays put. Every viewmodel mesh gets the same scale and the
    // same pivot, so the parts stay assembled.
    float scale = rotPivot.w > 0.0 ? rotPivot.w : 1.0;
    n0 *= scale;
    n1 *= scale;
    n2 *= scale;

    // Rotate about the pivot so the parts stay assembled, then move the whole
    // thing with the hand.
    // Rotate about the GRIP, not the cluster centre. The cluster centre is
    // the origin - the player's eye - and pivoting there swings the weapon
    // around their head instead of turning it in their hand.
    float3 rel = t - rotPivot.xyz;
    float3 nt = scale * float3(dot(D0, rel), dot(D1, rel), dot(D2, rel))
              + rotPivot.xyz + translate.xyz;

    Inst.Store4(0,  asuint(float4(n0, nt.x)));
    Inst.Store4(16, asuint(float4(n1, nt.y)));
    Inst.Store4(32, asuint(float4(n2, nt.z)));
}
)";

bool HackerDevice::GetOrCreateVRWeaponCompute(ID3D11ComputeShader **cs,
	ID3D11UnorderedAccessView **uav, ID3D11Buffer **params)
{
	if (mVRWeaponCS && mVRWeaponUAV && mVRWeaponParamsCB) {
		*cs = mVRWeaponCS;
		*uav = mVRWeaponUAV;
		*params = mVRWeaponParamsCB;
		return true;
	}
	static bool failed = false;
	if (failed)
		return false;

	ID3D11Buffer *vb = GetOrCreateVRWeaponPrivateVB();
	if (!vb) {
		failed = true;
		return false;
	}

	if (!mVRWeaponCS) {
		ID3DBlob *blob = NULL, *err = NULL;
		HRESULT hr = D3DCompile(kWeaponTransformHLSL, strlen(kWeaponTransformHLSL),
			"VRWeaponTransform", NULL, NULL, "main", "cs_5_0", 0, 0, &blob, &err);
		if (FAILED(hr)) {
			LogInfo("VRPose weapon: compute shader failed to compile: %s\n",
				err ? (char *)err->GetBufferPointer() : "(no message)");
			if (err) err->Release();
			failed = true;
			return false;
		}
		if (err) err->Release();
		hr = mOrigDevice1->CreateComputeShader(blob->GetBufferPointer(),
			blob->GetBufferSize(), NULL, &mVRWeaponCS);
		blob->Release();
		if (FAILED(hr)) {
			LogInfo("VRPose weapon: CreateComputeShader failed: %x\n", hr);
			failed = true;
			return false;
		}
	}

	if (!mVRWeaponUAV) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
		memset(&ud, 0, sizeof(ud));
		ud.Format = DXGI_FORMAT_R32_TYPELESS;
		ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		ud.Buffer.FirstElement = 0;
		ud.Buffer.NumElements = 16;             // 64 bytes as raw dwords
		ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(mOrigDevice1->CreateUnorderedAccessView(vb, &ud, &mVRWeaponUAV))) {
			LogInfo("VRPose weapon: CreateUnorderedAccessView failed\n");
			failed = true;
			return false;
		}
	}

	if (!mVRWeaponParamsCB) {
		D3D11_BUFFER_DESC cbd;
		memset(&cbd, 0, sizeof(cbd));
		cbd.ByteWidth = 96;                     // six float4s
		cbd.Usage = D3D11_USAGE_DEFAULT;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (FAILED(mOrigDevice1->CreateBuffer(&cbd, NULL, &mVRWeaponParamsCB))) {
			LogInfo("VRPose weapon: params buffer failed\n");
			failed = true;
			return false;
		}
	}

	LogInfo("VRPose weapon: GPU transform ready - no more readbacks\n");
	*cs = mVRWeaponCS;
	*uav = mVRWeaponUAV;
	*params = mVRWeaponParamsCB;
	return true;
}

ID3D11Buffer* HackerDevice::GetOrCreateWeaponStaging(UINT key)
{
	auto it = mVRWeaponStaging.find(key);
	if (it != mVRWeaponStaging.end())
		return it->second;

	D3D11_BUFFER_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = 64;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	ID3D11Buffer *buf = NULL;
	if (FAILED(mOrigDevice1->CreateBuffer(&desc, NULL, &buf)))
		buf = NULL;
	mVRWeaponStaging[key] = buf;
	return buf;
}

ID3D11Buffer* HackerDevice::GetOrCreateVRWeaponPrivateVB()
{
	if (mVRWeaponPrivateVB)
		return mVRWeaponPrivateVB;

	D3D11_BUFFER_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = 64;                       // one per-instance stride
	desc.Usage = D3D11_USAGE_DEFAULT;
	// Also writable by a compute shader, so the weapon's transform can be
	// applied on the GPU instead of being read back to the CPU. The raw-view
	// flag is what allows a ByteAddressBuffer UAV over it.
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	desc.CPUAccessFlags = 0;

	HRESULT hr = mOrigDevice1->CreateBuffer(&desc, NULL, &mVRWeaponPrivateVB);
	if (FAILED(hr)) {
		LogInfo("VRPose: failed to create the private weapon instance buffer: %x\n", hr);
		mVRWeaponPrivateVB = NULL;
	} else {
		LogInfo("VRPose: created the private weapon instance buffer\n");
	}
	return mVRWeaponPrivateVB;
}

ID3D11Buffer* HackerDevice::GetOrCreateVRInstanceXformReadbackStaging()
{
	if (mVRInstanceXformReadbackStaging)
		return mVRInstanceXformReadbackStaging;

	D3D11_BUFFER_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = 64;   // the full per-instance stride, not just the 3x4
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	HRESULT hr = mOrigDevice1->CreateBuffer(&desc, NULL, &mVRInstanceXformReadbackStaging);
	if (FAILED(hr)) {
		LogInfo("VRPose: failed to create instance xform readback staging buffer: %x\n", hr);
		mVRInstanceXformReadbackStaging = NULL;
	}
	return mVRInstanceXformReadbackStaging;
}

HRESULT HackerDevice::CreateIniParamResources()
{
	// No longer making this conditional. We are pretty well dependent on
	// the ini params these days and not creating this view might cause
	// issues with config reload.

	HRESULT ret;
	D3D11_SUBRESOURCE_DATA initialData;
	D3D11_TEXTURE1D_DESC desc;
	memset(&desc, 0, sizeof(D3D11_TEXTURE1D_DESC));

	// If we are resizing IniParams we must release the old versions:
	if (mIniResourceView) {
		long refcount = mIniResourceView->Release();
		mIniResourceView = NULL;
		LogInfo("  releasing ini parameters resource view, refcount = %d\n", refcount);
	}
	if (mIniTexture) {
		long refcount = mIniTexture->Release();
		mIniTexture = NULL;
		LogInfo("  releasing iniparams texture, refcount = %d\n", refcount);
	}

	if (G->iniParamsReserved > INI_PARAMS_SIZE_WARNING) {
		LogOverlay(LOG_NOTICE, "NOTICE: %d requested IniParams exceeds the recommended %d\n",
				G->iniParamsReserved, INI_PARAMS_SIZE_WARNING);
	}

	// Metro2033ReduxVR single-pass stereo: reserve room for the reprojection
	// matrix, which lives in entries 8..11.
	//
	// 3Dmigoto sizes this texture to whatever the ini declares - here just one
	// texel, since d3dx.ini only sets x and w. Loading texels 1..3 from a
	// one-texel texture returns zero, so the right eye's clip position came
	// out as (x, 0, 0, 0): w = 0, a degenerate divide, which is why its
	// geometry vanished and blew out to white while the left eye - which never
	// applies the matrix - stayed perfect.
	//
	// Entries 8..11 rather than 0..3 so the ini's own values cannot collide
	// with the matrix, in this game or in anyone else's config.
	G->iniParamsReserved = max(G->iniParamsReserved, StereoSinglePass::kReprojectionParam + 4);

	G->iniParams.resize(G->iniParamsReserved);
	if (G->iniParams.empty()) {
		LogInfo("  No IniParams used, skipping texture creation.\n");
		return S_OK;
	}

	LogInfo("  creating .ini constant parameter texture.\n");

	// Stuff the constants read from the .ini file into the subresource data structure, so 
	// we can init the texture with them.
	initialData.pSysMem = G->iniParams.data();
	initialData.SysMemPitch = sizeof(DirectX::XMFLOAT4) * (UINT)G->iniParams.size(); // Ignored for Texture1D, but still recommended for debugging

	desc.Width = (UINT)G->iniParams.size();					// n texels, .rgba as a float4
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;				// float4
	desc.Usage = D3D11_USAGE_DYNAMIC;							// Read/Write access from GPU and CPU
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;				// As resource view, access via t120
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;				// allow CPU access for hotkeys
	desc.MiscFlags = 0;
	ret = mOrigDevice1->CreateTexture1D(&desc, &initialData, &mIniTexture);
	if (FAILED(ret))
	{
		LogInfo("    CreateTexture1D call failed with result = %x.\n", ret);
		return ret;
	}
	LogInfo("    IniParam texture created, handle = %p\n", mIniTexture);

	// Since we need to bind the texture to a shader input, we also need a resource view.
	// The pDesc is set to NULL so that it will simply use the desc format above.
	LogInfo("  creating IniParam resource view.\n");

	D3D11_SHADER_RESOURCE_VIEW_DESC descRV;
	memset(&descRV, 0, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));

	ret = mOrigDevice1->CreateShaderResourceView(mIniTexture, NULL, &mIniResourceView);
	if (FAILED(ret))
	{
		LogInfo("   CreateShaderResourceView call failed with result = %x.\n", ret);
		return ret;
	}

	LogInfo("    Iniparams resource view created, handle = %p.\n", mIniResourceView);
	return S_OK;
}

void HackerDevice::CreatePinkHuntingResources()
{
	// Only create special pink mode PixelShader when requested.
	if (G->hunting && (G->marking_mode == MarkingMode::PINK || G->config_reloadable))
	{
		char* hlsl =
			"float4 pshader() : SV_Target0"
			"{"
			"	return float4(1,0,1,1);"
			"}";

		ID3D10Blob* blob = NULL;
		HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "JustPink", NULL, NULL, "pshader", "ps_4_0", 0, 0, &blob, NULL);
		LogInfo("  Created pink mode pixel shader: %d\n", hr);
		if (SUCCEEDED(hr))
		{
			hr = mOrigDevice1->CreatePixelShader((DWORD*)blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &G->mPinkingShader);
			CleanupShaderMaps(G->mPinkingShader);
			if (FAILED(hr))
				LogInfo("  Failed to create pinking pixel shader: %d\n", hr);
			blob->Release();
		}
	}
}

HRESULT HackerDevice::SetGlobalNVSurfaceCreationMode()
{
	HRESULT hr;

	// Override custom settings.
	if (mStereoHandle && G->gSurfaceCreateMode >= 0)
	{
		NvAPIOverride();
		LogInfo("  setting custom surface creation mode.\n");

		hr = Profiling::NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,	(NVAPI_STEREO_SURFACECREATEMODE)G->gSurfaceCreateMode);
		if (hr != NVAPI_OK)
		{
			LogInfo("    custom surface creation call failed: %d.\n", hr);
			return hr;
		}
	}

	return S_OK;
}


// With the addition of full DXGI support, this init sequence is too dangerous
// to do at object creation time.  The NV CreateHandleFromIUnknown calls back
// into this device, so we need to have it set up and ready.

void HackerDevice::Create3DMigotoResources()
{
	LogInfo("HackerDevice::Create3DMigotoResources(%s@%p) called.\n", type_name(this), this);

	// XXX: Ignoring the return values for now because so do our callers.
	// If we want to change this, keep in mind that failures in
	// CreateStereoParamResources and SetGlobalNVSurfaceCreationMode should
	// be considdered non-fatal, as stereo could be disabled in the control
	// panel, or we could be on an AMD or Intel card.

	LockResourceCreationMode();

	CreateStereoParamResources();
	CreateIniParamResources();
	CreatePinkHuntingResources();
	SetGlobalNVSurfaceCreationMode();

	UnlockResourceCreationMode();

	optimise_command_lists(this);
}


// Save reference to corresponding HackerContext during CreateDevice, needed for GetImmediateContext.

void HackerDevice::SetHackerContext(HackerContext *pHackerContext)
{
	mHackerContext = pHackerContext;
}

HackerContext* HackerDevice::GetHackerContext()
{
	LogInfo("HackerDevice::GetHackerContext returns %p\n", mHackerContext);
	return mHackerContext;
}

void HackerDevice::SetHackerSwapChain(HackerSwapChain *pHackerSwapChain)
{
	mHackerSwapChain = pHackerSwapChain;
}

HackerSwapChain* HackerDevice::GetHackerSwapChain()
{
	return mHackerSwapChain;
}


// Returns the "real" DirectX object. Note that if hooking is enabled calls
// through this object will go back into 3DMigoto, which would then subject
// them to extra logging and any processing 3DMigoto applies, which may be
// undesirable in some cases. This used to cause a crash if a command list
// issued a draw call, since that would then trigger the command list and
// recurse until the stack ran out:
ID3D11Device1* HackerDevice::GetPossiblyHookedOrigDevice1()
{
	return mRealOrigDevice1;
}

// Use this one when you specifically don't want calls through this object to
// ever go back into 3DMigoto. If hooking is disabled this is identical to the
// above, but when hooking this will be the trampoline object instead:
ID3D11Device1* HackerDevice::GetPassThroughOrigDevice1()
{
	return mOrigDevice1;
}

ID3D11DeviceContext1* HackerDevice::GetPossiblyHookedOrigContext1()
{
	return mOrigContext1;
}

ID3D11DeviceContext1* HackerDevice::GetPassThroughOrigContext1()
{
	if (mHackerContext)
		return mHackerContext->GetPassThroughOrigContext1();

	return mOrigContext1;
}

IUnknown* HackerDevice::GetIUnknown()
{
	return mUnknown;
}

void HackerDevice::HookDevice()
{
	// This will install hooks in the original device (if they have not
	// already been installed from a prior device) which will call the
	// equivalent function in this HackerDevice. It returns a trampoline
	// interface which we use in place of mOrigDevice1 to call the real
	// original device, thereby side stepping the problem that calling the
	// old mOrigDevice1 would be hooked and call back into us endlessly:
	mOrigDevice1 = hook_device(mOrigDevice1, this);
}





// -----------------------------------------------------------------------------------------------
// ToDo: I'd really rather not have these standalone utilities here, this file should
// ideally be only HackerDevice and it's methods.  Because of our spaghetti Globals+Utils,
// it gets too involved to move these out right now.


// For any given vertex or pixel shader from the ShaderFixes folder, we need to track them at load time so
// that we can associate a given active shader with an override file.  This allows us to reload the shaders
// dynamically, and do on-the-fly fix testing.
// ShaderModel is usually something like "vs_5_0", but "bin" is a valid ShaderModel string, and tells the 
// reloader to disassemble the .bin file to determine the shader model.

// Currently, critical lock must be taken BEFORE this is called.

static void RegisterForReload(ID3D11DeviceChild* ppShader, UINT64 hash, wstring shaderType, string shaderModel,
	ID3D11ClassLinkage* pClassLinkage, ID3DBlob* byteCode, FILETIME timeStamp, wstring text, bool deferred_replacement_candidate)
{
	LogInfo("    shader registered for possible reloading: %016llx_%ls as %s - %ls\n", hash, shaderType.c_str(), shaderModel.c_str(), text.c_str());

	// Pretty sure we had a bug before since we would save a pointer to the
	// class linkage object without bumping its refcount, but I don't know
	// of any game that uses this to test it.
	if (pClassLinkage)
		pClassLinkage->AddRef();

	G->mReloadedShaders[ppShader].hash = hash;
	G->mReloadedShaders[ppShader].shaderType = shaderType;
	G->mReloadedShaders[ppShader].shaderModel = shaderModel;
	G->mReloadedShaders[ppShader].linkage = pClassLinkage;
	G->mReloadedShaders[ppShader].byteCode = byteCode;
	G->mReloadedShaders[ppShader].timeStamp = timeStamp;
	G->mReloadedShaders[ppShader].replacement = NULL;
	G->mReloadedShaders[ppShader].infoText = text;
	G->mReloadedShaders[ppShader].deferred_replacement_candidate = deferred_replacement_candidate;
	G->mReloadedShaders[ppShader].deferred_replacement_processed = false;
}


// Helper routines for ReplaceShader, as a way to factor out some of the inline code, in
// order to make it more clear, and as a first step toward full refactoring.

// This routine exports the original binary shader from the game, the cso.  It is a hidden
// feature in the d3dx.ini.  Seems like it might be nice to have them named *_orig.bin, to
// make them more clear.

static void ExportOrigBinary(UINT64 hash, const wchar_t *pShaderType, const void *pShaderBytecode, SIZE_T pBytecodeLength)
{
	wchar_t path[MAX_PATH];
	HANDLE f;
	bool exists = false;

	swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls.bin", G->SHADER_CACHE_PATH, hash, pShaderType);
	f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f != INVALID_HANDLE_VALUE)
	{
		int cnt = 0;
		while (f != INVALID_HANDLE_VALUE)
		{
			// Check if same file.
			DWORD dataSize = GetFileSize(f, 0);
			char *buf = new char[dataSize];
			DWORD readSize;
			if (!ReadFile(f, buf, dataSize, &readSize, 0) || dataSize != readSize)
				LogInfo("  Error reading file.\n");
			CloseHandle(f);
			if (dataSize == pBytecodeLength && !memcmp(pShaderBytecode, buf, dataSize))
				exists = true;
			delete[] buf;
			if (exists)
				break;
			swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_%d.bin", G->SHADER_CACHE_PATH, hash, pShaderType, ++cnt);
			f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		}
	}
	if (!exists)
	{
		FILE *fw;
		wfopen_ensuring_access(&fw, path, L"wb");
		if (fw)
		{
			LogInfoW(L"    storing original binary shader to %s\n", path);
			fwrite(pShaderBytecode, 1, pBytecodeLength, fw);
			fclose(fw);
		}
		else
		{
			LogInfoW(L"    error storing original binary shader to %s\n", path);
		}
	}
}


static bool GetFileLastWriteTime(wchar_t *path, FILETIME *ftWrite)
{
	HANDLE f;

	f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return false;

	GetFileTime(f, NULL, NULL, ftWrite);
	CloseHandle(f);
	return true;
}

static bool CheckCacheTimestamp(HANDLE binHandle, wchar_t *binPath, FILETIME &pTimeStamp)
{
	FILETIME txtTime, binTime;
	wchar_t txtPath[MAX_PATH], *end = NULL;

	wcscpy_s(txtPath, MAX_PATH, binPath);
	end = wcsstr(txtPath, L".bin");
	wcscpy_s(end, sizeof(L".bin"), L".txt");
	if (GetFileLastWriteTime(txtPath, &txtTime) && GetFileTime(binHandle, NULL, NULL, &binTime)) {
		// We need to compare the timestamp on the .bin and .txt files.
		// This needs to be an exact match to ensure that the .bin file
		// corresponds to this .txt file (and we need to explicitly set
		// this timestamp when creating the .bin file). Just checking
		// for newer modification time is not enough, since the .txt
		// files in the zip files that fixes are distributed in contain
		// a timestamp that may be older than .bin files generated on
		// an end-users system.
		if (CompareFileTime(&binTime, &txtTime))
			return false;

		// It no longer matters which timestamp we save for later
		// comparison, since they need to match, but we save the .txt
		// file's timestamp since that is the one we are comparing
		// against later.
		pTimeStamp = txtTime;
		return true;
	}

	// If we couldn't get the timestamps it probably means the
	// corresponding .txt file no longer exists. This is actually a bit of
	// an odd (but not impossible) situation to be in - if a user used
	// uninstall.bat when updating a fix they should have removed any stale
	// .bin files as well, and if they didn't use uninstall.bat then they
	// should only be adding new files... so how did a shader that used to
	// be present disappear but leave the cache next to it?
	//
	// A shaderhacker might hit this if they removed the .txt file but not
	// the .bin file, but we could consider that to be user error, so it's
	// not clear any policy here would be correct. Alternatively, a fix
	// might have been shipped with only .bin files - historically we have
	// allowed (but discouraged) that scenario, so for now we issue a
	// warning but allow it.
	LogInfo("    WARNING: Unable to validate timestamp of %S"
			" - no corresponding .txt file?\n", binPath);
	return true;
}

static bool LoadCachedShader(wchar_t *binPath, const wchar_t *pShaderType,
	__out char* &pCode, SIZE_T &pCodeSize, string &pShaderModel, FILETIME &pTimeStamp)
{
	HANDLE f;
	DWORD codeSize, readSize;

	f = CreateFile(binPath, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return false;

	if (!CheckCacheTimestamp(f, binPath, pTimeStamp)) {
		LogInfoW(L"    Discarding stale cached shader: %s\n", binPath);
		goto bail_close_handle;
	}

	LogInfoW(L"    Replacement binary shader found: %s\n", binPath);
	WarnIfConflictingShaderExists(binPath, end_user_conflicting_shader_msg);

	codeSize = GetFileSize(f, 0);
	pCode = new char[codeSize];
	if (!ReadFile(f, pCode, codeSize, &readSize, 0)
		|| codeSize != readSize)
	{
		LogInfo("    Error reading binary file.\n");
		goto err_free_code;
	}

	pCodeSize = codeSize;
	LogInfo("    Bytecode loaded. Size = %Iu\n", pCodeSize);
	CloseHandle(f);

	pShaderModel = "bin";		// tag it as reload candidate, but needing disassemble

	return true;

err_free_code:
	delete[] pCode;
	pCode = NULL;
bail_close_handle:
	CloseHandle(f);
	return false;
}

// Load .bin shaders from the ShaderFixes folder as cached shaders.
// This will load either *_replace.bin, or *.bin variants.

static bool LoadBinaryShaders(__in UINT64 hash, const wchar_t *pShaderType,
	__out char* &pCode, SIZE_T &pCodeSize, string &pShaderModel, FILETIME &pTimeStamp)
{
	wchar_t path[MAX_PATH];

	swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_replace.bin", G->SHADER_PATH, hash, pShaderType);
	if (LoadCachedShader(path, pShaderType, pCode, pCodeSize, pShaderModel, pTimeStamp))
		return true;

	// If we can't find an HLSL compiled version, look for ASM assembled one.
	swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls.bin", G->SHADER_PATH, hash, pShaderType);
	return LoadCachedShader(path, pShaderType, pCode, pCodeSize, pShaderModel, pTimeStamp);
}


// Load an HLSL text file as the replacement shader.  Recompile it using D3DCompile.
// If caching is enabled, save a .bin replacement for this new shader.

static bool ReplaceHLSLShader(__in UINT64 hash, const wchar_t *pShaderType,
	__in const void *pShaderBytecode, SIZE_T pBytecodeLength, const char *pOverrideShaderModel,
	__out char* &pCode, SIZE_T &pCodeSize, string &pShaderModel, FILETIME &pTimeStamp, wstring &pHeaderLine)
{
	wchar_t path[MAX_PATH];
	HANDLE f;
	string shaderModel;

	swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_replace.txt", G->SHADER_PATH, hash, pShaderType);
	f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f != INVALID_HANDLE_VALUE)
	{
		LogInfo("    Replacement shader found. Loading replacement HLSL code.\n");
		WarnIfConflictingShaderExists(path, end_user_conflicting_shader_msg);

		DWORD srcDataSize = GetFileSize(f, 0);
		char *srcData = new char[srcDataSize];
		DWORD readSize;
		FILETIME ftWrite;
		if (!ReadFile(f, srcData, srcDataSize, &readSize, 0)
			|| !GetFileTime(f, NULL, NULL, &ftWrite)
			|| srcDataSize != readSize)
			LogInfo("    Error reading file.\n");
		CloseHandle(f);
		LogInfo("    Source code loaded. Size = %d\n", srcDataSize);

		// Disassemble old shader to get shader model.
		shaderModel = GetShaderModel(pShaderBytecode, pBytecodeLength);
		if (shaderModel.empty())
		{
			LogInfo("    disassembly of original shader failed.\n");

			delete[] srcData;
		}
		else
		{
			// Any HLSL compiled shaders are reloading candidates, if moved to ShaderFixes
			pShaderModel = shaderModel;
			pTimeStamp = ftWrite;
			std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> utf8_to_utf16;
			pHeaderLine = utf8_to_utf16.from_bytes(srcData, strchr(srcData, '\n'));

			// Way too many obscure interractions in this function, using another
			// temporary variable to not modify anything already here and reduce
			// the risk of breaking it in some subtle way:
			const char *tmpShaderModel;
			char apath[MAX_PATH];

			if (pOverrideShaderModel)
				tmpShaderModel = pOverrideShaderModel;
			else
				tmpShaderModel = shaderModel.c_str();

			// Compile replacement.
			LogInfo("    compiling replacement HLSL code with shader model %s\n", tmpShaderModel);

			// TODO: Add #defines for StereoParams and IniParams

			ID3DBlob *errorMsgs; // FIXME: This can leak
			ID3DBlob *compiledOutput = 0;
			// Pass the real filename and use the standard include handler so that
			// #include will work with a relative path from the shader itself.
			// Later we could add a custom include handler to track dependencies so
			// that we can make reloading work better when using includes:
			wcstombs(apath, path, MAX_PATH);
			MigotoIncludeHandler include_handler(apath);
			HRESULT ret = D3DCompile(srcData, srcDataSize, apath, 0,
				G->recursive_include == -1 ? D3D_COMPILE_STANDARD_FILE_INCLUDE : &include_handler,
				"main", tmpShaderModel, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &compiledOutput, &errorMsgs);
			delete[] srcData; srcData = 0;
			if (compiledOutput)
			{
				pCodeSize = compiledOutput->GetBufferSize();
				pCode = new char[pCodeSize];
				memcpy(pCode, compiledOutput->GetBufferPointer(), pCodeSize);
				compiledOutput->Release(); compiledOutput = 0;
			}

			LogInfo("    compile result of replacement HLSL shader: %x\n", ret);

			if (LogFile && errorMsgs)
			{
				LPVOID errMsg = errorMsgs->GetBufferPointer();
				SIZE_T errSize = errorMsgs->GetBufferSize();
				LogInfo("--------------------------------------------- BEGIN ---------------------------------------------\n");
				fwrite(errMsg, 1, errSize - 1, LogFile);
				LogInfo("---------------------------------------------- END ----------------------------------------------\n");
				errorMsgs->Release();
			}

			// Cache binary replacement.
			if (G->CACHE_SHADERS && pCode)
			{
				swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_replace.bin", G->SHADER_PATH, hash, pShaderType);
				FILE *fw;
				wfopen_ensuring_access(&fw, path, L"wb");
				if (fw)
				{
					LogInfo("    storing compiled shader to %S\n", path);
					fwrite(pCode, 1, pCodeSize, fw);
					fclose(fw);

					// Set the last modified timestamp on the cached shader to match the
					// .txt file it is created from, so we can later check its validity:
					set_file_last_write_time(path, &ftWrite);
				} else
					LogInfo("    error writing compiled shader to %S\n", path);
			}
		}
	}
	return !!pCode;
}


// If a matching file exists, load an ASM text shader as a replacement for a shader.  
// Reassemble it, and return the binary.
//
// Changing the output of this routine to be simply .bin files. We had some old test
// code for assembler validation, but that just causes confusion.  Retiring the *_reasm.txt
// files as redundant.
// Files are like: 
//  cc79d4a79b16b59c-vs.txt  as ASM text
//  cc79d4a79b16b59c-vs.bin  as reassembled binary shader code
//
// Using this naming convention because we already have multiple fixes that use the *-vs.txt format
// to mean ASM text files, and changing all of those seems unnecessary.  This will parallel the use
// of HLSL files like:
//  cc79d4a79b16b59c-vs_replace.txt   as HLSL text
//  cc79d4a79b16b59c-vs_replace.bin   as recompiled binary shader code
//
// So it should be clear by name, what type of file they are.  

static bool ReplaceASMShader(__in UINT64 hash, const wchar_t *pShaderType, const void *pShaderBytecode, SIZE_T pBytecodeLength,
	__out char* &pCode, SIZE_T &pCodeSize, string &pShaderModel, FILETIME &pTimeStamp, wstring &pHeaderLine)
{
	wchar_t path[MAX_PATH];
	HANDLE f;
	string shaderModel;

	swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls.txt", G->SHADER_PATH, hash, pShaderType);
	f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f != INVALID_HANDLE_VALUE)
	{
		LogInfo("    Replacement ASM shader found. Assembling replacement ASM code.\n");
		WarnIfConflictingShaderExists(path, end_user_conflicting_shader_msg);

		DWORD srcDataSize = GetFileSize(f, 0);
		vector<char> asmTextBytes(srcDataSize);
		DWORD readSize;
		FILETIME ftWrite;
		if (!ReadFile(f, asmTextBytes.data(), srcDataSize, &readSize, 0)
			|| !GetFileTime(f, NULL, NULL, &ftWrite)
			|| srcDataSize != readSize)
			LogInfo("    Error reading file.\n");
		CloseHandle(f);
		LogInfo("    Asm source code loaded. Size = %d\n", srcDataSize);

		// Disassemble old shader to get shader model.
		shaderModel = GetShaderModel(pShaderBytecode, pBytecodeLength);
		if (shaderModel.empty())
		{
			LogInfo("    disassembly of original shader failed.\n");
		}
		else
		{
			// Any ASM shaders are reloading candidates, if moved to ShaderFixes
			pShaderModel = shaderModel;
			pTimeStamp = ftWrite;
			std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> utf8_to_utf16;
			pHeaderLine = utf8_to_utf16.from_bytes(asmTextBytes.data(), strchr(asmTextBytes.data(), '\n'));

			vector<byte> byteCode(pBytecodeLength);
			memcpy(byteCode.data(), pShaderBytecode, pBytecodeLength);

			// Re-assemble the ASM text back to binary
			try
			{
				vector<AssemblerParseError> parse_errors;
				byteCode = AssembleFluganWithOptionalSignatureParsing(&asmTextBytes, G->assemble_signature_comments, &byteCode, &parse_errors);

				// Assuming the re-assembly worked, let's make it the active shader code.
				pCodeSize = byteCode.size();
				pCode = new char[pCodeSize];
				memcpy(pCode, byteCode.data(), pCodeSize);

				// Cache binary replacement.
				if (parse_errors.empty()) {
					if (G->CACHE_SHADERS && pCode && parse_errors.empty())
					{
						// Write reassembled binary output as a cached shader.
						FILE *fw;
						swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls.bin", G->SHADER_PATH, hash, pShaderType);
						wfopen_ensuring_access(&fw, path, L"wb");
						if (fw)
						{
							LogInfoW(L"    storing reassembled binary to %s\n", path);
							fwrite(byteCode.data(), 1, byteCode.size(), fw);
							fclose(fw);

							// Set the last modified timestamp on the cached shader to match the
							// .txt file it is created from, so we can later check its validity:
							set_file_last_write_time(path, &ftWrite);
						}
						else
						{
							LogInfoW(L"    error storing reassembled binary to %s\n", path);
						}
					}
				} else {
					// Parse errors are currently being treated as non-fatal on
					// creation time replacement and ShaderRegex for backwards
					// compatibility (live shader reload is fatal).
					for (auto &parse_error : parse_errors)
						LogOverlay(LOG_NOTICE, "%S: %s\n", path, parse_error.what());

					// Do not record the timestamp so that F10 will reload the
					// shader even if not touched in the meantime allowing the
					// shaderhackers to see their bugs. For much the same
					// reason we disable caching these shaders above (though
					// that is not retrospective if a cache already existed).
					pTimeStamp = {0};
				}
			}
			catch (const exception &e)
			{
				LogOverlay(LOG_WARNING, "Error assembling %S: %s\n",
						path, e.what());
			}
		}
	}

	return !!pCode;
}

static bool DecompileAndPossiblyPatchShader(__in UINT64 hash,
		const wchar_t *pShaderType, const void *pShaderBytecode,
		SIZE_T BytecodeLength, __out char* &pCode, SIZE_T &pCodeSize,
		string &pShaderModel, FILETIME &pTimeStamp,
		wstring &pHeaderLine, const wchar_t *shaderType,
		string &foundShaderModel, FILETIME &timeStamp,
		const char *overrideShaderModel)
{
	wchar_t val[MAX_PATH];
	string asmText;
	FILE *fw = NULL;
	string shaderModel = "";
	bool patched = false;
	bool errorOccurred = false;
	HRESULT hr;

	if (!G->EXPORT_HLSL && !G->decompiler_settings.fixSvPosition && !G->decompiler_settings.recompileVs)
		return NULL;

	// Skip?
	swprintf_s(val, MAX_PATH, L"%ls\\%016llx-%ls_bad.txt", G->SHADER_PATH, hash, shaderType);
	if (GetFileAttributes(val) != INVALID_FILE_ATTRIBUTES) {
		LogInfo("    skipping shader marked bad. %S\n", val);
		return NULL;
	}

	// Store HLSL export files in ShaderCache, auto-Fixed shaders in ShaderFixes
	if (G->EXPORT_HLSL >= 1)
		swprintf_s(val, MAX_PATH, L"%ls\\%016llx-%ls_replace.txt", G->SHADER_CACHE_PATH, hash, shaderType);
	else
		swprintf_s(val, MAX_PATH, L"%ls\\%016llx-%ls_replace.txt", G->SHADER_PATH, hash, shaderType);

	// If we can open the file already, it exists, and thus we should skip doing this slow operation again.
	if (GetFileAttributes(val) != INVALID_FILE_ATTRIBUTES)
		return NULL;

	// Disassemble old shader for fixing.
	asmText = BinaryToAsmText(pShaderBytecode, BytecodeLength, false);
	if (asmText.empty()) {
		LogInfo("    disassembly of original shader failed.\n");
		return NULL;
	}

	// Decompile code.
	LogInfo("    creating HLSL representation.\n");

	ParseParameters p;
	p.bytecode = pShaderBytecode;
	p.decompiled = asmText.c_str();
	p.decompiledSize = asmText.size();
	p.ZeroOutput = false;
	p.G = &G->decompiler_settings;
	const string decompiledCode = DecompileBinaryHLSL(p, patched, shaderModel, errorOccurred);
	if (!decompiledCode.size() || errorOccurred)
	{
		LogInfo("    error while decompiling.\n");
		return NULL;
	}

	if ((G->EXPORT_HLSL >= 1) || (G->EXPORT_FIXED && patched))
	{
		errno_t err = wfopen_ensuring_access(&fw, val, L"wb");
		if (err != 0 || !fw)
		{
			LogInfo("    !!! Fail to open replace.txt file: 0x%x\n", err);
			return NULL;
		}

		LogInfo("    storing patched shader to %S\n", val);
		// Save decompiled HLSL code to that new file.
		fwrite(decompiledCode.c_str(), 1, decompiledCode.size(), fw);

		// Now also write the ASM text to the shader file as a set of comments at the bottom.
		// That will make the ASM code the master reference for fixing shaders, and should be more
		// convenient, especially in light of the numerous decompiler bugs we see.
		if (G->EXPORT_HLSL >= 2)
		{
			fprintf_s(fw, "\n\n/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Original ASM ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
			fwrite(asmText.c_str(), 1, asmText.size(), fw);
			fprintf_s(fw, "\n//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/\n");

		}
	}

	// Let's re-compile every time we create a new one, regardless.  Previously this would only re-compile
	// after auto-fixing shaders. This makes shader Decompiler errors more obvious.

	// Way too many obscure interractions in this function, using another
	// temporary variable to not modify anything already here and reduce
	// the risk of breaking it in some subtle way:
	const char *tmpShaderModel;
	char apath[MAX_PATH];

	if (overrideShaderModel)
		tmpShaderModel = overrideShaderModel;
	else
		tmpShaderModel = shaderModel.c_str();

	LogInfo("    compiling fixed HLSL code with shader model %s, size = %Iu\n", tmpShaderModel, decompiledCode.size());

	// TODO: Add #defines for StereoParams and IniParams

	ID3DBlob *pErrorMsgs;
	ID3DBlob *pCompiledOutput = NULL;
	// Probably unecessary here since this shader is one we freshly decompiled,
	// but for consistency pass the path here as well so that the standard
	// include handler can correctly handle includes with paths relative to the
	// shader itself:
	wcstombs(apath, val, MAX_PATH);
	hr = D3DCompile(decompiledCode.c_str(), decompiledCode.size(), apath, 0, D3D_COMPILE_STANDARD_FILE_INCLUDE,
		"main", tmpShaderModel, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pCompiledOutput, &pErrorMsgs);
	LogInfo("    compile result of fixed HLSL shader: %x\n", hr);

	if (LogFile && pErrorMsgs)
	{
		LPVOID errMsg = pErrorMsgs->GetBufferPointer();
		SIZE_T errSize = pErrorMsgs->GetBufferSize();
		LogInfo("--------------------------------------------- BEGIN ---------------------------------------------\n");
		fwrite(errMsg, 1, errSize - 1, LogFile);
		LogInfo("------------------------------------------- HLSL code -------------------------------------------\n");
		fwrite(decompiledCode.c_str(), 1, decompiledCode.size(), LogFile);
		LogInfo("\n---------------------------------------------- END ----------------------------------------------\n");

		// And write the errors to the HLSL file as comments too, as a more convenient spot to see them.
		fprintf_s(fw, "\n\n/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~ HLSL errors ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
		fwrite(errMsg, 1, errSize - 1, fw);
		fprintf_s(fw, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/\n");
	}
	if (pErrorMsgs)
		pErrorMsgs->Release();

	// If requested by .ini, also write the newly re-compiled assembly code to the file.  This gives a direct
	// comparison between original ASM, and recompiled ASM.
	if ((G->EXPORT_HLSL >= 3) && pCompiledOutput)
	{
		asmText = BinaryToAsmText(pCompiledOutput->GetBufferPointer(), pCompiledOutput->GetBufferSize(), G->patch_cb_offsets);
		if (asmText.empty())
		{
			LogInfo("    disassembly of recompiled shader failed.\n");
		}
		else
		{
			fprintf_s(fw, "\n\n/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Recompiled ASM ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
			fwrite(asmText.c_str(), 1, asmText.size(), fw);
			fprintf_s(fw, "\n//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/\n");
		}
	}

	if (pCompiledOutput)
	{
		// If the shader has been auto-fixed, return it as the live shader.
		// For just caching shaders, we return zero so it won't affect game visuals.
		if (patched)
		{
			pCodeSize = pCompiledOutput->GetBufferSize();
			pCode = new char[pCodeSize];
			memcpy(pCode, pCompiledOutput->GetBufferPointer(), pCodeSize);
		}
		pCompiledOutput->Release();
		pCompiledOutput = NULL;
	}

	if (fw)
	{
		// Any HLSL compiled shaders are reloading candidates, if moved to ShaderFixes
		FILETIME ftWrite;
		GetFileTime(fw, NULL, NULL, &ftWrite);
		foundShaderModel = shaderModel;
		timeStamp = ftWrite;

		fclose(fw);
	}

	return !!pCode;
}

// Fairly bold new strategy here for ReplaceShader. 
// This is called at launch to replace any shaders that we might want patched to fix problems.
// It would previously use both ShaderCache, and ShaderFixes both to fix shaders, but this is
// problematic in that broken shaders dumped as part of universal cache could be buggy, and generated
// visual anomolies.  Moreover, we don't really want every file to patched, just the ones we need.

// I'm moving to a model where only stuff in ShaderFixes is active, and stuff in ShaderCache is for reference.
// This will allow us to dump and use the ShaderCache for offline fixes, looking for similar fix patterns, and
// also make them live by moving them to ShaderFixes.
// For auto-fixed shaders- rather than leave them in ShaderCache, when they are fixed, we'll move them into 
// ShaderFixes as being live.  

// Only used in CreateXXXShader (Vertex, Pixel, Compute, Geometry, Hull, Domain)

// This whole function is in need of major refactoring. At a quick glance I can
// see several code paths that will leak objects, and in general it is far,
// too long and complex - the human brain has between 5 an 9 (typically 7)
// general purpose registers, but this function requires far more than that to
// understand. I've added comments to a few objects that can leak, but there's
// little value in fixing one or two problems without tackling the whole thing,
// but I need to understand it better before I'm willing to start refactoring
// it. -DarkStarSword
//
// Chapter 6 of the Linux coding style guidelines is worth a read:
//   https://www.kernel.org/doc/Documentation/CodingStyle
//
// In general I (bo3b) agree, but would hesitate to apply a C style guide to kinda/sorta 
// C++ code.  A sort of mix of the linux guide and C++ is Google's Style Guide:
//   https://google.github.io/styleguide/cppguide.html
// Apparently serious C++ programmers hate it, so that must mean it makes things simpler
// and clearer. We could stand to even have just a style template in VS that would
// make everything consistent at a minimum.  I'd say refactoring this sucker is
// higher value though.
//
// I hate to make a bad thing worse, but I need to return yet another parameter here, 
// the string read from the first line of the HLSL file.  This the logical place for
// it because the file is already open and read into memory.

char* HackerDevice::_ReplaceShaderFromShaderFixes(UINT64 hash, const wchar_t *shaderType, const void *pShaderBytecode,
	SIZE_T BytecodeLength, SIZE_T &pCodeSize, string &foundShaderModel, FILETIME &timeStamp,
	wstring &headerLine, const char *overrideShaderModel)
{
	foundShaderModel = "";
	timeStamp = { 0 };

	char *pCode = 0;

	if (!G->SHADER_PATH[0] || !G->SHADER_CACHE_PATH[0])
		return NULL;

	// Export every original game shader as a .bin file.
	if (G->EXPORT_BINARY)
		ExportOrigBinary(hash, shaderType, pShaderBytecode, BytecodeLength);

	// Export every shader seen as an ASM text file.
	if (G->EXPORT_SHADERS)
		CreateAsmTextFile(G->SHADER_CACHE_PATH, hash, shaderType, pShaderBytecode, BytecodeLength, G->patch_cb_offsets);


	// Read the binary compiled shaders, as previously cached shaders.  This is how
	// fixes normally ship, so that we just load previously compiled/assembled shaders.
	if (LoadBinaryShaders(hash, shaderType, pCode, pCodeSize, foundShaderModel, timeStamp))
		return pCode;

	// Load previously created HLSL shaders, but only from ShaderFixes.
	if (ReplaceHLSLShader(hash, shaderType, pShaderBytecode, BytecodeLength, overrideShaderModel,
				pCode, pCodeSize, foundShaderModel, timeStamp, headerLine)) {
		return pCode;
	}

	// If still not found, look for replacement ASM text shaders.
	if (ReplaceASMShader(hash, shaderType, pShaderBytecode, BytecodeLength,
				pCode, pCodeSize, foundShaderModel, timeStamp, headerLine)) {
		return pCode;
	}

	if (DecompileAndPossiblyPatchShader(hash, shaderType, pShaderBytecode, BytecodeLength,
				pCode, pCodeSize, foundShaderModel, timeStamp, headerLine,
				shaderType, foundShaderModel, timeStamp, overrideShaderModel)) {
		return pCode;
	}

	return NULL;
}

// This function handles shaders replaced from ShaderFixes at load time with or
// without hunting.
//
// When hunting is disabled we don't save off the original shader unless we
// determine that we need it for depth or partner filtering.  These shaders are
// not candidates for the auto patch engine.
//
// When hunting is enabled we always save off the original shader because the
// answer to "do we need the original?" is "...maybe?"
template <class ID3D11Shader,
	 HRESULT (__stdcall ID3D11Device::*OrigCreateShader)(THIS_
			 __in const void *pShaderBytecode,
			 __in SIZE_T BytecodeLength,
			 __in_opt ID3D11ClassLinkage *pClassLinkage,
			 __out_opt ID3D11Shader **ppShader)
	 >
HRESULT HackerDevice::ReplaceShaderFromShaderFixes(UINT64 hash,
		const void *pShaderBytecode, SIZE_T BytecodeLength,
		ID3D11ClassLinkage *pClassLinkage, ID3D11Shader **ppShader,
		wchar_t *shaderType)
{
	ShaderOverrideMap::iterator override;
	const char *overrideShaderModel = NULL;
	SIZE_T replaceShaderSize;
	string shaderModel;
	wstring headerLine;
	FILETIME ftWrite;
	HRESULT hr = E_FAIL;

	// Check if the user has overridden the shader model:
	override = lookup_shaderoverride(hash);
	if (override != G->mShaderOverrideMap.end()) {
		if (override->second.model[0])
			overrideShaderModel = override->second.model;
	}

	char *replaceShader = _ReplaceShaderFromShaderFixes(hash, shaderType,
			pShaderBytecode, BytecodeLength, replaceShaderSize,
			shaderModel, ftWrite, headerLine, overrideShaderModel);
	if (!replaceShader)
		return E_FAIL;

	// Create the new shader.
	LogDebug("    HackerDevice::Create%lsShader.  Device: %p\n", shaderType, this);

	*ppShader = NULL; // Appease the static analysis gods
	hr = (mOrigDevice1->*OrigCreateShader)(replaceShader, replaceShaderSize, pClassLinkage, ppShader);
	if (FAILED(hr)) {
		LogInfo("    error replacing shader.\n");
		goto out_delete;
	}

	CleanupShaderMaps(*ppShader);

	LogInfo("    shader successfully replaced.\n");

	if (G->hunting) {
		// Hunting mode:  keep byteCode around for possible replacement or marking
		ID3DBlob* blob;
		hr = D3DCreateBlob(BytecodeLength, &blob);
		if (SUCCEEDED(hr)) {
			// We save the *original* shader bytecode, not the replaced shader,
			// because we will use this in CopyToFixes and ShaderRegex in the
			// event that the shader is deleted.
			memcpy(blob->GetBufferPointer(), pShaderBytecode, blob->GetBufferSize());
			EnterCriticalSectionPretty(&G->mCriticalSection);
			RegisterForReload(*ppShader, hash, shaderType, shaderModel, pClassLinkage, blob, ftWrite, headerLine, false);
			LeaveCriticalSection(&G->mCriticalSection);
		}
	}

	// FIXME: We have some very similar data structures that we should merge together:
	// mReloadedShaders and mOriginalShader.
	KeepOriginalShader<ID3D11Shader, OrigCreateShader>
		(hash, shaderType, *ppShader, pShaderBytecode, BytecodeLength, pClassLinkage);

out_delete:
	delete replaceShader;
	return hr;
}

// This function handles shaders that were *NOT* replaced from ShaderFixes
//
// When hunting is disabled we don't save off the original shader unless we
// determine that we need it for for deferred analysis in the auto patch
// engine. These are not candidates for depth or partner filtering since that
// would require a ShaderOverride and a manually patched shader (ok,
// technically we could with an auto patched shader, but those are deprecated
// features - don't encourage them!)
//
// When hunting is enabled we always save off the original shader because the
// answer to "do we need the original?" is "...maybe?"
template <class ID3D11Shader,
	 HRESULT (__stdcall ID3D11Device::*OrigCreateShader)(THIS_
			 __in const void *pShaderBytecode,
			 __in SIZE_T BytecodeLength,
			 __in_opt ID3D11ClassLinkage *pClassLinkage,
			 __out_opt ID3D11Shader **ppShader)
	 >
HRESULT HackerDevice::ProcessShaderNotFoundInShaderFixes(UINT64 hash,
		const void *pShaderBytecode, SIZE_T BytecodeLength,
		ID3D11ClassLinkage *pClassLinkage, ID3D11Shader **ppShader,
		wchar_t *shaderType)
{
	HRESULT hr;

	*ppShader = NULL; // Appease the static analysis gods
	hr = (mOrigDevice1->*OrigCreateShader)(pShaderBytecode, BytecodeLength, pClassLinkage, ppShader);
	if (FAILED(hr))
		return hr;

	CleanupShaderMaps(*ppShader);

	// When in hunting mode, make a copy of the original binary, regardless.  This can be replaced, but we'll at least
	// have a copy for every shader seen. If we are performing any sort of deferred shader replacement, such as pipline
	// state analysis we always need to keep a copy of the original bytecode for later analysis. For now the shader
	// regex engine counts as deferred, though that may change with optimisations in the future.
	if (G->hunting || !shader_regex_groups.empty()) {
		EnterCriticalSectionPretty(&G->mCriticalSection);
			ID3DBlob* blob;
			hr = D3DCreateBlob(BytecodeLength, &blob);
			if (SUCCEEDED(hr)) {
				memcpy(blob->GetBufferPointer(), pShaderBytecode, blob->GetBufferSize());
				RegisterForReload(*ppShader, hash, shaderType, "bin", pClassLinkage, blob, {0}, L"", true);

				// Also add the original shader to the original shaders
				// map so that if it is later replaced marking_mode =
				// original and depth buffer filtering will work:
				if (lookup_original_shader(*ppShader) == end(G->mOriginalShaders)) {
					// Since we are both returning *and* storing this we need to
					// bump the refcount to 2, otherwise it could get freed and we
					// may get a crash later in RevertMissingShaders, especially
					// easy to expose with the auto shader patching engine
					// and reverting shaders:
					(*ppShader)->AddRef();
					G->mOriginalShaders[*ppShader] = *ppShader;
				}
			}
		LeaveCriticalSection(&G->mCriticalSection);
	}

	return hr;
}

bool HackerDevice::NeedOriginalShader(UINT64 hash)
{
	ShaderOverride *shaderOverride;
	ShaderOverrideMap::iterator i;

	if (G->hunting && (G->marking_mode == MarkingMode::ORIGINAL || G->config_reloadable || G->show_original_enabled))
		return true;

	i = lookup_shaderoverride(hash);
	if (i == G->mShaderOverrideMap.end())
		return false;
	shaderOverride = &i->second;

	if ((shaderOverride->depth_filter == DepthBufferFilter::DEPTH_ACTIVE) ||
		(shaderOverride->depth_filter == DepthBufferFilter::DEPTH_INACTIVE)) {
		return true;
	}

	if (shaderOverride->partner_hash)
		return true;

	return false;
}

// This function ensures that a shader handle is expunged from all our shader
// maps. Ideally we would call this when the shader is released, but since we
// don't wrap or hook that call we can't do that. Instead, we call it just
// after any CreateXXXShader call - at that time we know the handle was
// previously invalid and is now valid, but we haven't used it yet.
//
// This is a big hammer, and we could probably cut this down, but I want to
// make very certain that we don't have any other unusual sequences that could
// lead to us using stale entries (e.g. suppose an application called
// XXGetShader() and retrieved a shader 3DMigoto had set, then later called
// XXSetShader() - we would look up that handle, and if that handle had been
// reused we might end up trying to replace it). This fixes an issue where we
// could sometimes mistakingly revert one shader to an unrelated shader on F10:
//
//   https://github.com/bo3b/3Dmigoto/issues/86
//
void CleanupShaderMaps(ID3D11DeviceChild *handle)
{
	if (!handle)
		return;

	EnterCriticalSectionPretty(&G->mCriticalSection);

	{
		ShaderMap::iterator i = lookup_shader_hash(handle);
		if (i != G->mShaders.end()) {
			LogInfo("Shader handle %p reused, previous hash was: %016llx\n", handle, i->second);
			G->mShaders.erase(i);
		}
	}

	{
		ShaderReloadMap::iterator i = lookup_reloaded_shader(handle);
		if (i != G->mReloadedShaders.end()) {
			LogInfo("Shader handle %p reused, found in mReloadedShaders\n", handle);
			if (i->second.replacement)
				i->second.replacement->Release();
			if (i->second.byteCode)
				i->second.byteCode->Release();
			if (i->second.linkage)
				i->second.linkage->Release();
			G->mReloadedShaders.erase(i);
		}
	}

	{
		ShaderReplacementMap::iterator i = lookup_original_shader(handle);
		if (i != G->mOriginalShaders.end()) {
			LogInfo("Shader handle %p reused, releasing previous original shader\n", handle);
			i->second->Release();
			G->mOriginalShaders.erase(i);
		}
	}

	LeaveCriticalSection(&G->mCriticalSection);
}

// Keep the original shader around if it may be needed by a filter in a
// [ShaderOverride] section, or if hunting is enabled and either the
// marking_mode=original, or reload_config support is enabled
template <class ID3D11Shader,
	 HRESULT (__stdcall ID3D11Device::*OrigCreateShader)(THIS_
			 __in const void *pShaderBytecode,
			 __in SIZE_T BytecodeLength,
			 __in_opt ID3D11ClassLinkage *pClassLinkage,
			 __out_opt ID3D11Shader **ppShader)
	 >
void HackerDevice::KeepOriginalShader(UINT64 hash, wchar_t *shaderType,
		ID3D11Shader *pShader,
		const void *pShaderBytecode,
		SIZE_T BytecodeLength,
		ID3D11ClassLinkage *pClassLinkage)
{
	ID3D11Shader *originalShader = NULL;
	HRESULT hr;

	if (!NeedOriginalShader(hash))
		return;

	LogInfoW(L"    keeping original shader for filtering: %016llx-%ls\n", hash, shaderType);

	EnterCriticalSectionPretty(&G->mCriticalSection);

		hr = (mOrigDevice1->*OrigCreateShader)(pShaderBytecode, BytecodeLength, pClassLinkage, &originalShader);
		CleanupShaderMaps(originalShader);
		if (SUCCEEDED(hr))
			G->mOriginalShaders[pShader] = originalShader;

		// Unlike the *other* code path in CreateShader that can also
		// fill out this structure, we do *not* bump the refcount on
		// the originalShader here since we are *only* storing it, not
		// also returning it to the game.

	LeaveCriticalSection(&G->mCriticalSection);
}


// -----------------------------------------------------------------------------------------------

/*** IUnknown methods ***/

STDMETHODIMP_(ULONG) HackerDevice::AddRef(THIS)
{
	return mOrigDevice1->AddRef();
}

STDMETHODIMP_(ULONG) HackerDevice::Release(THIS)
{
	ULONG ulRef = mOrigDevice1->Release();
	LogDebug("HackerDevice::Release counter=%d, this=%p\n", ulRef, this);

	if (ulRef <= 0)
	{
		if (!gLogDebug)
			LogInfo("HackerDevice::Release counter=%d, this=%p\n", ulRef, this);
		LogInfo("  deleting self\n");

		unregister_hacker_device(this);

		if (mStereoHandle)
		{
			int result = NvAPI_Stereo_DestroyHandle(mStereoHandle);
			mStereoHandle = 0;
			LogInfo("  releasing NVAPI stereo handle, result = %d\n", result);
		}
		if (mStereoResourceView)
		{
			long result = mStereoResourceView->Release();
			mStereoResourceView = 0;
			LogInfo("  releasing stereo parameters resource view, result = %d\n", result);
		}
		if (mStereoTexture)
		{
			long result = mStereoTexture->Release();
			mStereoTexture = 0;
			LogInfo("  releasing stereo texture, result = %d\n", result);
		}
		if (mIniResourceView)
		{
			long result = mIniResourceView->Release();
			mIniResourceView = 0;
			LogInfo("  releasing ini parameters resource view, result = %d\n", result);
		}
		if (mIniTexture)
		{
			long result = mIniTexture->Release();
			mIniTexture = 0;
			LogInfo("  releasing iniparams texture, result = %d\n", result);
		}
		delete this;
		return 0L;
	}
	return ulRef;
}

// If called with IDXGIDevice, that's the game trying to access the original DXGIFactory to 
// get access to the swap chain.  We need to return a HackerDXGIDevice so that we can get 
// access to that swap chain.
// 
// This is the 'secret' path to getting the DXGIFactory and thus the swap chain, without
// having to go direct to DXGI calls. As described:
// https://msdn.microsoft.com/en-us/library/windows/desktop/bb174535(v=vs.85).aspx
//
// This technique is used in Mordor for sure, and very likely others.
//
// Next up, it seems that we also need to handle a QueryInterface(IDXGIDevice1), as
// WatchDogs uses that call.  Another oddity: this device is called to return the
// same device. ID3D11Device->QueryInterface(ID3D11Device).  No idea why, but we
// need to return our wrapped version.
// 
// 1-4-18: No longer using this technique, we have a direct hook on CreateSwapChain,
// which will catch all variants. But leaving documentation for awhile.

// New addition, we need to also look for QueryInterface casts to different types.
// In Dragon Age, it seems clear that they are upcasting their ID3D11Device to an
// ID3D11Device1, and if we don't wrap that, we have an object leak where they can bypass us.
//
// Initial call needs to be LogDebug, because this is otherwise far to chatty in the
// log.  That can be kind of misleading, so careful with missing log info. To
// keep it consistent, all normal cases will be LogDebug, error states are LogInfo.

HRESULT STDMETHODCALLTYPE HackerDevice::QueryInterface(
	/* [in] */ REFIID riid,
	/* [iid_is][out] */ _COM_Outptr_ void __RPC_FAR *__RPC_FAR *ppvObject)
{
	LogDebug("HackerDevice::QueryInterface(%s@%p) called with IID: %s\n", type_name(this), this, NameFromIID(riid).c_str());

	if (ppvObject && IsEqualIID(riid, IID_HackerDevice)) {
		// This is a special case - only 3DMigoto itself should know
		// this IID, so this is us checking if it has a HackerDevice.
		// There's no need to call through to DX for this one.
		AddRef();
		*ppvObject = this;
		return S_OK;
	}

	HRESULT hr = mOrigDevice1->QueryInterface(riid, ppvObject);
	if (FAILED(hr))
	{
		LogInfo("  failed result = %x for %p\n", hr, ppvObject);
		return hr;
	}

	// No need for further checks of null ppvObject, as it could not have successfully
	// called the original in that case.

	if (riid == __uuidof(ID3D11Device))
	{
		if (!(G->enable_hooks & EnableHooks::DEVICE)) {
			// If we are hooking we don't return the wrapped device
			*ppvObject = this;
		}
		LogDebug("  return HackerDevice(%s@%p) wrapper of %p\n", type_name(this), this, mRealOrigDevice1);
	}
	else if (riid == __uuidof(ID3D11Device1))
	{
		// Well, bizarrely, this approach to upcasting to a ID3D11Device1 is supported on Win7, 
		// but only if you have the 'evil update', the platform update installed.  Since that
		// is an optional update, that certainly means that numerous people do not have it 
		// installed. Ergo, a game developer cannot in good faith just assume that it's there,
		// and it's very unlikely they would require it. No performance advantage on Win8.
		// So, that means that a game developer must support a fallback path, even if they
		// actually want Device1 for some reason.
		//
		// Sooo... Current plan is to return an error here, and pretend that the platform
		// update is not installed, or missing feature on Win8.1.  This will force the game
		// to use a more compatible path and make our job easier.
		// This worked in DragonAge, to avoid a crash. Wrapping Device1 also progressed but
		// adds a ton of undesirable complexity, so let's keep it simpler since we don't 
		// seem to lose anything. Not features, not performance.
		//
		// Dishonored 2 is the first known game that lacks a fallback
		// and requires the platform update.

		if (!G->enable_platform_update) {
			LogInfo("  returns E_NOINTERFACE as error for ID3D11Device1 (try allow_platform_update=1 if the game refuses to run).\n");
			*ppvObject = NULL;
			return E_NOINTERFACE;
		}

		if (!(G->enable_hooks & EnableHooks::DEVICE)) {
			// If we are hooking we don't return the wrapped device
			*ppvObject = this;
		}
		LogDebug("  return HackerDevice(%s@%p) wrapper of %p\n", type_name(this), this, mRealOrigDevice1);
	}

	LogDebug("  returns result = %x for %p\n", hr, *ppvObject);
	return hr;
}

// -----------------------------------------------------------------------------------------------

/*** ID3D11Device methods ***/

// These are the boilerplate routines that are necessary to pass through any calls to these
// to Direct3D.  Since Direct3D does not have proper objects, we can't rely on super class calls.

STDMETHODIMP HackerDevice::CreateUnorderedAccessView(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pResource,
	/* [annotation] */
	__in_opt  const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc,
	/* [annotation] */
	__out_opt  ID3D11UnorderedAccessView **ppUAView)
{
	return mOrigDevice1->CreateUnorderedAccessView(pResource, pDesc, ppUAView);
}

STDMETHODIMP HackerDevice::CreateRenderTargetView(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pResource,
	/* [annotation] */
	__in_opt  const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
	/* [annotation] */
	__out_opt  ID3D11RenderTargetView **ppRTView)
{
	LogDebug("HackerDevice::CreateRenderTargetView(%s@%p)\n", type_name(this), this);

	// Over a two-slice target the game's own view has to be pinned to slice 0.
	// Left as the game asked - which is usually a NULL description - it would
	// cover BOTH slices and its post-processing would write the left eye's
	// image into the right eye as well.
	D3D11_RENDER_TARGET_VIEW_DESC vrDesc;
	pDesc = StereoTwin::NarrowRTVDesc(pResource, pDesc, &vrDesc);

	HRESULT hr = mOrigDevice1->CreateRenderTargetView(pResource, pDesc, ppRTView);
	if (hr == S_OK && ppRTView)
		StereoTwin::OnCreateRTV(mOrigDevice1, pResource, pDesc, *ppRTView);
	return hr;
}

STDMETHODIMP HackerDevice::CreateDepthStencilView(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pResource,
	/* [annotation] */
	__in_opt  const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
	/* [annotation] */
	__out_opt  ID3D11DepthStencilView **ppDepthStencilView)
{
	LogDebug("HackerDevice::CreateDepthStencilView(%s@%p)\n", type_name(this), this);

	D3D11_DEPTH_STENCIL_VIEW_DESC vrDesc;
	pDesc = StereoTwin::NarrowDSVDesc(pResource, pDesc, &vrDesc);

	HRESULT hr = mOrigDevice1->CreateDepthStencilView(pResource, pDesc, ppDepthStencilView);
	if (hr == S_OK && ppDepthStencilView)
		StereoTwin::OnCreateDSV(mOrigDevice1, pResource, pDesc, *ppDepthStencilView);
	return hr;
}

STDMETHODIMP HackerDevice::CreateInputLayout(THIS_
	/* [annotation] */
	__in_ecount(NumElements)  const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs,
	/* [annotation] */
	__in_range(0, D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT)  UINT NumElements,
	/* [annotation] */
	__in  const void *pShaderBytecodeWithInputSignature,
	/* [annotation] */
	__in  SIZE_T BytecodeLength,
	/* [annotation] */
	__out_opt  ID3D11InputLayout **ppInputLayout)
{
	HRESULT ret;
	ID3DBlob *blob;

	ret = mOrigDevice1->CreateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature,
		BytecodeLength, ppInputLayout);

	// Metro2033ReduxVR: identify layouts using the particle/instanced-
	// sprite system's per-instance "inst0..inst7" semantic convention at
	// IA slot 1 (billboard position/orientation data, e.g. fences - see
	// Notes/10) - the one shader family confirmed via disassembly to
	// NOT treat this buffer as a matrix, so PatchInstanceXformAtOffset
	// must leave it alone. Deliberately a blocklist, not an allowlist:
	// an earlier allowlist (only patch the weapon's confirmed
	// "xform0..xform3" naming) regressed the weapon AND wrongly
	// excluded other legitimate matrix-consuming layouts this project
	// has never sampled (Notes/10 Update 3) - every distinct per-object
	// shader family plausibly has its own semantic naming, and
	// enumerating all of them isn't tractable, whereas blocking just
	// the one confirmed non-matrix case is. Unconditional (not gated by
	// G->hunting like the existing frame-analysis blob caching below)
	// since PatchInstanceXformAtOffset needs this in normal gameplay,
	// not just in a debug hunting session. Case-insensitive
	// (_strnicmp, not strncmp) since D3D11 semantic names are matched
	// case-insensitively by the runtime/HLSL convention.
	if (SUCCEEDED(ret) && ppInputLayout && *ppInputLayout && pInputElementDescs) {
		static int sLayoutDiagCount = 0;
		bool matched = false;
		for (UINT i = 0; i < NumElements; i++) {
			if (pInputElementDescs[i].InputSlot == 1 &&
			    pInputElementDescs[i].SemanticName &&
			    !_strnicmp(pInputElementDescs[i].SemanticName, "inst", 4)) {
				VRPose::RegisterKnownNonMatrixInputLayout(*ppInputLayout);
				matched = true;
			}
		}
		// Laser-impact diagnostic: the impact dot shares a 200-instance
		// particle draw with unrelated effects.  Record the exact packed input
		// layout so its one instance can be decoded and corrected without
		// moving the entire batch.
		if (matched) {
			LogInfo("VRPose particle layout: layout=%p elements=%u\n",
				*ppInputLayout, NumElements);
			for (UINT i = 0; i < NumElements; ++i) {
				const D3D11_INPUT_ELEMENT_DESC &e = pInputElementDescs[i];
				LogInfo("VRPose particle layout: elem=%u semantic=%s%u format=%u "
					"slot=%u offset=%u class=%u step=%u\n", i,
					e.SemanticName ? e.SemanticName : "(null)", e.SemanticIndex,
					(unsigned)e.Format, e.InputSlot, e.AlignedByteOffset,
					(unsigned)e.InputSlotClass, e.InstanceDataStepRate);
			}
		}
		// Metro2033ReduxVR TEMPORARY diagnostic (Notes/10 Update 5):
		// also specifically tag the confirmed weapon layout so
		// PatchInstanceXformAtOffset can test writing an inert value
		// for it while everything else keeps its real correction.
		for (UINT i = 0; i < NumElements; i++) {
			if (pInputElementDescs[i].InputSlot == 1 &&
			    pInputElementDescs[i].SemanticName &&
			    !_strnicmp(pInputElementDescs[i].SemanticName, "xform", 5)) {
				VRPose::RegisterKnownWeaponXformInputLayout(*ppInputLayout);
				break;
			}
		}
		// Metro2033ReduxVR: does this layout actually CONSUME a per-instance
		// matrix from slot 1?
		//
		// The game binds slot 1 for essentially every draw, but only shaders
		// whose layout declares elements there read it. Substituting our
		// buffer for a draw that ignores slot 1 changes nothing visible -
		// which is exactly what happened: the delta was live, the matrix was
		// rewritten, and the weapon did not move.
		//
		// Element COUNT is the discriminator established in Notes/10: four
		// elements on slot 1 is a 4x4 matrix regardless of whether they are
		// named xform or inst, while the particle path uses eight elements
		// with TEXCOORD semantics. Naming was never reliable - the registry
		// built from "xform" prefixes matched draws that were not the weapon
		// and missed the ones that were.
		// Metro2033ReduxVR: is this a SKINNED mesh?
		//
		// Bone weights and indices are declared per-vertex, so the semantics
		// say so outright. Naming varies between engines, hence matching a
		// few spellings rather than one.
		for (UINT i = 0; i < NumElements; i++) {
			const char *sem = pInputElementDescs[i].SemanticName;
			if (!sem)
				continue;
			if (!_strnicmp(sem, "BLEND", 5) || !_strnicmp(sem, "BONE", 4) ||
			    !_strnicmp(sem, "WEIGHT", 6) || !_strnicmp(sem, "SKIN", 4)) {
				VRPose::RegisterSkinnedInputLayout(*ppInputLayout);
				LogInfo("VRPose diag: skinned layout %p (semantic \"%s\", %u elements)\n",
					*ppInputLayout, sem, NumElements);
				break;
			}
		}

		{
			UINT slot1Count = 0;
			for (UINT i = 0; i < NumElements; i++) {
				if (pInputElementDescs[i].InputSlot == 1)
					slot1Count++;
			}
			if (slot1Count == 4)
				VRPose::RegisterInstanceMatrixInputLayout(*ppInputLayout);
		}

		if (sLayoutDiagCount < 500) {
			bool hasSlot1 = false;
			for (UINT i = 0; i < NumElements; i++) {
				if (pInputElementDescs[i].InputSlot == 1) {
					hasSlot1 = true;
					LogInfo("VRPose diag: CreateInputLayout layout=%p elem[%u] slot1 semantic=\"%s\" excludedAsNonMatrix=%d\n",
						*ppInputLayout, i, pInputElementDescs[i].SemanticName ? pInputElementDescs[i].SemanticName : "(null)", matched);
				}
			}
			if (hasSlot1)
				sLayoutDiagCount++;
		}
	}

	if (G->hunting && SUCCEEDED(ret) && ppInputLayout && *ppInputLayout) {
		// When dumping vertex buffers to text file in frame analysis
		// we want to use the input layout to decode the buffer, but
		// DirectX provides no API to query this. So, we store a copy
		// of the input layout in a blob inside the private data of the
		// input layout object. The private data is slow to access, so
		// we should not use this in a hot path, but for frame analysis
		// it doesn't matter. We use a blob to manage releasing the
		// backing memory, since the anonymous void* version of this
		// API does not appear to free the private data on release.

		if (SUCCEEDED(D3DCreateBlob(sizeof(D3D11_INPUT_ELEMENT_DESC) * NumElements, &blob))) {
			memcpy(blob->GetBufferPointer(), pInputElementDescs, blob->GetBufferSize());
			(*ppInputLayout)->SetPrivateDataInterface(InputLayoutDescGuid, blob);
			blob->Release();
		}
	}

	return ret;
}

STDMETHODIMP HackerDevice::CreateClassLinkage(THIS_
	/* [annotation] */
	__out  ID3D11ClassLinkage **ppLinkage)
{
	return mOrigDevice1->CreateClassLinkage(ppLinkage);
}

STDMETHODIMP HackerDevice::CreateBlendState(THIS_
	/* [annotation] */
	__in  const D3D11_BLEND_DESC *pBlendStateDesc,
	/* [annotation] */
	__out_opt  ID3D11BlendState **ppBlendState)
{
	return mOrigDevice1->CreateBlendState(pBlendStateDesc, ppBlendState);
}

STDMETHODIMP HackerDevice::CreateDepthStencilState(THIS_
	/* [annotation] */
	__in  const D3D11_DEPTH_STENCIL_DESC *pDepthStencilDesc,
	/* [annotation] */
	__out_opt  ID3D11DepthStencilState **ppDepthStencilState)
{
	return mOrigDevice1->CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState);
}

STDMETHODIMP HackerDevice::CreateSamplerState(THIS_
	/* [annotation] */
	__in  const D3D11_SAMPLER_DESC *pSamplerDesc,
	/* [annotation] */
	__out_opt  ID3D11SamplerState **ppSamplerState)
{
	return mOrigDevice1->CreateSamplerState(pSamplerDesc, ppSamplerState);
}

STDMETHODIMP HackerDevice::CreateQuery(THIS_
	/* [annotation] */
	__in  const D3D11_QUERY_DESC *pQueryDesc,
	/* [annotation] */
	__out_opt  ID3D11Query **ppQuery)
{
	HRESULT hr = mOrigDevice1->CreateQuery(pQueryDesc, ppQuery);
	if (G->hunting && SUCCEEDED(hr) && ppQuery && *ppQuery)
		G->mQueryTypes[*ppQuery] = AsyncQueryType::QUERY;
	return hr;
}

STDMETHODIMP HackerDevice::CreatePredicate(THIS_
	/* [annotation] */
	__in  const D3D11_QUERY_DESC *pPredicateDesc,
	/* [annotation] */
	__out_opt  ID3D11Predicate **ppPredicate)
{
	HRESULT hr = mOrigDevice1->CreatePredicate(pPredicateDesc, ppPredicate);
	if (G->hunting && SUCCEEDED(hr) && ppPredicate && *ppPredicate)
		G->mQueryTypes[*ppPredicate] = AsyncQueryType::PREDICATE;
	return hr;
}

STDMETHODIMP HackerDevice::CreateCounter(THIS_
	/* [annotation] */
	__in  const D3D11_COUNTER_DESC *pCounterDesc,
	/* [annotation] */
	__out_opt  ID3D11Counter **ppCounter)
{
	HRESULT hr = mOrigDevice1->CreateCounter(pCounterDesc, ppCounter);
	if (G->hunting && SUCCEEDED(hr) && ppCounter && *ppCounter)
		G->mQueryTypes[*ppCounter] = AsyncQueryType::COUNTER;
	return hr;
}

STDMETHODIMP HackerDevice::OpenSharedResource(THIS_
	/* [annotation] */
	__in  HANDLE hResource,
	/* [annotation] */
	__in  REFIID ReturnedInterface,
	/* [annotation] */
	__out_opt  void **ppResource)
{
	return mOrigDevice1->OpenSharedResource(hResource, ReturnedInterface, ppResource);
}

STDMETHODIMP HackerDevice::CheckFormatSupport(THIS_
	/* [annotation] */
	__in  DXGI_FORMAT Format,
	/* [annotation] */
	__out  UINT *pFormatSupport)
{
	return mOrigDevice1->CheckFormatSupport(Format, pFormatSupport);
}

STDMETHODIMP HackerDevice::CheckMultisampleQualityLevels(THIS_
	/* [annotation] */
	__in  DXGI_FORMAT Format,
	/* [annotation] */
	__in  UINT SampleCount,
	/* [annotation] */
	__out  UINT *pNumQualityLevels)
{
	return mOrigDevice1->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels);
}

STDMETHODIMP_(void) HackerDevice::CheckCounterInfo(THIS_
	/* [annotation] */
	__out  D3D11_COUNTER_INFO *pCounterInfo)
{
	return mOrigDevice1->CheckCounterInfo(pCounterInfo);
}

STDMETHODIMP HackerDevice::CheckCounter(THIS_
	/* [annotation] */
	__in  const D3D11_COUNTER_DESC *pDesc,
	/* [annotation] */
	__out  D3D11_COUNTER_TYPE *pType,
	/* [annotation] */
	__out  UINT *pActiveCounters,
	/* [annotation] */
	__out_ecount_opt(*pNameLength)  LPSTR szName,
	/* [annotation] */
	__inout_opt  UINT *pNameLength,
	/* [annotation] */
	__out_ecount_opt(*pUnitsLength)  LPSTR szUnits,
	/* [annotation] */
	__inout_opt  UINT *pUnitsLength,
	/* [annotation] */
	__out_ecount_opt(*pDescriptionLength)  LPSTR szDescription,
	/* [annotation] */
	__inout_opt  UINT *pDescriptionLength)
{
	return mOrigDevice1->CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength, szUnits,
		pUnitsLength, szDescription, pDescriptionLength);
}

STDMETHODIMP HackerDevice::CheckFeatureSupport(THIS_
	D3D11_FEATURE Feature,
	/* [annotation] */
	__out_bcount(FeatureSupportDataSize)  void *pFeatureSupportData,
	UINT FeatureSupportDataSize)
{
	return mOrigDevice1->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
}

STDMETHODIMP HackerDevice::GetPrivateData(THIS_
	/* [annotation] */
	__in  REFGUID guid,
	/* [annotation] */
	__inout  UINT *pDataSize,
	/* [annotation] */
	__out_bcount_opt(*pDataSize)  void *pData)
{
	return mOrigDevice1->GetPrivateData(guid, pDataSize, pData);
}

STDMETHODIMP HackerDevice::SetPrivateData(THIS_
	/* [annotation] */
	__in  REFGUID guid,
	/* [annotation] */
	__in  UINT DataSize,
	/* [annotation] */
	__in_bcount_opt(DataSize)  const void *pData)
{
	return mOrigDevice1->SetPrivateData(guid, DataSize, pData);
}

STDMETHODIMP HackerDevice::SetPrivateDataInterface(THIS_
	/* [annotation] */
	__in  REFGUID guid,
	/* [annotation] */
	__in_opt  const IUnknown *pData)
{
	LogInfo("HackerDevice::SetPrivateDataInterface(%s@%p) called with IID: %s\n", type_name(this), this, NameFromIID(guid).c_str());

	return mOrigDevice1->SetPrivateDataInterface(guid, pData);
}

// Doesn't seem like any games use this, but might be something we need to
// return only DX11.

STDMETHODIMP_(D3D_FEATURE_LEVEL) HackerDevice::GetFeatureLevel(THIS)
{
	D3D_FEATURE_LEVEL featureLevel = mOrigDevice1->GetFeatureLevel();

	LogDebug("HackerDevice::GetFeatureLevel(%s@%p) returns FeatureLevel:%x\n", type_name(this), this, featureLevel);
	return featureLevel;
}

STDMETHODIMP_(UINT) HackerDevice::GetCreationFlags(THIS)
{
	return mOrigDevice1->GetCreationFlags();
}

STDMETHODIMP HackerDevice::GetDeviceRemovedReason(THIS)
{
	return mOrigDevice1->GetDeviceRemovedReason();
}

STDMETHODIMP HackerDevice::SetExceptionMode(THIS_
	UINT RaiseFlags)
{
	return mOrigDevice1->SetExceptionMode(RaiseFlags);
}

STDMETHODIMP_(UINT) HackerDevice::GetExceptionMode(THIS)
{
	return mOrigDevice1->GetExceptionMode();
}



// -----------------------------------------------------------------------------------------------

static bool check_texture_override_iteration(TextureOverride *textureOverride)
{
	if (textureOverride->iterations.empty())
		return true;

	std::vector<int>::iterator k = textureOverride->iterations.begin();
	int currentIteration = textureOverride->iterations[0] = textureOverride->iterations[0] + 1;
	LogInfo("  current iteration = %d\n", currentIteration);

	while (++k != textureOverride->iterations.end()) {
		if (currentIteration == *k)
			return true;
	}

	LogInfo("  override skipped\n");
	return false;
}

// Only Texture2D surfaces can be square. Use template specialisation to skip
// the check on other resource types:
template <typename DescType>
static bool is_square_surface(DescType *desc) {
	return false;
}
static bool is_square_surface(D3D11_TEXTURE2D_DESC *desc)
{
	return (desc && G->gSurfaceSquareCreateMode >= 0
			&& desc->Width == desc->Height
			&& (desc->Usage & D3D11_USAGE_IMMUTABLE) == 0);
}

// Template specialisations to override resource descriptions.
// TODO: Refactor this to use common code with CustomResource.
// TODO: Add overrides for BindFlags since they can affect the stereo mode.
// Maybe MiscFlags as well in case we need to do something like forcing a
// buffer to be unstructured to allow it to be steroised when
// StereoFlagsDX10=0x000C000.

template <typename DescType>
static void override_resource_desc_common_2d_3d(DescType *desc, TextureOverride *textureOverride)
{
	if (textureOverride->format != -1) {
		LogInfo("  setting custom format to %d\n", textureOverride->format);
		desc->Format = (DXGI_FORMAT) textureOverride->format;
	}

	if (textureOverride->width != -1) {
		LogInfo("  setting custom width to %d\n", textureOverride->width);
		desc->Width = textureOverride->width;
	}

	if (textureOverride->width_multiply != 1.0f) {
		desc->Width = (UINT)(desc->Width * textureOverride->width_multiply);
		LogInfo("  multiplying custom width by %f to %d\n", textureOverride->width_multiply, desc->Width);
	}

	if (textureOverride->height != -1) {
		LogInfo("  setting custom height to %d\n", textureOverride->height);
		desc->Height = textureOverride->height;
	}

	if (textureOverride->height_multiply != 1.0f) {
		desc->Height = (UINT)(desc->Height * textureOverride->height_multiply);
		LogInfo("  multiplying custom height by %f to %d\n", textureOverride->height_multiply, desc->Height);
	}
}

static void override_resource_desc(D3D11_BUFFER_DESC *desc, TextureOverride *textureOverride) {}
static void override_resource_desc(D3D11_TEXTURE1D_DESC *desc, TextureOverride *textureOverride) {}
static void override_resource_desc(D3D11_TEXTURE2D_DESC *desc, TextureOverride *textureOverride)
{
	override_resource_desc_common_2d_3d(desc, textureOverride);
}
static void override_resource_desc(D3D11_TEXTURE3D_DESC *desc, TextureOverride *textureOverride)
{
	override_resource_desc_common_2d_3d(desc, textureOverride);
}

template <typename DescType>
static const DescType* process_texture_override(uint32_t hash,
		StereoHandle mStereoHandle,
		const DescType *origDesc,
		DescType *newDesc,
		NVAPI_STEREO_SURFACECREATEMODE *oldMode)
{
	NVAPI_STEREO_SURFACECREATEMODE newMode = (NVAPI_STEREO_SURFACECREATEMODE) -1;
	TextureOverrideMatches matches;
	TextureOverride *textureOverride = NULL;
	const DescType* ret = origDesc;
	unsigned i;

	*oldMode = (NVAPI_STEREO_SURFACECREATEMODE) -1;

	// Check for square surfaces. We used to do this after processing the
	// StereoMode in TextureOverrides, but realistically we always want the
	// TextureOverrides to be able to override this since they are more
	// specific, so now we do this first.
	if (is_square_surface(origDesc))
		newMode = (NVAPI_STEREO_SURFACECREATEMODE) G->gSurfaceSquareCreateMode;

	find_texture_overrides(hash, origDesc, &matches, NULL);

	if (origDesc && !matches.empty()) {
		// There is at least one matching texture override, which means
		// we may possibly be altering the resource description. Make a
		// copy of it and adjust the return pointer to the copy:
		*newDesc = *origDesc;
		ret = newDesc;

		// We go through each matching texture override applying any
		// resource description and stereo mode overrides. The texture
		// overrides with higher priorities come later in the list, so
		// if there are any conflicts they will override the earlier
		// lower priority ones.
		for (i = 0; i < matches.size(); i++) {
			textureOverride = matches[i];

			if (LogFile) {
				char buf[256];
				StrResourceDesc(buf, 256, origDesc);
				LogInfo("  %S matched resource with hash=%08x %s\n",
						textureOverride->ini_section.c_str(), hash, buf);
			}

			if (!check_texture_override_iteration(textureOverride))
				continue;

			if (textureOverride->stereoMode != -1)
				newMode = (NVAPI_STEREO_SURFACECREATEMODE) textureOverride->stereoMode;

			override_resource_desc(newDesc, textureOverride);
		}
	}

	LockResourceCreationMode();

	if (newMode != (NVAPI_STEREO_SURFACECREATEMODE) -1) {
		Profiling::NvAPI_Stereo_GetSurfaceCreationMode(mStereoHandle, oldMode);
		NvAPIOverride();
		LogInfo("    setting custom surface creation mode %d\n", newMode);

		if (NVAPI_OK != Profiling::NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle, newMode))
			LogInfo("      call failed.\n");
	}

	return ret;
}

static void restore_old_surface_create_mode(NVAPI_STEREO_SURFACECREATEMODE oldMode, StereoHandle mStereoHandle)
{
	if (oldMode != (NVAPI_STEREO_SURFACECREATEMODE) - 1) {
		if (NVAPI_OK != Profiling::NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle, oldMode))
			LogInfo("    restore call failed.\n");
	}

	UnlockResourceCreationMode();
}

STDMETHODIMP HackerDevice::CreateBuffer(THIS_
	/* [annotation] */
	__in  const D3D11_BUFFER_DESC *pDesc,
	/* [annotation] */
	__in_opt  const D3D11_SUBRESOURCE_DATA *pInitialData,
	/* [annotation] */
	__out_opt  ID3D11Buffer **ppBuffer)
{
	D3D11_BUFFER_DESC newDesc;
	const D3D11_BUFFER_DESC *pNewDesc = NULL;
	NVAPI_STEREO_SURFACECREATEMODE oldMode;

	LogDebug("HackerDevice::CreateBuffer called\n");
	if (pDesc)
		LogDebugResourceDesc(pDesc);

	// Create hash from the raw buffer data if available, but also include
	// the pDesc data as a unique fingerprint for a buffer.
	uint32_t data_hash = 0, hash = 0;
	if (pInitialData && pInitialData->pSysMem && pDesc)
		hash = data_hash = crc32c_hw(hash, pInitialData->pSysMem, pDesc->ByteWidth);
	if (pDesc)
		hash = crc32c_hw(hash, pDesc, sizeof(D3D11_BUFFER_DESC));

	// Override custom settings?
	pNewDesc = process_texture_override(hash, mStereoHandle, pDesc, &newDesc, &oldMode);

	HRESULT hr = mOrigDevice1->CreateBuffer(pNewDesc, pInitialData, ppBuffer);
	restore_old_surface_create_mode(oldMode, mStereoHandle);
	if (hr == S_OK && ppBuffer && *ppBuffer)
	{
		EnterCriticalSectionPretty(&G->mResourcesLock);
			ResourceHandleInfo *handle_info = &G->mResources[*ppBuffer];
			new ResourceReleaseTracker(*ppBuffer);
			handle_info->type = D3D11_RESOURCE_DIMENSION_BUFFER;
			handle_info->hash = hash;
			handle_info->orig_hash = hash;
			handle_info->data_hash = data_hash;

			// XXX: This is only used for hash tracking, which we
			// don't enable for buffers for performance reasons:
			// if (pDesc)
			//	memcpy(&handle_info->descBuf, pDesc, sizeof(D3D11_BUFFER_DESC));

		LeaveCriticalSection(&G->mResourcesLock);
		EnterCriticalSectionPretty(&G->mCriticalSection);
			// For stat collection and hash contamination tracking:
			if (G->hunting && pDesc) {
				G->mResourceInfo[hash] = *pDesc;
				G->mResourceInfo[hash].initial_data_used_in_hash = !!data_hash;
			}
		LeaveCriticalSection(&G->mCriticalSection);
	}
	return hr;
}

STDMETHODIMP HackerDevice::CreateTexture1D(THIS_
	/* [annotation] */
	__in  const D3D11_TEXTURE1D_DESC *pDesc,
	/* [annotation] */
	__in_xcount_opt(pDesc->MipLevels * pDesc->ArraySize)  const D3D11_SUBRESOURCE_DATA *pInitialData,
	/* [annotation] */
	__out_opt  ID3D11Texture1D **ppTexture1D)
{
	D3D11_TEXTURE1D_DESC newDesc;
	const D3D11_TEXTURE1D_DESC *pNewDesc = NULL;
	NVAPI_STEREO_SURFACECREATEMODE oldMode;
	uint32_t data_hash, hash;

	LogDebug("HackerDevice::CreateTexture1D called\n");
	if (pDesc)
		LogDebugResourceDesc(pDesc);

	hash = data_hash = CalcTexture1DDataHash(pDesc, pInitialData);
	if (pDesc)
		hash = crc32c_hw(hash, pDesc, sizeof(D3D11_TEXTURE1D_DESC));
	LogDebug("  InitialData = %p, hash = %08lx\n", pInitialData, hash);

	// Override custom settings?
	pNewDesc = process_texture_override(hash, mStereoHandle, pDesc, &newDesc, &oldMode);

	HRESULT hr = mOrigDevice1->CreateTexture1D(pNewDesc, pInitialData, ppTexture1D);

	restore_old_surface_create_mode(oldMode, mStereoHandle);

	if (hr == S_OK && ppTexture1D && *ppTexture1D)
	{
		EnterCriticalSectionPretty(&G->mResourcesLock);
			ResourceHandleInfo *handle_info = &G->mResources[*ppTexture1D];
			new ResourceReleaseTracker(*ppTexture1D);
			handle_info->type = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
			handle_info->hash = hash;
			handle_info->orig_hash = hash;
			handle_info->data_hash = data_hash;

			// TODO: For hash tracking if we ever need it for Texture1Ds:
			// if (pDesc)
			// 	memcpy(&handle_info->desc1D, pDesc, sizeof(D3D11_TEXTURE1D_DESC));
		LeaveCriticalSection(&G->mResourcesLock);
		EnterCriticalSectionPretty(&G->mCriticalSection);

			// For stat collection and hash contamination tracking:
			if (G->hunting && pDesc) {
				G->mResourceInfo[hash] = *pDesc;
				G->mResourceInfo[hash].initial_data_used_in_hash = !!data_hash;
			}
		LeaveCriticalSection(&G->mCriticalSection);
	}
	return hr;
}

static bool heuristic_could_be_possible_resolution(unsigned width, unsigned height)
{
	// Exclude very small resolutions:
	if (width < 640 || height < 480)
		return false;

	// Assume square textures are not a resolution, like 3D Vision:
	if (width == height)
		return false;

	// Special case for WATCH_DOGS2 1.09.154 update, which creates 16384 x 4096
	// shadow maps on ultra that are mistaken for the resolution. I don't
	// think that 4 is ever a valid aspect radio, so exclude it:
	if (width == height * 4)
		return false;

	return true;
}

// Metro's renderer-wide resolution setter at metro.exe+0x839540 writes the
// actual scene dimensions to +0xD044B0/+0xD044B4 and rebuilds all dependent
// viewport/aspect state. First-allocation activation calls it as soon as the
// native scene canvas exists, and the hook keeps later resets at that scale.
typedef void (*tMetroSetRenderResolution)(unsigned width, unsigned height);
static tMetroSetRenderResolution trampoline_MetroSetRenderResolution = NULL;

static void Hooked_MetroSetRenderResolution(unsigned width, unsigned height)
{
	static volatile LONG logged = 0;
	const bool shouldLog = InterlockedIncrement(&logged) <= 24;
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	BYTE *caller = (BYTE *)_ReturnAddress();
	const SIZE_T callerRva = base && caller >= base ? (SIZE_T)(caller - base) : 0;
	unsigned appliedInputWidth = width;
	unsigned appliedInputHeight = height;
	// These are the two confirmed callers that pass the full stereo scene
	// canvas. Adjacent calls pass the base/menu canvas and remain unchanged.
	const bool fullSceneCaller = callerRva == 0x83AF8B ||
		callerRva == 0x83BAD7;
	if (fullSceneCaller)
		VRMenu::SetDetectedSceneResolution(width, height);
	const float scale = VRMenu::EffectiveSceneResolutionScale();
	if (scale > 1.0001f && fullSceneCaller) {
		appliedInputWidth = max(1u, (unsigned)floorf(width * scale + 0.5f));
		appliedInputHeight = max(1u, (unsigned)floorf(height * scale + 0.5f));
	}
	if (shouldLog)
		LogInfo("VR resolution setter: requested=%ux%u scale=%.4f applied-input=%ux%u "
			"caller=metro.exe+0x%zX\n", width, height, scale,
			appliedInputWidth, appliedInputHeight, callerRva);
	trampoline_MetroSetRenderResolution(appliedInputWidth, appliedInputHeight);
	if (shouldLog) {
		unsigned appliedWidth = 0, appliedHeight = 0;
		__try {
			appliedWidth = *(unsigned *)(base + 0xD044B0);
			appliedHeight = *(unsigned *)(base + 0xD044B4);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		LogInfo("VR resolution setter: applied=%ux%u\n",
			appliedWidth, appliedHeight);
	}
}

static void InstallMetroResolutionSetterDiagnostic()
{
	static volatile LONG tried = 0;
	if (InterlockedCompareExchange(&tried, 1, 0) != 0)
		return;
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	if (!base)
		return;
	BYTE *fn = base + 0x839540;
	static const BYTE expected[] = {
		0x48, 0x89, 0x5C, 0x24, 0x18, // mov [rsp+18],rbx
		0x48, 0x89, 0x74, 0x24, 0x20, // mov [rsp+20],rsi
		0x57, 0x48, 0x83, 0xEC, 0x70  // push rdi; sub rsp,70
	};
	if (memcmp(fn, expected, sizeof(expected)) != 0) {
		LogInfo("VR resolution setter: signature mismatch at +0x839540\n");
		return;
	}
	SIZE_T hookId = 0;
	const DWORD err = cHookMgr.Hook(&hookId,
		(LPVOID *)&trampoline_MetroSetRenderResolution, fn,
		Hooked_MetroSetRenderResolution);
	if (err || !trampoline_MetroSetRenderResolution) {
		LogInfo("VR resolution setter: diagnostic hook FAILED (err=0x%x)\n", err);
		return;
	}
	LogInfo("VR resolution setter: startup scene-scale hook installed at +0x839540\n");
}

static volatile LONG sMetroSceneScaleActivated = 0;

static bool IsMetroSceneDepthAllocation(const D3D11_TEXTURE2D_DESC *desc,
	unsigned *sceneWidth, unsigned *sceneHeight)
{
	if (!desc || desc->Format != DXGI_FORMAT_R24G8_TYPELESS ||
		(desc->BindFlags & (D3D11_BIND_SHADER_RESOURCE |
			D3D11_BIND_DEPTH_STENCIL)) !=
			(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL))
		return false;
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	if (!base)
		return false;
	unsigned width = 0, height = 0;
	__try {
		width = *(unsigned *)(base + 0xD044B0);
		height = *(unsigned *)(base + 0xD044B4);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (width < 640 || height < 360 || desc->Width != width ||
		desc->Height != height)
		return false;
	const float aspect = (float)width / (float)height;
	if (aspect < 1.4f || aspect > 2.2f)
		return false;
	if (sceneWidth) *sceneWidth = width;
	if (sceneHeight) *sceneHeight = height;
	return true;
}

STDMETHODIMP HackerDevice::CreateTexture2D(THIS_
	/* [annotation] */
	__in  const D3D11_TEXTURE2D_DESC *pDesc,
	/* [annotation] */
	__in_xcount_opt(pDesc->MipLevels * pDesc->ArraySize)  const D3D11_SUBRESOURCE_DATA *pInitialData,
	/* [annotation] */
	__out_opt  ID3D11Texture2D **ppTexture2D)
{
	D3D11_TEXTURE2D_DESC newDesc;
	const D3D11_TEXTURE2D_DESC *pNewDesc = NULL;
	NVAPI_STEREO_SURFACECREATEMODE oldMode;
	unsigned detectedSceneWidth = 0, detectedSceneHeight = 0;
	if (IsMetroSceneDepthAllocation(pDesc, &detectedSceneWidth,
		&detectedSceneHeight)) {
		InstallMetroResolutionSetterDiagnostic();
		const float requestedScale = VRMenu::ResolutionScale();
		// If the hook could not be installed, leave scales above 1x inactive.
		// The effective-scale guard then keeps a readable native fallback rather
		// than applying UI/high-resolution assumptions to an unscaled canvas.
		if (requestedScale <= 1.0001f || trampoline_MetroSetRenderResolution)
			VRMenu::SetDetectedSceneResolution(detectedSceneWidth,
				detectedSceneHeight);
		const float scale = VRMenu::EffectiveSceneResolutionScale();
		if (InterlockedCompareExchange(&sMetroSceneScaleActivated, 1, 0) == 0
			&& scale > 1.0001f && trampoline_MetroSetRenderResolution) {
			const unsigned scaledWidth = max(1u,
				(unsigned)floorf(detectedSceneWidth * scale + 0.5f));
			const unsigned scaledHeight = max(1u,
				(unsigned)floorf(detectedSceneHeight * scale + 0.5f));
			LogInfo("VR resolution first-allocation activation: %ux%u scale=%.4f -> "
				"%ux%u\n", detectedSceneWidth, detectedSceneHeight, scale,
				scaledWidth, scaledHeight);
			// The initial renderer setup happens before d3d11.dll is loaded, so
			// the first scene allocation is the earliest safe interception point.
			// Rebuild Metro's viewport/aspect state before creating this resource.
			trampoline_MetroSetRenderResolution(scaledWidth, scaledHeight);
		}
	}
	if (kResolutionDiagnosticsEnabled && pDesc &&
		pDesc->Width >= 2000 && pDesc->Height >= 1000) {
		const float aspect = (float)pDesc->Width / (float)pDesc->Height;
		if (aspect > 1.4f && aspect < 2.2f &&
			(pDesc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
			static UINT lastWidth = 0, lastHeight = 0;
			if (pDesc->Width != lastWidth || pDesc->Height != lastHeight) {
				lastWidth = pDesc->Width;
				lastHeight = pDesc->Height;
				LogInfo("VR resolution diagnostic: internal target %ux%u fmt=%d bind=0x%x misc=0x%x\n",
					pDesc->Width, pDesc->Height, (int)pDesc->Format,
					pDesc->BindFlags, pDesc->MiscFlags);
			}
		}
	}

	LogDebug("HackerDevice::CreateTexture2D called with parameters\n");
	if (pDesc)
		LogDebugResourceDesc(pDesc);
	if (pInitialData && pInitialData->pSysMem)
	{
		LogDebugNoNL("  pInitialData = %p->%p, SysMemPitch: %u, SysMemSlicePitch: %u ",
			pInitialData, pInitialData->pSysMem, pInitialData->SysMemPitch, pInitialData->SysMemSlicePitch);
		const uint8_t* hex = static_cast<const uint8_t*>(pInitialData->pSysMem);
		for (size_t i = 0; i < 16; i++)
			LogDebugNoNL(" %02hX", hex[i]);
		LogDebug("\n");
	}

	// Rectangular depth stencil textures of at least 640x480 may indicate
	// the game's resolution, for games that upscale to their swap chains:
	if (pDesc &&
		(pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) &&
		G->mResolutionInfo.from == GetResolutionFrom::DEPTH_STENCIL &&
		heuristic_could_be_possible_resolution(pDesc->Width, pDesc->Height))
	{
		G->mResolutionInfo.width = pDesc->Width;
		G->mResolutionInfo.height = pDesc->Height;
		LogInfo("Got resolution from depth/stencil buffer: %ix%i\n",
			G->mResolutionInfo.width, G->mResolutionInfo.height);
	}

	// Hash based on raw texture data
	// TODO: Wrap these texture objects and return them to the game.
	//  That would avoid the hash lookup later.

	// We are using both pDesc and pInitialData if it exists.  Even in the 
	// pInitialData=0 case, we still need to make a hash, as these are often
	// hashes that are created on the fly, filled in later. So, even though all
	// we have to go on is the easily duplicated pDesc, we'll still use it and
	// accept that we might get collisions.

	// Also, we see duplicate hashes happen, sort-of collisions.  These don't
	// happen because of hash miscalculation, they are literally exactly the
	// same data. Like a fully black texture screen size, shows up multiple times
	// and calculates to same hash.
	// We also see the handle itself get reused. That suggests that maybe we ought
	// to be tracking Release operations as well, and removing them from the map.

	uint32_t data_hash, hash;
	hash = data_hash = CalcTexture2DDataHash(pDesc, pInitialData);
	if (pDesc)
		hash = CalcTexture2DDescHash(hash, pDesc);
	LogDebug("  InitialData = %p, hash = %08lx\n", pInitialData, hash);

	// Override custom settings?
	pNewDesc = process_texture_override(hash, mStereoHandle, pDesc, &newDesc, &oldMode);

	// The descriptor for the allocation that triggered activation was built
	// before Metro's setter ran. Resize it here; subsequent descriptors normally
	// arrive already scaled because the engine globals have been updated. Also
	// handle any cached native-size descriptors Metro submits later.
	D3D11_TEXTURE2D_DESC vrScaleDesc;
	const D3D11_TEXTURE2D_DESC *candidateDesc = pNewDesc ? pNewDesc : pDesc;
	if (candidateDesc && VRMenu::IsBaseSceneResolution(
		candidateDesc->Width, candidateDesc->Height)
		&& InterlockedCompareExchange(&sMetroSceneScaleActivated, 0, 0) != 0) {
		const float scale = VRMenu::EffectiveSceneResolutionScale();
		if (scale > 1.0001f) {
			vrScaleDesc = *candidateDesc;
			vrScaleDesc.Width = max(1u,
				(unsigned)floorf(candidateDesc->Width * scale + 0.5f));
			vrScaleDesc.Height = max(1u,
				(unsigned)floorf(candidateDesc->Height * scale + 0.5f));
			pNewDesc = &vrScaleDesc;
			LogInfo("VR resolution texture scale: %ux%u -> %ux%u fmt=%d bind=0x%x\n",
				candidateDesc->Width, candidateDesc->Height,
				vrScaleDesc.Width, vrScaleDesc.Height,
				(int)vrScaleDesc.Format, vrScaleDesc.BindFlags);
		}
	}

	// Metro2033ReduxVR true stereo: if the two eyes disagree about this
	// target, give it a second array slice to hold the other eye. Done here,
	// before creation, because a texture's array size cannot be changed
	// afterwards - and one draw can fill two slices of one resource, which is
	// what single-pass stereo needs and two separate textures can never offer.
	D3D11_TEXTURE2D_DESC vrDesc;
	if (pNewDesc ? pNewDesc : pDesc) {
		vrDesc = *(pNewDesc ? pNewDesc : pDesc);
		if (StereoTwin::WidenDescForStereo(&vrDesc)) {
			pNewDesc = &vrDesc;
			pInitialData = NULL;   // no initial data for a render target
		}
	}
	const bool vrWidened = (pNewDesc == &vrDesc);

	// Actual creation:
	HRESULT hr = mOrigDevice1->CreateTexture2D(pNewDesc, pInitialData, ppTexture2D);
	restore_old_surface_create_mode(oldMode, mStereoHandle);
	if (ppTexture2D) LogDebug("  returns result = %x, handle = %p\n", hr, *ppTexture2D);

	// Register the twin - slice 1 of what was just created, or a separate
	// texture in the fallback path - before any view over the original exists.
	if (hr == S_OK && ppTexture2D)
		StereoTwin::OnCreateTexture2D(mOrigDevice1, pNewDesc ? pNewDesc : pDesc, *ppTexture2D, vrWidened);

	// Register texture. Every one seen.
	if (hr == S_OK && ppTexture2D)
	{
		EnterCriticalSectionPretty(&G->mResourcesLock);
			ResourceHandleInfo *handle_info = &G->mResources[*ppTexture2D];
			new ResourceReleaseTracker(*ppTexture2D);
			handle_info->type = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
			handle_info->hash = hash;
			handle_info->orig_hash = hash;
			handle_info->data_hash = data_hash;
			if (pDesc)
				memcpy(&handle_info->desc2D, pDesc, sizeof(D3D11_TEXTURE2D_DESC));
		LeaveCriticalSection(&G->mResourcesLock);
		EnterCriticalSectionPretty(&G->mCriticalSection);
			if (G->hunting && pDesc) {
				G->mResourceInfo[hash] = *pDesc;
				G->mResourceInfo[hash].initial_data_used_in_hash = !!data_hash;
			}
		LeaveCriticalSection(&G->mCriticalSection);
	}

	return hr;
}

STDMETHODIMP HackerDevice::CreateTexture3D(THIS_
	/* [annotation] */
	__in  const D3D11_TEXTURE3D_DESC *pDesc,
	/* [annotation] */
	__in_xcount_opt(pDesc->MipLevels)  const D3D11_SUBRESOURCE_DATA *pInitialData,
	/* [annotation] */
	__out_opt  ID3D11Texture3D **ppTexture3D)
{
	D3D11_TEXTURE3D_DESC newDesc;
	const D3D11_TEXTURE3D_DESC *pNewDesc = NULL;
	NVAPI_STEREO_SURFACECREATEMODE oldMode;

	LogInfo("HackerDevice::CreateTexture3D called with parameters\n");
	if (pDesc)
		LogDebugResourceDesc(pDesc);
	if (pInitialData && pInitialData->pSysMem) {
		LogInfo("  pInitialData = %p->%p, SysMemPitch: %u, SysMemSlicePitch: %u\n",
			pInitialData, pInitialData->pSysMem, pInitialData->SysMemPitch, pInitialData->SysMemSlicePitch);
	}

	// Rectangular depth stencil textures of at least 640x480 may indicate
	// the game's resolution, for games that upscale to their swap chains:
	if (pDesc && (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) &&
		G->mResolutionInfo.from == GetResolutionFrom::DEPTH_STENCIL &&
		heuristic_could_be_possible_resolution(pDesc->Width, pDesc->Height)) {
		G->mResolutionInfo.width = pDesc->Width;
		G->mResolutionInfo.height = pDesc->Height;
		LogInfo("Got resolution from depth/stencil buffer: %ix%i\n",
			G->mResolutionInfo.width, G->mResolutionInfo.height);
	}

	// Create hash code from raw texture data and description.
	// Initial data is optional, so we might have zero data to add to the hash there.
	uint32_t data_hash, hash;
	hash = data_hash = CalcTexture3DDataHash(pDesc, pInitialData);
	if (pDesc)
		hash = CalcTexture3DDescHash(hash, pDesc);
	LogInfo("  InitialData = %p, hash = %08lx\n", pInitialData, hash);

	// Override custom settings?
	pNewDesc = process_texture_override(hash, mStereoHandle, pDesc, &newDesc, &oldMode);

	HRESULT hr = mOrigDevice1->CreateTexture3D(pNewDesc, pInitialData, ppTexture3D);

	restore_old_surface_create_mode(oldMode, mStereoHandle);

	// Register texture.
	if (hr == S_OK && ppTexture3D)
	{
		EnterCriticalSectionPretty(&G->mResourcesLock);
			ResourceHandleInfo *handle_info = &G->mResources[*ppTexture3D];
			new ResourceReleaseTracker(*ppTexture3D);
			handle_info->type = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
			handle_info->hash = hash;
			handle_info->orig_hash = hash;
			handle_info->data_hash = data_hash;
			if (pDesc)
				memcpy(&handle_info->desc3D, pDesc, sizeof(D3D11_TEXTURE3D_DESC));
		LeaveCriticalSection(&G->mResourcesLock);
		EnterCriticalSectionPretty(&G->mCriticalSection);
			if (G->hunting && pDesc) {
				G->mResourceInfo[hash] = *pDesc;
				G->mResourceInfo[hash].initial_data_used_in_hash = !!data_hash;
			}
		LeaveCriticalSection(&G->mCriticalSection);
	}

	LogInfo("  returns result = %x\n", hr);

	return hr;
}

STDMETHODIMP HackerDevice::CreateShaderResourceView(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pResource,
	/* [annotation] */
	__in_opt  const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
	/* [annotation] */
	__out_opt  ID3D11ShaderResourceView **ppSRView)
{
	LogDebug("HackerDevice::CreateShaderResourceView called\n");

	D3D11_SHADER_RESOURCE_VIEW_DESC vrDesc;
	pDesc = StereoTwin::NarrowSRVDesc(pResource, pDesc, &vrDesc);

	HRESULT hr = mOrigDevice1->CreateShaderResourceView(pResource, pDesc, ppSRView);

	if (hr == S_OK && ppSRView)
		StereoTwin::OnCreateSRV(mOrigDevice1, pResource, pDesc, *ppSRView);

	// Check for depth buffer view.
	if (hr == S_OK && G->ZBufferHashToInject && ppSRView)
	{
		EnterCriticalSectionPretty(&G->mResourcesLock);
		unordered_map<ID3D11Resource *, ResourceHandleInfo>::iterator i = lookup_resource_handle_info(pResource);
		if (i != G->mResources.end() && i->second.hash == G->ZBufferHashToInject)
		{
			LogInfo("  resource view of z buffer found: handle = %p, hash = %08lx\n", *ppSRView, i->second.hash);

			mZBufferResourceView = *ppSRView;
		}
		LeaveCriticalSection(&G->mResourcesLock);
	}

	LogDebug("  returns result = %x\n", hr);

	return hr;
}

// Whitelist bytecode sections for the bytecode hash. This should include any
// section that clearly makes the shader different from another near identical
// shader such that they are not compatible with one another, such as the
// bytecode itself as well as the input/output/patch constant signatures.
//
// It should not include metadata that might change for a reason other than the
// shader being changed. In particular, it should not include the compiler
// version (in the RDEF section), which may change if the developer upgrades
// their build environment, or debug information that includes the directory on
// the developer's machine that the shader was compiled from (in the SDBG
// section). The STAT section is also intentionally not included because it
// contains nothing useful.
//
// The RDEF section may arguably be useful to compromise between this and a
// hash of the entire shader - it includes the compiler version, which makes it
// a bad idea to hash, BUT it also includes the reflection information such as
// variable names which arguably might be useful to distinguish between
// otherwise identical shaders. However I don't think there is much advantage
// of that over just hashing the full shader, and in some cases we might like
// to ignore variable name changes, so it seems best to skip it.
static char* hash_whitelisted_sections[] = {
	"SHDR", "SHEX",         // Bytecode
	"ISGN",         "ISG1", // Input signature
	"PCSG",         "PSG1", // Patch constant signature
	"OSGN", "OSG5", "OSG1", // Output signature
};

static uint32_t hash_shader_bytecode(struct dxbc_header *header, SIZE_T BytecodeLength)
{
	uint32_t *offsets = (uint32_t*)((char*)header + sizeof(struct dxbc_header));
	struct section_header *section;
	unsigned i, j;
	uint32_t hash = 0;

	if (BytecodeLength < sizeof(struct dxbc_header) + header->num_sections*sizeof(uint32_t))
		return 0;

	for (i = 0; i < header->num_sections; i++) {
		section = (struct section_header*)((char*)header + offsets[i]);
		if (BytecodeLength < (char*)section - (char*)header + sizeof(struct section_header) + section->size)
			return 0;

		for (j = 0; j < ARRAYSIZE(hash_whitelisted_sections); j++) {
			if (!strncmp(section->signature, hash_whitelisted_sections[j], 4))
				hash = crc32c_hw(hash, (char*)section + sizeof(struct section_header), section->size);
		}
	}

	return hash;
}

static UINT64 hash_shader(const void *pShaderBytecode, SIZE_T BytecodeLength)
{
	UINT64 hash = 0;
	struct dxbc_header *header = (struct dxbc_header *)pShaderBytecode;

	if (BytecodeLength < sizeof(struct dxbc_header))
		goto fnv;

	switch (G->shader_hash_type) {
		case ShaderHashType::FNV:
fnv:
			hash = fnv_64_buf(pShaderBytecode, BytecodeLength);
			LogInfo("       FNV hash = %016I64x\n", hash);
			break;

		case ShaderHashType::EMBEDDED:
			// Confirmed with dx11shaderanalyse that the hash
			// embedded in the file is as md5sum would have printed
			// it (that is - if md5sum used the same obfuscated
			// message size padding), so read it as big-endian so
			// that we print it the same way for consistency.
			//
			// Endian bug: _byteswap_uint64 is unconditional, but I
			// don't want to pull in all of winsock just for ntohl,
			// and since we are only targetting x86... meh.
			hash = _byteswap_uint64(header->hash[0] | (UINT64)header->hash[1] << 32);
			LogInfo("  Embedded hash = %016I64x\n", hash);
			break;

		case ShaderHashType::BYTECODE:
			hash = hash_shader_bytecode(header, BytecodeLength);
			if (!hash)
				goto fnv;
			LogInfo("  Bytecode hash = %016I64x\n", hash);
			break;
	}

	return hash;
}


// C++ function template of common code shared by all CreateXXXShader functions:
template <class ID3D11Shader,
	 HRESULT (__stdcall ID3D11Device::*OrigCreateShader)(THIS_
			 __in const void *pShaderBytecode,
			 __in SIZE_T BytecodeLength,
			 __in_opt ID3D11ClassLinkage *pClassLinkage,
			 __out_opt ID3D11Shader **ppShader)
	 >
STDMETHODIMP HackerDevice::CreateShader(THIS_
	/* [annotation] */
	__in  const void *pShaderBytecode,
	/* [annotation] */
	__in  SIZE_T BytecodeLength,
	/* [annotation] */
	__in_opt  ID3D11ClassLinkage *pClassLinkage,
	/* [annotation] */
	__out_opt  ID3D11Shader **ppShader,
	wchar_t *shaderType)
{
	HRESULT hr;
	UINT64 hash;

	if (!ppShader || !pShaderBytecode) {
		// Let DX worry about the error code
		return (mOrigDevice1->*OrigCreateShader)(pShaderBytecode, BytecodeLength, pClassLinkage, ppShader);
	}

	// Calculate hash
	hash = hash_shader(pShaderBytecode, BytecodeLength);

	hr = ReplaceShaderFromShaderFixes<ID3D11Shader, OrigCreateShader>
		(hash, pShaderBytecode, BytecodeLength, pClassLinkage,
		 ppShader, shaderType);

	if (hr != S_OK) {
		hr = ProcessShaderNotFoundInShaderFixes<ID3D11Shader, OrigCreateShader>
			(hash, pShaderBytecode, BytecodeLength, pClassLinkage,
			 ppShader, shaderType);
	}

	if (hr == S_OK) {
		EnterCriticalSectionPretty(&G->mCriticalSection);
			G->mShaders[*ppShader] = hash;
			LogDebugW(L"    %ls: handle = %p, hash = %016I64x\n", shaderType, *ppShader, hash);
		LeaveCriticalSection(&G->mCriticalSection);
	}

	LogInfo("  returns result = %x, handle = %p\n", hr, *ppShader);

	return hr;
}

STDMETHODIMP HackerDevice::CreateVertexShader(THIS_
	/* [annotation] */
	__in  const void *pShaderBytecode,
	/* [annotation] */
	__in  SIZE_T BytecodeLength,
	/* [annotation] */
	__in_opt  ID3D11ClassLinkage *pClassLinkage,
	/* [annotation] */
	__out_opt  ID3D11VertexShader **ppVertexShader)
{
	LogInfo("HackerDevice::CreateVertexShader called with BytecodeLength = %Iu, handle = %p, ClassLinkage = %p\n", BytecodeLength, pShaderBytecode, pClassLinkage);

	HRESULT hr = CreateShader<ID3D11VertexShader, &ID3D11Device::CreateVertexShader>
			(pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader, L"vs");

	// Metro2033ReduxVR single-pass stereo: build a stereo variant of this
	// shader now, while its bytecode is in hand.
	//
	// Done at creation rather than from a folder of pre-patched files. That
	// approach was tried and only covered the shaders the game happened to
	// load while we were dumping - correct in the rooms we tested, silently
	// wrong everywhere else, which is disqualifying for a release. Patching
	// here catches every shader the game will ever create.
	if (SUCCEEDED(hr) && ppVertexShader && *ppVertexShader)
		StereoSinglePass::OnCreateVertexShader(mOrigDevice1, pShaderBytecode,
			BytecodeLength, *ppVertexShader);

	// TASK 27: dump the viewmodel's vertex shaders, once each.
	//
	// The weapon's positions turned out to be a packed short4 - the raw vertex
	// dump in HackerContext's mesh measurement settled that - and the factor
	// that turns those integers into metres lives in whatever decodes them.
	// Guessing it would put this straight back to tuning by eye, which is the
	// one thing this task must not do, so read it out of the shader instead.
	//
	// These are the hashes seen drawing viewmodel meshes, logged by the
	// "viewmodel drawn by VS" line. Hardcoded because the identification has
	// already happened and a list of six is cheaper than plumbing the draw-time
	// verdict back to shader creation.
	if (SUCCEEDED(hr) && ppVertexShader && *ppVertexShader) {
		static const UINT64 kViewmodelVS[] = {
			0x79BA9F1ECFAF0CA1ull, 0x93EF04911A6F1F6Dull, 0xB80CA200D7A77A0Bull,
			0x65C62A5148831C58ull, 0x9420C1D720E7C3B8ull, 0x03065006306540D5ull,
			// The muzzle flash, found by the firing-only draw diff: one
			// shader, one layout, index counts 234/462/738 - all multiples
			// of six, so a growing batch of quads. It draws in the WORLD
			// pass with a slot-1 layout belonging to neither known family,
			// so how it gets its positions decides whether the flash can be
			// moved with the weapon at all. Read it rather than guess.
			0xC5A6A1F5B984FBF0ull,
		};

		UINT64 hash = 0;
		EnterCriticalSection(&G->mCriticalSection);
		{
			ShaderMap::iterator i = G->mShaders.find(*ppVertexShader);
			if (i != G->mShaders.end())
				hash = i->second;
		}
		LeaveCriticalSection(&G->mCriticalSection);

		bool wanted = false;
		for (size_t i = 0; i < _countof(kViewmodelVS); i++)
			if (kViewmodelVS[i] == hash)
				wanted = true;

		if (wanted) {
			static std::set<UINT64> dumped;
			if (dumped.insert(hash).second) {
				std::string asmText = BinaryToAsmText(pShaderBytecode, BytecodeLength, false);
				char path[64];
				_snprintf_s(path, sizeof(path), _TRUNCATE, "vm_vs_%016llX.txt",
					(unsigned long long)hash);
				FILE *f = NULL;
				if (fopen_s(&f, path, "wb") == 0 && f) {
					if (!asmText.empty())
						fwrite(asmText.c_str(), 1, asmText.size(), f);
					fclose(f);
					LogInfo("VRPose mesh: dumped viewmodel VS %016llX to %s (%zu bytes)\n",
						(unsigned long long)hash, path, asmText.size());
				} else {
					LogInfo("VRPose mesh: could not write %s\n", path);
				}
			}
		}
	}

	return hr;
}

STDMETHODIMP HackerDevice::CreateGeometryShader(THIS_
	/* [annotation] */
	__in  const void *pShaderBytecode,
	/* [annotation] */
	__in  SIZE_T BytecodeLength,
	/* [annotation] */
	__in_opt  ID3D11ClassLinkage *pClassLinkage,
	/* [annotation] */
	__out_opt  ID3D11GeometryShader **ppGeometryShader)
{
	LogInfo("HackerDevice::CreateGeometryShader called with BytecodeLength = %Iu, handle = %p\n", BytecodeLength, pShaderBytecode);

	return CreateShader<ID3D11GeometryShader, &ID3D11Device::CreateGeometryShader>
			(pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader, L"gs");
}

STDMETHODIMP HackerDevice::CreateGeometryShaderWithStreamOutput(THIS_
	/* [annotation] */
	__in  const void *pShaderBytecode,
	/* [annotation] */
	__in  SIZE_T BytecodeLength,
	/* [annotation] */
	__in_ecount_opt(NumEntries)  const D3D11_SO_DECLARATION_ENTRY *pSODeclaration,
	/* [annotation] */
	__in_range(0, D3D11_SO_STREAM_COUNT * D3D11_SO_OUTPUT_COMPONENT_COUNT)  UINT NumEntries,
	/* [annotation] */
	__in_ecount_opt(NumStrides)  const UINT *pBufferStrides,
	/* [annotation] */
	__in_range(0, D3D11_SO_BUFFER_SLOT_COUNT)  UINT NumStrides,
	/* [annotation] */
	__in  UINT RasterizedStream,
	/* [annotation] */
	__in_opt  ID3D11ClassLinkage *pClassLinkage,
	/* [annotation] */
	__out_opt  ID3D11GeometryShader **ppGeometryShader)
{
	LogInfo("HackerDevice::CreateGeometryShaderWithStreamOutput called.\n");

	// TODO: This is another call that can create geometry and/or vertex
	// shaders - hook them up and allow them to be overridden as well.

	HRESULT hr = mOrigDevice1->CreateGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength, pSODeclaration,
		NumEntries, pBufferStrides, NumStrides, RasterizedStream, pClassLinkage, ppGeometryShader);
	LogInfo("  returns result = %x, handle = %p\n", hr, (ppGeometryShader ? *ppGeometryShader : NULL));

	return hr;
}

STDMETHODIMP HackerDevice::CreatePixelShader(THIS_
	/* [annotation] */
	__in  const void *pShaderBytecode,
	/* [annotation] */
	__in  SIZE_T BytecodeLength,
	/* [annotation] */
	__in_opt  ID3D11ClassLinkage *pClassLinkage,
	/* [annotation] */
	__out_opt  ID3D11PixelShader **ppPixelShader)
{
	LogInfo("HackerDevice::CreatePixelShader called with BytecodeLength = %Iu, handle = %p, ClassLinkage = %p\n", BytecodeLength, pShaderBytecode, pClassLinkage);

	HRESULT hr = CreateShader<ID3D11PixelShader, &ID3D11Device::CreatePixelShader>
			(pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader, L"ps");

	// Metro2033ReduxVR single-pass stereo: record which texture slots this
	// shader really samples, so a draw can be judged on what it reads rather
	// than on what happens to be left bound from an earlier pass.
	if (SUCCEEDED(hr) && ppPixelShader && *ppPixelShader)
		StereoSinglePass::OnCreatePixelShader(mOrigDevice1, pShaderBytecode,
			BytecodeLength, *ppPixelShader);

	return hr;
}

STDMETHODIMP HackerDevice::CreateHullShader(THIS_
	/* [annotation] */
	__in  const void *pShaderBytecode,
	/* [annotation] */
	__in  SIZE_T BytecodeLength,
	/* [annotation] */
	__in_opt  ID3D11ClassLinkage *pClassLinkage,
	/* [annotation] */
	__out_opt  ID3D11HullShader **ppHullShader)
{
	LogInfo("HackerDevice::CreateHullShader called with BytecodeLength = %Iu, handle = %p\n", BytecodeLength, pShaderBytecode);

	return CreateShader<ID3D11HullShader, &ID3D11Device::CreateHullShader>
			(pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader, L"hs");
}

STDMETHODIMP HackerDevice::CreateDomainShader(THIS_
	/* [annotation] */
	__in  const void *pShaderBytecode,
	/* [annotation] */
	__in  SIZE_T BytecodeLength,
	/* [annotation] */
	__in_opt  ID3D11ClassLinkage *pClassLinkage,
	/* [annotation] */
	__out_opt  ID3D11DomainShader **ppDomainShader)
{
	LogInfo("HackerDevice::CreateDomainShader called with BytecodeLength = %Iu, handle = %p\n", BytecodeLength, pShaderBytecode);

	return CreateShader<ID3D11DomainShader, &ID3D11Device::CreateDomainShader>
			(pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader, L"ds");
}

STDMETHODIMP HackerDevice::CreateComputeShader(THIS_
	/* [annotation] */
	__in  const void *pShaderBytecode,
	/* [annotation] */
	__in  SIZE_T BytecodeLength,
	/* [annotation] */
	__in_opt  ID3D11ClassLinkage *pClassLinkage,
	/* [annotation] */
	__out_opt  ID3D11ComputeShader **ppComputeShader)
{
	LogInfo("HackerDevice::CreateComputeShader called with BytecodeLength = %Iu, handle = %p\n", BytecodeLength, pShaderBytecode);

	return CreateShader<ID3D11ComputeShader, &ID3D11Device::CreateComputeShader>
			(pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader, L"cs");
}

STDMETHODIMP HackerDevice::CreateRasterizerState(THIS_
	/* [annotation] */
	__in const D3D11_RASTERIZER_DESC *pRasterizerDesc,
	/* [annotation] */
	__out_opt  ID3D11RasterizerState **ppRasterizerState)
{
	HRESULT hr;

	if (pRasterizerDesc) LogDebug("HackerDevice::CreateRasterizerState called with\n"
		"  FillMode = %d, CullMode = %d, DepthBias = %d, DepthBiasClamp = %f, SlopeScaledDepthBias = %f,\n"
		"  DepthClipEnable = %d, ScissorEnable = %d, MultisampleEnable = %d, AntialiasedLineEnable = %d\n",
		pRasterizerDesc->FillMode, pRasterizerDesc->CullMode, pRasterizerDesc->DepthBias, pRasterizerDesc->DepthBiasClamp,
		pRasterizerDesc->SlopeScaledDepthBias, pRasterizerDesc->DepthClipEnable, pRasterizerDesc->ScissorEnable,
		pRasterizerDesc->MultisampleEnable, pRasterizerDesc->AntialiasedLineEnable);

	if (G->SCISSOR_DISABLE && pRasterizerDesc && pRasterizerDesc->ScissorEnable)
	{
		LogDebug("  disabling scissor mode.\n");
		const_cast<D3D11_RASTERIZER_DESC*>(pRasterizerDesc)->ScissorEnable = FALSE;
	}

	hr = mOrigDevice1->CreateRasterizerState(pRasterizerDesc, ppRasterizerState);

	LogDebug("  returns result = %x\n", hr);
	return hr;
}

// This method creates a Context, and we want to return a wrapped/hacker
// version as the result. The method signature requires an 
// ID3D11DeviceContext, but we return our HackerContext.

// A deferred context is for multithreading part of the drawing. 

STDMETHODIMP HackerDevice::CreateDeferredContext(THIS_
	UINT ContextFlags,
	/* [annotation] */
	__out_opt  ID3D11DeviceContext **ppDeferredContext)
{
	LogInfo("HackerDevice::CreateDeferredContext(%s@%p) called with flags = %#x, ptr:%p\n", 
		type_name(this), this, ContextFlags, ppDeferredContext);

	HRESULT hr = mOrigDevice1->CreateDeferredContext(ContextFlags, ppDeferredContext);
	if (FAILED(hr))
	{
		LogInfo("  failed result = %x for %p\n", hr, ppDeferredContext);
		return hr;
	}

	if (ppDeferredContext)
	{
		analyse_iunknown(*ppDeferredContext);
		ID3D11DeviceContext1 *origContext1;
		HRESULT res = (*ppDeferredContext)->QueryInterface(IID_PPV_ARGS(&origContext1));
		if (FAILED(res))
			origContext1 = static_cast<ID3D11DeviceContext1*>(*ppDeferredContext);
		HackerContext *hackerContext = HackerContextFactory(mRealOrigDevice1, origContext1);
		hackerContext->SetHackerDevice(this);
		hackerContext->Bind3DMigotoResources();

		if (G->enable_hooks & EnableHooks::DEFERRED_CONTEXTS)
			hackerContext->HookContext();
		else
			*ppDeferredContext = hackerContext;

		LogInfo("  created HackerContext(%s@%p) wrapper of %p\n", type_name(hackerContext), hackerContext, origContext1);
	}

	LogInfo("  returns result = %x for %p\n", hr, *ppDeferredContext);
	return hr;
}

// A variant where we want to return a HackerContext instead of the
// real one.  Creating a new HackerContext is not correct here, because we 
// need to provide the one created originally with the device.

// This is a main way to get the context when you only have the device.
// There is only one immediate context per device, so if they are requesting
// it, we need to return them the HackerContext.
// 
// It is apparently possible for poorly written games to call this function
// with null as the ppImmediateContext. This not an optional parameter, and
// that call makes no sense, but apparently happens if they pass null to
// CreateDeviceAndSwapChain for ImmediateContext.  A bug in an older SDK.
// WatchDogs seems to do this. 
// 
// Also worth noting here is that by not calling through to GetImmediateContext
// we did not properly account for references.
// "The GetImmediateContext method increments the reference count of the immediate context by one. "
//
// Fairly common to see this called all the time, so switching to LogDebug for
// this as a way to trim down normal log.

STDMETHODIMP_(void) HackerDevice::GetImmediateContext(THIS_
	/* [annotation] */
	__out  ID3D11DeviceContext **ppImmediateContext)
{
	LogDebug("HackerDevice::GetImmediateContext(%s@%p) called with:%p\n", 
		type_name(this), this, ppImmediateContext);

	if (ppImmediateContext == nullptr)
	{
		LogInfo("  *** no return possible, nullptr input.\n");
		return;
	}

	// XXX: We might need to add locking here if one thread can call
	// GetImmediateContext() while another calls Release on the same
	// immediate context. Thought this might have been necessary to
	// eliminate a race in Far Cry 4, but that turned out to be due to the
	// HackerContext not having a link back to the HackerDevice, and the
	// same device was not being accessed from multiple threads so there
	// was no race.

	// We still need to call the original function to make sure the reference counts are correct:
	mOrigDevice1->GetImmediateContext(ppImmediateContext);

	// we can arrive here with no mHackerContext created if one was not
	// requested from CreateDevice/CreateDeviceFromSwapChain. In that case
	// we need to wrap the immediate context now:
	if (mHackerContext == nullptr)
	{
		LogInfo("*** HackerContext missing at HackerDevice::GetImmediateContext\n");

		analyse_iunknown(*ppImmediateContext);

		ID3D11DeviceContext1 *origContext1;
		HRESULT res = (*ppImmediateContext)->QueryInterface(IID_PPV_ARGS(&origContext1));
		if (FAILED(res))
			origContext1 = static_cast<ID3D11DeviceContext1*>(*ppImmediateContext);
		mHackerContext = HackerContextFactory(mRealOrigDevice1, origContext1);
		mHackerContext->SetHackerDevice(this);
		mHackerContext->Bind3DMigotoResources();
		if (!G->constants_run)
			mHackerContext->InitIniParams();
		if (G->enable_hooks & EnableHooks::IMMEDIATE_CONTEXT)
			mHackerContext->HookContext();
		LogInfo("  HackerContext %p created to wrap %p\n", mHackerContext, *ppImmediateContext);
	}
	else if (mHackerContext->GetPossiblyHookedOrigContext1() != *ppImmediateContext)
	{
		LogInfo("WARNING: mHackerContext %p found to be wrapping %p instead of %p at HackerDevice::GetImmediateContext!\n",
				mHackerContext, mHackerContext->GetPossiblyHookedOrigContext1(), *ppImmediateContext);
	}

	if (!(G->enable_hooks & EnableHooks::IMMEDIATE_CONTEXT))
		*ppImmediateContext = mHackerContext;
	LogDebug("  returns handle = %p\n", *ppImmediateContext);
}


// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// HackerDevice1 methods.  Requires Win7 Platform Update
//
// This object requires implementation of every single method in the object
// hierarchy ID3D11Device1->ID3D11Device->IUnknown
//
// Everything outside of the methods directly related to the ID3D11Device1 
// will call through to the HackerDevice object using the local reference
// as composition, instead of inheritance.  We cannot use inheritance, because
// the vtable needs to remain exactly as defined by COM.


// Follow the lead for GetImmediateContext and return the wrapped version.

STDMETHODIMP_(void) HackerDevice::GetImmediateContext1(
	/* [annotation] */
	_Out_  ID3D11DeviceContext1 **ppImmediateContext)
{
	LogInfo("HackerDevice::GetImmediateContext1(%s@%p) called with:%p\n",
		type_name(this), this, ppImmediateContext);

	if (ppImmediateContext == nullptr)
	{
		LogInfo("  *** no return possible, nullptr input.\n");
		return;
	}

	// We still need to call the original function to make sure the reference counts are correct:
	mOrigDevice1->GetImmediateContext1(ppImmediateContext);

	// we can arrive here with no mHackerContext created if one was not
	// requested from CreateDevice/CreateDeviceFromSwapChain. In that case
	// we need to wrap the immediate context now:
	if (mHackerContext == nullptr)
	{
		LogInfo("*** HackerContext1 missing at HackerDevice::GetImmediateContext1\n");

		analyse_iunknown(*ppImmediateContext);

		mHackerContext = HackerContextFactory(mOrigDevice1, *ppImmediateContext);
		mHackerContext->SetHackerDevice(this);
		LogInfo("  mHackerContext %p created to wrap %p\n", mHackerContext, *ppImmediateContext);
	}
	else if (mHackerContext->GetPossiblyHookedOrigContext1() != *ppImmediateContext)
	{
		LogInfo("WARNING: mHackerContext %p found to be wrapping %p instead of %p at HackerDevice::GetImmediateContext1!\n",
			mHackerContext, mHackerContext->GetPossiblyHookedOrigContext1(), *ppImmediateContext);
	}

	*ppImmediateContext = reinterpret_cast<ID3D11DeviceContext1*>(mHackerContext);
	LogInfo("  returns handle = %p\n", *ppImmediateContext);
}


// Now used for platform_update games.  Dishonored2 uses this.
// Updated to follow the lead of CreateDeferredContext.

STDMETHODIMP HackerDevice::CreateDeferredContext1(
	UINT ContextFlags,
	/* [annotation] */
	_Out_opt_  ID3D11DeviceContext1 **ppDeferredContext)
{
	LogInfo("HackerDevice::CreateDeferredContext1(%s@%p) called with flags = %#x, ptr:%p\n",
		type_name(this), this, ContextFlags, ppDeferredContext);

	HRESULT hr = mOrigDevice1->CreateDeferredContext1(ContextFlags, ppDeferredContext);
	if (FAILED(hr))
	{
		LogInfo("  failed result = %x for %p\n", hr, ppDeferredContext);
		return hr;
	}

	if (ppDeferredContext)
	{
		analyse_iunknown(*ppDeferredContext);
		HackerContext *hackerContext = HackerContextFactory(mRealOrigDevice1, *ppDeferredContext);
		hackerContext->SetHackerDevice(this);
		hackerContext->Bind3DMigotoResources();

		if (G->enable_hooks & EnableHooks::DEFERRED_CONTEXTS)
			hackerContext->HookContext();
		else
			*ppDeferredContext = hackerContext;

		LogInfo("  created HackerContext(%s@%p) wrapper of %p\n", type_name(hackerContext), hackerContext, *ppDeferredContext);
	}

	LogInfo("  returns result = %x for %p\n", hr, *ppDeferredContext);
	return hr;
}

STDMETHODIMP HackerDevice::CreateBlendState1(
	/* [annotation] */
	_In_  const D3D11_BLEND_DESC1 *pBlendStateDesc,
	/* [annotation] */
	_Out_opt_  ID3D11BlendState1 **ppBlendState)
{
	return mOrigDevice1->CreateBlendState1(pBlendStateDesc, ppBlendState);
}

STDMETHODIMP HackerDevice::CreateRasterizerState1(
	/* [annotation] */
	_In_  const D3D11_RASTERIZER_DESC1 *pRasterizerDesc,
	/* [annotation] */
	_Out_opt_  ID3D11RasterizerState1 **ppRasterizerState)
{
	return mOrigDevice1->CreateRasterizerState1(pRasterizerDesc, ppRasterizerState);
}

STDMETHODIMP HackerDevice::CreateDeviceContextState(
	UINT Flags,
	/* [annotation] */
	_In_reads_(FeatureLevels)  const D3D_FEATURE_LEVEL *pFeatureLevels,
	UINT FeatureLevels,
	UINT SDKVersion,
	REFIID EmulatedInterface,
	/* [annotation] */
	_Out_opt_  D3D_FEATURE_LEVEL *pChosenFeatureLevel,
	/* [annotation] */
	_Out_opt_  ID3DDeviceContextState **ppContextState)
{
	return mOrigDevice1->CreateDeviceContextState(Flags, pFeatureLevels, FeatureLevels, SDKVersion, EmulatedInterface, pChosenFeatureLevel, ppContextState);
}

STDMETHODIMP HackerDevice::OpenSharedResource1(
	/* [annotation] */
	_In_  HANDLE hResource,
	/* [annotation] */
	_In_  REFIID returnedInterface,
	/* [annotation] */
	_Out_  void **ppResource)
{
	return mOrigDevice1->OpenSharedResource1(hResource, returnedInterface, ppResource);
}

STDMETHODIMP HackerDevice::OpenSharedResourceByName(
	/* [annotation] */
	_In_  LPCWSTR lpName,
	/* [annotation] */
	_In_  DWORD dwDesiredAccess,
	/* [annotation] */
	_In_  REFIID returnedInterface,
	/* [annotation] */
	_Out_  void **ppResource)
{
	return mOrigDevice1->OpenSharedResourceByName(lpName, dwDesiredAccess, returnedInterface, ppResource);
}

