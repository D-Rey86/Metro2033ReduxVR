#pragma once

#include <d3d11_1.h>
#include <INITGUID.h>
#include <vector>

#include "DrawCallInfo.h"

#include "CommandList.h"

#include "HackerDevice.h"
//#include "ResourceHash.h"
#include "Globals.h"

// {A3046B1E-336B-4D90-9FD6-234BC09B8687}
DEFINE_GUID(IID_HackerContext,
0xa3046b1e, 0x336b, 0x4d90, 0x9f, 0xd6, 0x23, 0x4b, 0xc0, 0x9b, 0x86, 0x87);


// Self forward reference for the factory interface.
// Per-frame CPU cost of building the second eye's constant buffers.
void ReportEyeCBCost();

// Drop everything we hold that belongs to a D3D device, after the game
// replaces it - which it does whenever a video setting changes.
// deviceReplaced=false: a new swap chain on the SAME device - only render-
// target state is dropped; buffers the game will never rebind are kept.
void ResetVRDeviceState(bool deviceReplaced = true);

class HackerContext;

HackerContext* HackerContextFactory(ID3D11Device1 *pDevice1, ID3D11DeviceContext1 *pContext1);

// Forward declaration to allow circular reference between HackerContext and HackerDevice. 
// We need this to allow each to reference the other as needed.

class HackerDevice;

namespace DirectX {
	inline namespace DX11 {
		class SpriteBatch;
		class SpriteFont;
	}
}

enum class FrameAnalysisOptions;
struct ShaderOverride;


struct DrawContext
{
	float oldSeparation;
	ID3D11PixelShader *oldPixelShader;
	ID3D11VertexShader *oldVertexShader;
	CommandList *post_commands[5];
	DrawCallInfo call_info;

	DrawContext(DrawCall type,
			UINT VertexCount, UINT IndexCount, UINT InstanceCount,
			UINT FirstVertex, UINT FirstIndex, UINT FirstInstance,
			ID3D11Buffer **indirect_buffer, UINT args_offset) :
		oldSeparation(FLT_MAX),
		oldVertexShader(NULL),
		oldPixelShader(NULL),
		call_info(type, VertexCount, IndexCount, InstanceCount, FirstVertex, FirstIndex, FirstInstance,
				indirect_buffer, args_offset)
	{
		memset(post_commands, 0, sizeof(post_commands));
	}
};

struct DispatchContext
{
	CommandList *post_commands;
	DrawCallInfo call_info;

	DispatchContext(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) :
		post_commands(NULL),
		call_info(DrawCall::Dispatch, 0, 0, 0, 0, 0, 0, NULL, 0, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ)
	{}

	DispatchContext(ID3D11Buffer **indirect_buffer, UINT args_offset) :
		post_commands(NULL),
		call_info(DrawCall::DispatchIndirect, 0, 0, 0, 0, 0, 0, indirect_buffer, args_offset)
	{}
};



// These are per-context so we shouldn't need locks
struct MappedResourceInfo {
	D3D11_MAPPED_SUBRESOURCE map;
	bool mapped_writable;
	void *orig_pData;
	size_t size;

	MappedResourceInfo() :
		orig_pData(NULL),
		size(0),
		mapped_writable(false)
	{}
};


// 1-6-18:  Current approach will be to only create one level of wrapping,
// specifically HackerDevice and HackerContext, based on the ID3D11Device1,
// and ID3D11DeviceContext1.  ID3D11Device1/ID3D11DeviceContext1 is supported
// on Win7+platform_update, and thus is a superset of what we need.  By
// using the highest level object supported, we can kill off a lot of conditional
// code that just complicates things. 
//
// The ID3D11DeviceContext1 will be supported on all OS except Win7 minus the 
// platform_update.  In that scenario, we will save a reference to the 
// ID3D11DeviceContext object instead, but store it and wrap it in HackerContext.
// 
// Specifically decided to not name everything *1, because frankly that is 
// was an awful choice on Microsoft's part to begin with.  Meaningless number
// completely unrelated to version/revision or functionality.  Bad.
// We will use the *1 notation for object names that are specific types,
// like the mOrigContext1 to avoid misleading types.
//
// Any HackerDevice will be the superset object ID3D11DeviceContext1 in all cases
// except for Win7 missing the evil platform_update.

// Hierarchy:
//  HackerContext <- ID3D11DeviceContext1 <- ID3D11DeviceContext <- ID3D11DeviceChild <- IUnknown

class HackerContext : public ID3D11DeviceContext1
{
private:
	ID3D11Device1 *mOrigDevice1;
	ID3D11DeviceContext1 *mOrigContext1;
	ID3D11DeviceContext1 *mRealOrigContext1;
	HackerDevice *mHackerDevice;

	// These are per-context, moved from globals.h:
	uint32_t mCurrentVertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	uint32_t mCurrentIndexBuffer; // Only valid while hunting=1
	std::vector<ID3D11Resource *> mCurrentRenderTargets;
	ID3D11Resource *mCurrentDepthTarget;
	UINT mCurrentPSUAVStartSlot;
	UINT mCurrentPSNumUAVs;

	// Used for deny_cpu_read, track_texture_updates and constant buffer matching
	typedef std::unordered_map<ID3D11Resource*, MappedResourceInfo> MappedResources;
	MappedResources mMappedResources;

	// These private methods are utility routines for HackerContext.
	void BeforeDraw(DrawContext &data);
	void AfterDraw(DrawContext &data);

	// Metro2033ReduxVR: swap a private cb_main_matrices1 in for the muzzle
	// flash's draws only, so it can be given the VR camera without touching
	// the buffer every other shader shares. See BeforeDraw.
	void BeginMuzzleFlashCB(UINT indexCount);
	void EndMuzzleFlashCB();
	ID3D11Buffer *mFlashCB;
	ID3D11Buffer *mFlashSavedCB;
	bool mFlashCBSwapped;
	bool mFlashCBIsDecal;

	// The combined first-person hands are one skinned draw, but the two
	// disconnected islands use disjoint bone rows.  Keep a private copy of
	// cb_bones so the left island can receive the left-controller delta without
	// splitting triangles (which breaks skinning at the wrist).
	bool BeginLeftHandBonePalette(bool rigidInstanceOnly = false,
		bool prologueSkeleton = false, bool lateGameSkeleton = false,
		bool lateVisibleSkeleton = false);
	bool BeginRightHandBonePalette(bool postClothingSkeleton = false);
	bool SubstituteLeftHandCompletedInstance(UINT indexCount,
		bool survivalAnalog = false, bool survivalTimerHand = false);
	bool SubstituteLeftHandLighterInstance(unsigned lighterVariant);
	bool SubstituteLeftHandChargerInstance(bool primaryBody, UINT indexCount,
		UINT startIndex, INT baseVertex);
	bool BeginLeftHandLighterParticlePivotGS(bool rightEye,
		UINT startInstance, UINT64 pixelShaderHash, bool secondaryBatch = false);
	bool BeginLeftHandLighterParticleReparentGS(bool rightEye);
	bool UpdateLeftHandLighterFlameCB(bool rightEye, bool rigidEmitter);
	void EndLeftHandBonePalette();
	ID3D11Buffer *mLeftHandBonesCB;
	ID3D11Buffer *mLeftHandBonesStaging;
	ID3D11Buffer *mLeftHandBonesSavedCB;
	bool mLeftHandBonesSwapped;
	ID3D11Buffer *mLighterFlameCB;
	ID3D11GeometryShader *mLighterParticlePivotGS;
	ID3D11Buffer *mLighterParticlePivotCB;
	ID3D11GeometryShader *mLighterParticleReparentGS;
	ID3D11Buffer *mLighterParticleReparentCB;
	ID3D11RasterizerState *mHandsNoScissorRasterState;
	ID3D11RasterizerState *mHandsSavedRasterState;
	bool mHandsRasterStateSwapped;

	// Metro's native ADS changes the skinned first-person hands palette to
	// pull the weapon toward the camera. Preserve the last hip-fire palette
	// and bind it only for the combined-hands draw while ADS is active. This
	// is visual-only: the game's ADS input and therefore its recoil/spread
	// suppression remain enabled.
	bool BeginADSFrozenHandPalette(UINT indexCount, UINT startIndex, INT baseVertex);
	void EndADSFrozenHandPalette();
	ID3D11Buffer *mADSFrozenHandsCB;
	ID3D11Buffer *mADSFrozenHandsSavedCB;
	UINT mADSFrozenHandsByteWidth;
	bool mADSFrozenHandsValid;
	bool mADSFrozenHandsSwapped;
	std::unordered_map<unsigned long long, ID3D11Buffer *> mADSRestPaletteBuffers;
	std::unordered_set<unsigned long long> mADSRestPaletteValid;
	std::unordered_map<unsigned long long, ID3D11Buffer *> mADSRestInstanceBuffers;
	std::unordered_set<unsigned long long> mADSRestInstanceValid;

	// Exact world-decal vertex shader used only after the shared
	// muzzle/decal draw has been classified as a decal. It gives hardware
	// depth a physically bounded surface-normal separation while leaving the
	// original pixel shader's projected-volume test intact.
	bool EnsureDecalOffsetVS();
	ID3D11VertexShader *mDecalOffsetVS;
	ID3D11VertexShader *mDecalSavedVS;
	bool mDecalOffsetVSInitDone;
	bool mDecalVSSwapped;

	// Metro's flare/corona pass deliberately uses Always depth because the
	// flat game performs visibility separately. That CPU visibility follows
	// the native camera, not the HMD. Restrict corrected depth testing to the
	// exact measured flare shader pair and retain a calibrated self-occlusion
	// tolerance so lamp housings do not erase their own corona.
	void BeginLensFlareDepthOcclusion(bool matchesFlarePair);
	void EndLensFlareDepthOcclusion();
	ID3D11DepthStencilState *mLensFlareDepthState;
	ID3D11DepthStencilState *mLensFlareDepthStateBase;
	ID3D11DepthStencilState *mLensFlareSavedDepthState;
	ID3D11RasterizerState *mLensFlareBiasRasterState;
	ID3D11RasterizerState *mLensFlareBiasRasterStateBase;
	ID3D11RasterizerState *mLensFlareSavedRasterState;
	UINT mLensFlareSavedStencilRef;
	bool mLensFlareDepthSwapped;

	// The muzzle flash itself: near-eye particle draws, whose cb0 gets the
	// weapon's view-space transform folded into m_WV/m_WVP.
	void BeginFlashParticleCB(UINT indexCount, UINT instanceCount, UINT firstInstance);
	void EndFlashParticleCB();
	ID3D11Buffer *mParticleCB;
	ID3D11Buffer *mParticleSavedCB;
	bool mParticleCBSwapped;

	// The laser's wall dot is a deferred projected light: cb0 bounds the
	// light volume while PS b12 performs the actual red projection. Both must
	// receive the weapon affine together or the visible dot remains head-aimed.
	void BeginLaserDotLightCB(UINT indexCount);
	void UpdateLaserDotLightEye(bool rightEye);
	void EndLaserDotLightCB();
	ID3D11Buffer *mLaserDotVSCB;
	ID3D11Buffer *mLaserDotPSCB;
	ID3D11Buffer *mLaserDotSavedVSCB;
	ID3D11Buffer *mLaserDotSavedPSCB;
	ID3D11PixelShader *mLaserDotSavedShader;
	bool mLaserDotCBSwapped;
	float mLaserDotSourceVS[52];
	float mLaserDotSourcePS[64];

	// Metro2033ReduxVR: the ammo counter's cb_misc_0 (VS slot b3) carries a
	// flatscreen-only m_screen scale that anchors it to the game's native
	// (2560x1421-ish) canvas corner, putting it outside VR's comfortably
	// visible cone. Swap in a private copy with m_screen's two scale terms
	// shrunk, for this draw only, so the same corner-anchored quad ends up
	// pulled in toward center instead. See BeforeDraw.
	void BeginAmmoCounterCB(bool isAmmoDigitDraw);
	void EndAmmoCounterCB();
	ID3D11Buffer *mAmmoCB;
	ID3D11Buffer *mAmmoSavedCB;
	bool mAmmoCBSwapped;

	// Metro2033ReduxVR: same fix, same cb_misc_0/m_screen mechanism, for
	// the wrist watch's digital time readout - a separate flat 2D HUD
	// element using the same generic text shader as the ammo digits, so it
	// needs its own CB slot rather than reusing mAmmoCB (both could be
	// mid-flight in the same frame). See BeginWatchDigitCB.
	void BeginWatchDigitCB(bool isWatchDigitDraw);
	void EndWatchDigitCB();
	ID3D11Buffer *mWatchCB;
	ID3D11Buffer *mWatchSavedCB;
	bool mWatchCBSwapped;

	// Metro2033ReduxVR: the "big icon" variant of the reticle shader (same
	// kUI2DSpriteVS/kReticlePS pair as the reticle itself, disambiguated
	// by its 1024x1024 texture instead of the reticle's 32x32 one - see
	// IsReticleDraw's comment). Used for at least the door/trader
	// interaction prompt, reported showing doubled in VR the same way the
	// reticle did. No repositioning needed, unlike the ammo counter/watch -
	// just render it in one eye, so no CB swap here, only a BeginTwinPass
	// exclusion flag.
	bool mBigIconTwinSuppress;

	// Metro2033ReduxVR: real stereo fusion for the shared 2D UI shader
	// family (see IsUniversalUIDraw/BuildViewLockedScreenMatrix) -
	// supersedes mAmmoCB/mWatchCB/mBigIconTwinSuppress above, which only
	// ever gave these elements ONE eye instead of genuinely fusing both.
	// Those are left defined but unused rather than deleted, in case this
	// needs a fast revert. One CB slot for all of it: only one such draw
	// is ever "in flight" at a time on this immediate context.
	void BeginUniversalUICB(bool matches, UINT indexCount, bool watchDigits = false,
		bool reticle = false);
	void EndUniversalUICB();
	ID3D11Buffer *mUICB;
	ID3D11Buffer *mUISavedCB;
	bool mUICBSwapped;
	void BeginVideoPanelVS();
	void EndVideoPanelVS();
	ID3D11VertexShader *mVideoPanelVS;
	ID3D11VertexShader *mVideoPanelSavedVS;
	bool mVideoPanelSwapped;

	// Metro2033ReduxVR: outside gameplay (the diegetic main menu, see
	// IsLikelyInGameplay), each UI draw already carries its OWN correct
	// per-item 3D placement in the game's own m_screen (confirmed live:
	// reading it back showed a genuine, different-per-item projection
	// matrix, not a flat HUD-pixel scale) - unlike in gameplay, where
	// BuildViewLockedScreenMatrix's canvas-pixel plane is what's needed.
	// So outside gameplay this preserves the game's own matrix for eye 0
	// (copied through unchanged) and only recomposes it with eye 1's own
	// projection for the twin draw, rather than replacing it outright.
	// mUIGameCB caches the as-read 272 bytes between BeginUniversalUICB
	// (eye 0) and BeginTwinPass (eye 1), since both need the same base.
	bool mUIPreserveMode;
	bool mUIWatchMode;
	bool mUIJournalMode;
	bool mUIReticleMode;
	float mUIGameCB[68];

	// Metro's weapon-selection previews are real 3D meshes, not members of
	// the sprite family above. Their HUD shader reads cb1.m_P together with
	// Metro's original HUD-authored m_WV, so the general scene-camera patch
	// leaves them on the desktop layout. Give only that exact menu shader a
	// private per-eye projection. A measured clip-space affine moves Metro's
	// native bottom row into the compact boxes beside the radial without
	// replacing its projection; eye 1 then starts from that same matrix and
	// receives only the proven finite-depth UI-plane disparity.
	void BeginWeaponMenuPreviewCB();
	void UpdateWeaponMenuPreviewEye(bool rightEye);
	void EndWeaponMenuPreviewCB();
	ID3D11Buffer *mWeaponMenuPreviewCB;
	ID3D11Buffer *mWeaponMenuPreviewSavedCB;
	UINT mWeaponMenuPreviewCBBytes;
	bool mWeaponMenuPreviewCBSwapped;
	std::vector<unsigned char> mWeaponMenuPreviewEye0;
	std::vector<unsigned char> mWeaponMenuPreviewEye1;

	// Metro2033ReduxVR: weapon-mounted ammo display. Injects a small
	// 7-segment LED-style quad mesh, weapon-relative via the same
	// BuildWeaponAffine transform the muzzle flash uses, showing the live
	// magazine count read from VRPose::ConfirmedAmmoAddress(). First
	// version - single eye only, see DrawAmmoDisplay's own comment for
	// why. Called from AfterDraw, not a Begin/End pair: this is a whole
	// extra draw call, not a patch to an existing one.
	void DrawAmmoDisplay();
	void DrawWatchDisplay();
	void DrawVRReticle();
	void DrawVRLaserDot();
	void DrawVRMenu();
	ID3D11VertexShader *mAmmoDisplayVS;
	ID3D11PixelShader *mAmmoDisplayPS;
	ID3D11InputLayout *mAmmoDisplayLayout;
	ID3D11Buffer *mAmmoDisplayVB;
	ID3D11Buffer *mAmmoDisplayIB;
	ID3D11Buffer *mAmmoDisplayCB;
	bool mAmmoDisplayInitDone;
	bool mAmmoDisplayInitOk;
	unsigned mAmmoDisplayLastFrame;
	unsigned mWatchDisplayLastFrame;
	ID3D11VertexShader *mVRMenuVS;
	ID3D11PixelShader *mVRMenuPS;
	ID3D11InputLayout *mVRMenuLayout;
	ID3D11Buffer *mVRMenuVB;
	ID3D11Texture2D *mVRMenuTexture;
	ID3D11RenderTargetView *mVRMenuRTV;
	ID3D11DepthStencilState *mVRMenuDepthState;
	ID3D11BlendState *mVRMenuBlendState;
	ID3D11RasterizerState *mVRMenuRasterizerState;
	DirectX::SpriteBatch *mVRMenuSpriteBatch;
	DirectX::SpriteFont *mVRMenuFont;
	bool mVRMenuInitDone;
	bool mVRMenuInitOk;
	unsigned mVRMenuLastFrame;
	// Profiler HUD (VRPerf text on its own compositor overlay).
	void DrawPerfHud();
	ID3D11Texture2D *mPerfHudTexture;
	ID3D11RenderTargetView *mPerfHudRTV;
	DirectX::SpriteBatch *mPerfHudSpriteBatch;
	DirectX::SpriteFont *mPerfHudFont;
	bool mPerfHudInitDone;
	bool mPerfHudInitOk;
	unsigned mPerfHudLastFrame;
	unsigned long long mPerfHudTextHash;
	// Late-2D UI layer for non-gameplay screens: Metro's post-scene 2D is
	// redirected into one untwinned transparent target and shown as a quad.
	bool EnsureUILayerResources(UINT width, UINT height, DXGI_FORMAT format);
	void ActivateUILayer();
	void BeginUILayerDraw(bool worldLabel);
	void EndUILayerDraw(const DrawCallInfo &call);
	ID3D11Texture2D *mUILayerTexture;
	ID3D11RenderTargetView *mUILayerRTV;
	ID3D11Texture2D *mUILayerPresentTexture;
	UINT mUILayerWidth;
	UINT mUILayerHeight;
	DXGI_FORMAT mUILayerFormat;
	bool mUILayerUsedThisFrame;
	bool mUILayerBlendSwapped;
	bool mUILayerExceptionBound;
	// Opaque-draw coverage pass: a stencil tag per opaque draw, then the same
	// draw again writing alpha 1 where the tag landed.
	ID3D11Texture2D *mUILayerStencilTexture;
	ID3D11DepthStencilView *mUILayerDSV;
	UINT mUILayerStencilRef;
	bool mUILayerCoverageTagged;
	ID3D11DepthStencilState *mUILayerSavedDSS;
	UINT mUILayerSavedStencilRef;
	// Is this the immediate context? Asked on every shadowed Map; the answer
	// cannot change for a given context. -1 until first asked.
	int mContextTypeImmediate;
	// The blend state actually bound around a layer draw (held reference).
	ID3D11BlendState *mUILayerSavedBlend;
	FLOAT mUILayerSavedBlendFactor[4];
	UINT mUILayerSavedSampleMask;
	void ReleaseUILayerResources();

	bool BeforeDispatch(DispatchContext *context);
	void AfterDispatch(DispatchContext *context);
	template <class ID3D11Shader,
		void (__stdcall ID3D11DeviceContext::*GetShaderVS2013BUGWORKAROUND)(ID3D11Shader**, ID3D11ClassInstance**, UINT*),
		void (__stdcall ID3D11DeviceContext::*SetShaderVS2013BUGWORKAROUND)(ID3D11Shader*, ID3D11ClassInstance*const*, UINT),
		HRESULT (__stdcall ID3D11Device::*CreateShader)(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11Shader**)
	>
	void DeferredShaderReplacement(ID3D11DeviceChild *shader, UINT64 hash, wchar_t *shader_type);
	void DeferredShaderReplacementBeforeDraw();
	void DeferredShaderReplacementBeforeDispatch();
	bool ExpandRegionCopy(ID3D11Resource *pDstResource, UINT DstX,
		UINT DstY, ID3D11Resource *pSrcResource, const D3D11_BOX *pSrcBox,
		UINT *replaceDstX, D3D11_BOX *replaceBox);
	bool MapDenyCPURead(ID3D11Resource *pResource, UINT Subresource,
			D3D11_MAP MapType, UINT MapFlags,
			D3D11_MAPPED_SUBRESOURCE *pMappedResource);
	void TrackAndDivertMap(HRESULT map_hr, ID3D11Resource *pResource,
		UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
		D3D11_MAPPED_SUBRESOURCE *pMappedResource);
	void TrackAndDivertUnmap(ID3D11Resource *pResource, UINT Subresource);
	void ProcessShaderOverride(ShaderOverride *shaderOverride, bool isPixelShader, DrawContext *data);
	ID3D11PixelShader* SwitchPSShader(ID3D11PixelShader *shader);
	ID3D11VertexShader* SwitchVSShader(ID3D11VertexShader *shader);
	void RecordDepthStencil(ID3D11DepthStencilView *target);
	template <void (__stdcall ID3D11DeviceContext::*GetShaderResources)(THIS_
		UINT StartSlot,
		UINT NumViews,
		ID3D11ShaderResourceView **ppShaderResourceViews)>
	void RecordShaderResourceUsage(std::map<UINT64, ShaderInfoData> &ShaderInfo, UINT64 currentShader);
	void _RecordShaderResourceUsage(ShaderInfoData *shader_info,
			ID3D11ShaderResourceView *views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]);
	void RecordGraphicsShaderStats();
	void RecordComputeShaderStats();
	void RecordPeerShaders(std::set<UINT64> *PeerShaders, UINT64 this_shader_hash);
	void RecordRenderTargetInfo(ID3D11RenderTargetView *target, UINT view_num);
	ID3D11Resource* RecordResourceViewStats(ID3D11View *view, std::set<uint32_t> *resource_info);

	// Templates to reduce duplicated code:
	template <class ID3D11Shader,
		 void (__stdcall ID3D11DeviceContext::*OrigSetShader)(THIS_
				 ID3D11Shader *pShader,
				 ID3D11ClassInstance *const *ppClassInstances,
				 UINT NumClassInstances)
		 >
	STDMETHODIMP_(void) SetShader(THIS_
		/* [annotation] */
		__in_opt ID3D11Shader *pShader,
		/* [annotation] */
		__in_ecount_opt(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances,
		std::set<UINT64> *visitedShaders,
		UINT64 selectedShader,
		UINT64 *currentShaderHash,
		ID3D11Shader **currentShaderHandle);
	template <void (__stdcall ID3D11DeviceContext::*OrigSetShaderResources)(THIS_
			UINT StartSlot,
			UINT NumViews,
			ID3D11ShaderResourceView *const *ppShaderResourceViews)>
	void BindStereoResources();
	template <void (__stdcall ID3D11DeviceContext::*OrigSetShaderResources)(THIS_
			UINT StartSlot,
			UINT NumViews,
			ID3D11ShaderResourceView *const *ppShaderResourceViews)>
	void SetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews);

protected:
	// Allow FrameAnalysisContext access to these as an interim measure
	// until it has been further decoupled from HackerContext.
	// Metro2033ReduxVR: these now always hold the bound shader's hash (0 for
	// a shader with no known hash), because this mod's draw classifiers read
	// them on every draw. Upstream left them zero without ShaderOverrides.
	UINT64 mCurrentVertexShader;
	UINT64 mCurrentHullShader;
	UINT64 mCurrentDomainShader;
	UINT64 mCurrentGeometryShader;
	UINT64 mCurrentPixelShader;
	UINT64 mCurrentComputeShader;

public:
	// Metro2033ReduxVR: patches the game's camera constant buffer
	// (cb_main_matrices1) rotation in place, in the CPU-mapped memory
	// the game just wrote via Map(WRITE_DISCARD), right before Unmap
	// flushes it to the GPU. See VRPose.h and Unmap. Public (rather than
	// private, like the rest of this file's helpers) so the file-scope
	// sTrackedVRSlots table in HackerContext.cpp can take its address.
	void PatchMappedVRCameraData(void *mappedData);

	// Metro2033ReduxVR: patches the game's per-object constant buffer
	// (cb_main_matrices0) m_WV/m_WVP in place, recomputed from this
	// buffer's own m_W plus the last patched view/projection cached by
	// PatchMappedVRCameraData - see Notes/07 in the mod project for why
	// this is needed (most scene geometry reads a precombined WVP here,
	// not cb1's m_V directly). No-op if no camera frame has been patched
	// yet (G->vrCachedViewValid false).
	void PatchMappedVRObjectData(void *mappedData);

	// Metro2033ReduxVR true stereo: run the current draw a second time into
	// the other eye's render targets. See HackerContext.cpp.
	bool BeginTwinPass(bool instanced);
	int ClassifyEyeDraw(bool instanced);
	void EndTwinPass();

	// Single-pass stereo: fill both eyes from ONE draw with twice the
	// instances, instead of drawing everything a second time. Returns true if
	// it took the draw over, in which case the caller issues the doubled-
	// instance draw and calls EndSinglePass; false means fall back to the
	// double-draw path, which stays as the always-correct route.
	bool BeginSinglePass(bool instanced);
	void EndSinglePass();

	// Writes the left eye's matrices back into the game's own constant
	// buffer, but only when they are actually still needed. See
	// HackerContext.cpp.
	void RestoreEye0IfNeeded();

	// Batched second eye: hold this pass's right-eye draws and replay them in
	// one go, so the render targets are switched once per pass instead of
	// twice per draw. See StereoBatch.h for why that matters.
	bool WantsSecondEye();
	bool HoldForSecondEye(int kind, UINT p0, UINT p1, UINT p2, UINT p3, UINT p4);
	void FlushSecondEye();
	void DrawVRMenuAtFrameEnd();
	// Present-thread hand-off of the late-2D UI layer captured this frame.
	void PresentUILayer();
	void ActivateHighResolutionOverlays();
	void EndHighResolutionOverlayFrame();

	// Clears the per-frame bookkeeping that only makes sense in one stereo
	// mode, so the first frame after a switch does not inherit stale state.
	void ResetStereoModeState();
	void PatchForBothEyes(struct TrackedVRSlot &tracked);
	void SynchronizeEarlyNativeCamera(const void *objectData, UINT bytes);

	// Skinned characters store bone matrices as view x bone, so the eye
	// offset has to be applied to the palette itself. See HackerContext.cpp.
	void PatchBonePaletteForBothEyes(ID3D11Buffer *buf, void *data, UINT bytes);

	// Metro2033ReduxVR: reads, corrects (via G->vrViewCorrection3x4), and
	// writes back the 48-byte local-to-view matrix at a specific offset
	// within the per-instance "xform" vertex buffer (64-byte stride: 48
	// bytes matrix + 16 bytes unrelated data, confirmed via RenderDoc
	// against the weapon-viewmodel draw - see Notes/09 in the mod
	// project). Unlike cb0/cb1, these objects (confirmed: the weapon)
	// never read the camera buffer at all - the engine pre-bakes their
	// view transform CPU-side, so we correct just the specific slot a
	// draw uses rather than the whole (1MB, shared-pool) buffer. Each
	// offset is only corrected once per frame - see
	// ResetVRInstanceXformFrameTracking. No-op if no camera frame has
	// been patched yet.
	// Metro2033ReduxVR TEMPORARY (Notes/10 Update 5): isWeaponLayout
	// controls whether the write is the real corrected value or an
	// inert (unchanged) one - a controlled experiment to isolate
	// whether the prop regression is caused by the weapon's Map/Unmap
	// cycle itself (timing) or the specific values it writes (data).
	void PatchInstanceXformAtOffset(ID3D11Buffer *buf, UINT offset, bool isWeaponLayout);
	bool SubstituteWeaponInstanceBuffer(UINT indexCount, bool classifying,
		bool leftHand = false, bool rightHandOnly = false);
	void RestoreWeaponInstanceBuffer();

	// Metro2033ReduxVR: clears the per-frame "already corrected" offset
	// tracking used by PatchInstanceXformAtOffset. Call once per frame -
	// see HackerSwapChain::Present.
	void ResetVRInstanceXformFrameTracking();

	HackerContext(ID3D11Device1 *pDevice1, ID3D11DeviceContext1 *pContext1);

	void SetHackerDevice(HackerDevice *pDevice);
	HackerDevice* GetHackerDevice();
	void Bind3DMigotoResources();
	void InitIniParams();
	ID3D11DeviceContext1* GetPossiblyHookedOrigContext1();
	ID3D11DeviceContext1* GetPassThroughOrigContext1();
	void HookContext();

	// public to allow CommandList access
	virtual void FrameAnalysisLog(char *fmt, ...) {};
	virtual void FrameAnalysisTrigger(FrameAnalysisOptions new_options) {};
	virtual void FrameAnalysisDump(ID3D11Resource *resource, FrameAnalysisOptions options,
		const wchar_t *target, DXGI_FORMAT format, UINT stride, UINT offset) {};

	// These are the shaders the game has set, which may be different from
	// the ones we have bound to the pipeline:
	ID3D11VertexShader *mCurrentVertexShaderHandle;
	ID3D11PixelShader *mCurrentPixelShaderHandle;
	ID3D11ComputeShader *mCurrentComputeShaderHandle;
	ID3D11GeometryShader *mCurrentGeometryShaderHandle;
	ID3D11DomainShader *mCurrentDomainShaderHandle;
	ID3D11HullShader *mCurrentHullShaderHandle;

	/*** IUnknown methods ***/

	HRESULT STDMETHODCALLTYPE QueryInterface(
		/* [in] */ REFIID riid,
		/* [iid_is][out] */ _COM_Outptr_ void __RPC_FAR *__RPC_FAR *ppvObject);

	ULONG STDMETHODCALLTYPE AddRef(void);

	ULONG STDMETHODCALLTYPE Release(void);


	/** ID3D11DeviceChild **/

	void STDMETHODCALLTYPE GetDevice(
		/* [annotation] */
		_Out_  ID3D11Device **ppDevice);

	HRESULT STDMETHODCALLTYPE GetPrivateData(
		/* [annotation] */
		_In_  REFGUID guid,
		/* [annotation] */
		_Inout_  UINT *pDataSize,
		/* [annotation] */
		_Out_writes_bytes_opt_(*pDataSize)  void *pData);

	HRESULT STDMETHODCALLTYPE SetPrivateData(
		/* [annotation] */
		_In_  REFGUID guid,
		/* [annotation] */
		_In_  UINT DataSize,
		/* [annotation] */
		_In_reads_bytes_opt_(DataSize)  const void *pData);

	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
		/* [annotation] */
		_In_  REFGUID guid,
		/* [annotation] */
		_In_opt_  const IUnknown *pData);


	/** ID3D11DeviceContext **/

	void STDMETHODCALLTYPE VSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE PSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE PSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11PixelShader *pPixelShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE PSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE VSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11VertexShader *pVertexShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE DrawIndexed(
		/* [annotation] */
		_In_  UINT IndexCount,
		/* [annotation] */
		_In_  UINT StartIndexLocation,
		/* [annotation] */
		_In_  INT BaseVertexLocation);

	void STDMETHODCALLTYPE Draw(
		/* [annotation] */
		_In_  UINT VertexCount,
		/* [annotation] */
		_In_  UINT StartVertexLocation);

	HRESULT STDMETHODCALLTYPE Map(
		/* [annotation] */
		_In_  ID3D11Resource *pResource,
		/* [annotation] */
		_In_  UINT Subresource,
		/* [annotation] */
		_In_  D3D11_MAP MapType,
		/* [annotation] */
		_In_  UINT MapFlags,
		/* [annotation] */
		_Out_  D3D11_MAPPED_SUBRESOURCE *pMappedResource);

	void STDMETHODCALLTYPE Unmap(
		/* [annotation] */
		_In_  ID3D11Resource *pResource,
		/* [annotation] */
		_In_  UINT Subresource);

	void STDMETHODCALLTYPE PSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE IASetInputLayout(
		/* [annotation] */
		_In_opt_  ID3D11InputLayout *pInputLayout);

	void STDMETHODCALLTYPE IASetVertexBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppVertexBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pStrides,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pOffsets);

	void STDMETHODCALLTYPE IASetIndexBuffer(
		/* [annotation] */
		_In_opt_  ID3D11Buffer *pIndexBuffer,
		/* [annotation] */
		_In_  DXGI_FORMAT Format,
		/* [annotation] */
		_In_  UINT Offset);

	void STDMETHODCALLTYPE DrawIndexedInstanced(
		/* [annotation] */
		_In_  UINT IndexCountPerInstance,
		/* [annotation] */
		_In_  UINT InstanceCount,
		/* [annotation] */
		_In_  UINT StartIndexLocation,
		/* [annotation] */
		_In_  INT BaseVertexLocation,
		/* [annotation] */
		_In_  UINT StartInstanceLocation);

	void STDMETHODCALLTYPE DrawInstanced(
		/* [annotation] */
		_In_  UINT VertexCountPerInstance,
		/* [annotation] */
		_In_  UINT InstanceCount,
		/* [annotation] */
		_In_  UINT StartVertexLocation,
		/* [annotation] */
		_In_  UINT StartInstanceLocation);

	void STDMETHODCALLTYPE GSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE GSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11GeometryShader *pShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE IASetPrimitiveTopology(
		/* [annotation] */
		_In_  D3D11_PRIMITIVE_TOPOLOGY Topology);

	void STDMETHODCALLTYPE VSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE VSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE Begin(
		/* [annotation] */
		_In_  ID3D11Asynchronous *pAsync);

	void STDMETHODCALLTYPE End(
		/* [annotation] */
		_In_  ID3D11Asynchronous *pAsync);

	HRESULT STDMETHODCALLTYPE GetData(
		/* [annotation] */
		_In_  ID3D11Asynchronous *pAsync,
		/* [annotation] */
		_Out_writes_bytes_opt_(DataSize)  void *pData,
		/* [annotation] */
		_In_  UINT DataSize,
		/* [annotation] */
		_In_  UINT GetDataFlags);

	void STDMETHODCALLTYPE SetPredication(
		/* [annotation] */
		_In_opt_  ID3D11Predicate *pPredicate,
		/* [annotation] */
		_In_  BOOL PredicateValue);

	void STDMETHODCALLTYPE GSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE GSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE OMSetRenderTargets(
		/* [annotation] */
		_In_range_(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11RenderTargetView *const *ppRenderTargetViews,
		/* [annotation] */
		_In_opt_  ID3D11DepthStencilView *pDepthStencilView);

	void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(
		/* [annotation] */
		_In_  UINT NumRTVs,
		/* [annotation] */
		_In_reads_opt_(NumRTVs)  ID3D11RenderTargetView *const *ppRenderTargetViews,
		/* [annotation] */
		_In_opt_  ID3D11DepthStencilView *pDepthStencilView,
		/* [annotation] */
		_In_range_(0, D3D11_1_UAV_SLOT_COUNT - 1)  UINT UAVStartSlot,
		/* [annotation] */
		_In_  UINT NumUAVs,
		/* [annotation] */
		_In_reads_opt_(NumUAVs)  ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
		/* [annotation] */
		_In_reads_opt_(NumUAVs)  const UINT *pUAVInitialCounts);

	void STDMETHODCALLTYPE OMSetBlendState(
		/* [annotation] */
		_In_opt_  ID3D11BlendState *pBlendState,
		/* [annotation] */
		_In_opt_  const FLOAT BlendFactor[4],
		/* [annotation] */
		_In_  UINT SampleMask);

	void STDMETHODCALLTYPE OMSetDepthStencilState(
		/* [annotation] */
		_In_opt_  ID3D11DepthStencilState *pDepthStencilState,
		/* [annotation] */
		_In_  UINT StencilRef);

	void STDMETHODCALLTYPE SOSetTargets(
		/* [annotation] */
		_In_range_(0, D3D11_SO_BUFFER_SLOT_COUNT)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppSOTargets,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pOffsets);

	void STDMETHODCALLTYPE DrawAuto(void);

	void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(
		/* [annotation] */
		_In_  ID3D11Buffer *pBufferForArgs,
		/* [annotation] */
		_In_  UINT AlignedByteOffsetForArgs);

	void STDMETHODCALLTYPE DrawInstancedIndirect(
		/* [annotation] */
		_In_  ID3D11Buffer *pBufferForArgs,
		/* [annotation] */
		_In_  UINT AlignedByteOffsetForArgs);

	void STDMETHODCALLTYPE Dispatch(
		/* [annotation] */
		_In_  UINT ThreadGroupCountX,
		/* [annotation] */
		_In_  UINT ThreadGroupCountY,
		/* [annotation] */
		_In_  UINT ThreadGroupCountZ);

	void STDMETHODCALLTYPE DispatchIndirect(
		/* [annotation] */
		_In_  ID3D11Buffer *pBufferForArgs,
		/* [annotation] */
		_In_  UINT AlignedByteOffsetForArgs);

	void STDMETHODCALLTYPE RSSetState(
		/* [annotation] */
		_In_opt_  ID3D11RasterizerState *pRasterizerState);

	void STDMETHODCALLTYPE RSSetViewports(
		/* [annotation] */
		_In_range_(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)  UINT NumViewports,
		/* [annotation] */
		_In_reads_opt_(NumViewports)  const D3D11_VIEWPORT *pViewports);

	void STDMETHODCALLTYPE RSSetScissorRects(
		/* [annotation] */
		_In_range_(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)  UINT NumRects,
		/* [annotation] */
		_In_reads_opt_(NumRects)  const D3D11_RECT *pRects);

	void STDMETHODCALLTYPE CopySubresourceRegion(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_  UINT DstX,
		/* [annotation] */
		_In_  UINT DstY,
		/* [annotation] */
		_In_  UINT DstZ,
		/* [annotation] */
		_In_  ID3D11Resource *pSrcResource,
		/* [annotation] */
		_In_  UINT SrcSubresource,
		/* [annotation] */
		_In_opt_  const D3D11_BOX *pSrcBox);

	void STDMETHODCALLTYPE CopyResource(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  ID3D11Resource *pSrcResource);

	void STDMETHODCALLTYPE UpdateSubresource(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_opt_  const D3D11_BOX *pDstBox,
		/* [annotation] */
		_In_  const void *pSrcData,
		/* [annotation] */
		_In_  UINT SrcRowPitch,
		/* [annotation] */
		_In_  UINT SrcDepthPitch);

	void STDMETHODCALLTYPE CopyStructureCount(
		/* [annotation] */
		_In_  ID3D11Buffer *pDstBuffer,
		/* [annotation] */
		_In_  UINT DstAlignedByteOffset,
		/* [annotation] */
		_In_  ID3D11UnorderedAccessView *pSrcView);

	void STDMETHODCALLTYPE ClearRenderTargetView(
		/* [annotation] */
		_In_  ID3D11RenderTargetView *pRenderTargetView,
		/* [annotation] */
		_In_  const FLOAT ColorRGBA[4]);

	void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(
		/* [annotation] */
		_In_  ID3D11UnorderedAccessView *pUnorderedAccessView,
		/* [annotation] */
		_In_  const UINT Values[4]);

	void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(
		/* [annotation] */
		_In_  ID3D11UnorderedAccessView *pUnorderedAccessView,
		/* [annotation] */
		_In_  const FLOAT Values[4]);

	void STDMETHODCALLTYPE ClearDepthStencilView(
		/* [annotation] */
		_In_  ID3D11DepthStencilView *pDepthStencilView,
		/* [annotation] */
		_In_  UINT ClearFlags,
		/* [annotation] */
		_In_  FLOAT Depth,
		/* [annotation] */
		_In_  UINT8 Stencil);

	void STDMETHODCALLTYPE GenerateMips(
		/* [annotation] */
		_In_  ID3D11ShaderResourceView *pShaderResourceView);

	void STDMETHODCALLTYPE SetResourceMinLOD(
		/* [annotation] */
		_In_  ID3D11Resource *pResource,
		FLOAT MinLOD);

	FLOAT STDMETHODCALLTYPE GetResourceMinLOD(
		/* [annotation] */
		_In_  ID3D11Resource *pResource);

	void STDMETHODCALLTYPE ResolveSubresource(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_  ID3D11Resource *pSrcResource,
		/* [annotation] */
		_In_  UINT SrcSubresource,
		/* [annotation] */
		_In_  DXGI_FORMAT Format);

	void STDMETHODCALLTYPE ExecuteCommandList(
		/* [annotation] */
		_In_  ID3D11CommandList *pCommandList,
		BOOL RestoreContextState);

	void STDMETHODCALLTYPE HSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE HSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11HullShader *pHullShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE HSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE HSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE DSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE DSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11DomainShader *pDomainShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE DSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE DSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE CSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE CSSetUnorderedAccessViews(
		/* [annotation] */
		_In_range_(0, D3D11_1_UAV_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_1_UAV_SLOT_COUNT - StartSlot)  UINT NumUAVs,
		/* [annotation] */
		_In_reads_opt_(NumUAVs)  ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
		/* [annotation] */
		_In_reads_opt_(NumUAVs)  const UINT *pUAVInitialCounts);

	void STDMETHODCALLTYPE CSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11ComputeShader *pComputeShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE CSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE CSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE VSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE PSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE PSGetShader(
		/* [annotation] */
		_Out_  ID3D11PixelShader **ppPixelShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE PSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE VSGetShader(
		/* [annotation] */
		_Out_  ID3D11VertexShader **ppVertexShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE PSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE IAGetInputLayout(
		/* [annotation] */
		_Out_  ID3D11InputLayout **ppInputLayout);

	void STDMETHODCALLTYPE IAGetVertexBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppVertexBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pStrides,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pOffsets);

	void STDMETHODCALLTYPE IAGetIndexBuffer(
		/* [annotation] */
		_Out_opt_  ID3D11Buffer **pIndexBuffer,
		/* [annotation] */
		_Out_opt_  DXGI_FORMAT *Format,
		/* [annotation] */
		_Out_opt_  UINT *Offset);

	void STDMETHODCALLTYPE GSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE GSGetShader(
		/* [annotation] */
		_Out_  ID3D11GeometryShader **ppGeometryShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE IAGetPrimitiveTopology(
		/* [annotation] */
		_Out_  D3D11_PRIMITIVE_TOPOLOGY *pTopology);

	void STDMETHODCALLTYPE VSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE VSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE GetPredication(
		/* [annotation] */
		_Out_opt_  ID3D11Predicate **ppPredicate,
		/* [annotation] */
		_Out_opt_  BOOL *pPredicateValue);

	void STDMETHODCALLTYPE GSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE GSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE OMGetRenderTargets(
		/* [annotation] */
		_In_range_(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11RenderTargetView **ppRenderTargetViews,
		/* [annotation] */
		_Out_opt_  ID3D11DepthStencilView **ppDepthStencilView);

	void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(
		/* [annotation] */
		_In_range_(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumRTVs,
		/* [annotation] */
		_Out_writes_opt_(NumRTVs)  ID3D11RenderTargetView **ppRenderTargetViews,
		/* [annotation] */
		_Out_opt_  ID3D11DepthStencilView **ppDepthStencilView,
		/* [annotation] */
		_In_range_(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT UAVStartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_PS_CS_UAV_REGISTER_COUNT - UAVStartSlot)  UINT NumUAVs,
		/* [annotation] */
		_Out_writes_opt_(NumUAVs)  ID3D11UnorderedAccessView **ppUnorderedAccessViews);

	void STDMETHODCALLTYPE OMGetBlendState(
		/* [annotation] */
		_Out_opt_  ID3D11BlendState **ppBlendState,
		/* [annotation] */
		_Out_opt_  FLOAT BlendFactor[4],
		/* [annotation] */
		_Out_opt_  UINT *pSampleMask);

	void STDMETHODCALLTYPE OMGetDepthStencilState(
		/* [annotation] */
		_Out_opt_  ID3D11DepthStencilState **ppDepthStencilState,
		/* [annotation] */
		_Out_opt_  UINT *pStencilRef);

	void STDMETHODCALLTYPE SOGetTargets(
		/* [annotation] */
		_In_range_(0, D3D11_SO_BUFFER_SLOT_COUNT)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppSOTargets);

	void STDMETHODCALLTYPE RSGetState(
		/* [annotation] */
		_Out_  ID3D11RasterizerState **ppRasterizerState);

	void STDMETHODCALLTYPE RSGetViewports(
		/* [annotation] */
		_Inout_ /*_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE )*/   UINT *pNumViewports,
		/* [annotation] */
		_Out_writes_opt_(*pNumViewports)  D3D11_VIEWPORT *pViewports);

	void STDMETHODCALLTYPE RSGetScissorRects(
		/* [annotation] */
		_Inout_ /*_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE )*/   UINT *pNumRects,
		/* [annotation] */
		_Out_writes_opt_(*pNumRects)  D3D11_RECT *pRects);

	void STDMETHODCALLTYPE HSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE HSGetShader(
		/* [annotation] */
		_Out_  ID3D11HullShader **ppHullShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE HSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE HSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE DSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE DSGetShader(
		/* [annotation] */
		_Out_  ID3D11DomainShader **ppDomainShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE DSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE DSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE CSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE CSGetUnorderedAccessViews(
		/* [annotation] */
		_In_range_(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_PS_CS_UAV_REGISTER_COUNT - StartSlot)  UINT NumUAVs,
		/* [annotation] */
		_Out_writes_opt_(NumUAVs)  ID3D11UnorderedAccessView **ppUnorderedAccessViews);

	void STDMETHODCALLTYPE CSGetShader(
		/* [annotation] */
		_Out_  ID3D11ComputeShader **ppComputeShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE CSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE CSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE ClearState(void);

	void STDMETHODCALLTYPE Flush(void);

	D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType(void);

	UINT STDMETHODCALLTYPE GetContextFlags(void);

	HRESULT STDMETHODCALLTYPE FinishCommandList(
		BOOL RestoreDeferredContextState,
		/* [annotation] */
		_Out_opt_  ID3D11CommandList **ppCommandList);


	/** ID3D11DeviceContext1 **/

	void STDMETHODCALLTYPE CopySubresourceRegion1(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_  UINT DstX,
		/* [annotation] */
		_In_  UINT DstY,
		/* [annotation] */
		_In_  UINT DstZ,
		/* [annotation] */
		_In_  ID3D11Resource *pSrcResource,
		/* [annotation] */
		_In_  UINT SrcSubresource,
		/* [annotation] */
		_In_opt_  const D3D11_BOX *pSrcBox,
		/* [annotation] */
		_In_  UINT CopyFlags);

	void STDMETHODCALLTYPE UpdateSubresource1(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_opt_  const D3D11_BOX *pDstBox,
		/* [annotation] */
		_In_  const void *pSrcData,
		/* [annotation] */
		_In_  UINT SrcRowPitch,
		/* [annotation] */
		_In_  UINT SrcDepthPitch,
		/* [annotation] */
		_In_  UINT CopyFlags);

	void STDMETHODCALLTYPE DiscardResource(
		/* [annotation] */
		_In_  ID3D11Resource *pResource);

	void STDMETHODCALLTYPE DiscardView(
		/* [annotation] */
		_In_  ID3D11View *pResourceView);

	void STDMETHODCALLTYPE VSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE HSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE DSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE GSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE PSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE CSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE VSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE HSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE DSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE GSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE PSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE CSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE SwapDeviceContextState(
		/* [annotation] */
		_In_  ID3DDeviceContextState *pState,
		/* [annotation] */
		_Out_opt_  ID3DDeviceContextState **ppPreviousState);

	void STDMETHODCALLTYPE ClearView(
		/* [annotation] */
		_In_  ID3D11View *pView,
		/* [annotation] */
		_In_  const FLOAT Color[4],
		/* [annotation] */
		_In_reads_opt_(NumRects)  const D3D11_RECT *pRect,
		UINT NumRects);

	void STDMETHODCALLTYPE DiscardView1(
		/* [annotation] */
		_In_  ID3D11View *pResourceView,
		/* [annotation] */
		_In_reads_opt_(NumRects)  const D3D11_RECT *pRects,
		UINT NumRects);
};
