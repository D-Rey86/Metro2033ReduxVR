#pragma once

// Metro2033ReduxVR: a duplicate set of render targets, one per eye.
//
// True stereo means rendering the whole frame twice per game frame. The
// matrices for that are already solved - every eye-dependent value in this
// engine goes through cb_main_matrices0/1, both of which we rewrite. What is
// missing is somewhere to PUT the second eye: if both eyes draw into the same
// targets, the second overwrites the first and post-processing then reads
// whichever landed last.
//
// So every eye-dependent render target gets a twin, created alongside it with
// an identical description, and every view over it gets a matching twin view.
// The second eye then renders into the twin set and its bloom, lighting and
// tonemapping read its own image rather than the other eye's.
//
// Which targets are eye-dependent falls out of one measurement (see
// Notes/16): shadow maps are rendered in light space and are identical for
// both eyes - and in this engine they are 67% of all draw calls. Sharing them
// is the whole reason true stereo costs about a third more here rather than
// twice as much. The census showed the shadow atlas is the only square
// render target in the frame, so "width != height" separates it cleanly from
// everything else, and the twin map then doubles as the pass classifier: a
// draw whose targets have no twin is by definition eye-independent.

#include <d3d11_1.h>

namespace StereoTwin {

	// True once at least one twin exists. Every lookup below is guarded on
	// this at the call site so the bind path pays one bool test until the
	// first twin is created.
	extern bool gHaveTwins;

	// TEMPORARY, for bringing the twin set up: send ALL eye-dependent
	// rendering into the twins instead of the originals, and nothing into
	// the originals. Same cost as today and the picture should be
	// completely unchanged - which is exactly what makes it a good test.
	// If it looks normal, then twin creation, view twinning, render-target
	// substitution and shader-resource substitution are all proven at once,
	// with no per-eye matrix work in the way to confuse a failure.
	extern bool gRedirectToTwins;

	// The real thing: draw every eye-dependent draw call twice, once into
	// each set. Mutually exclusive with gRedirectToTwins.
	extern bool gDoubleDraw;

	// Stage one of single-pass stereo: instead of a separate twin TEXTURE,
	// each eye-dependent target is created with two array slices and the twin
	// becomes slice 1 of the same resource. Nothing about rendering changes
	// yet - both eyes still draw in separate passes, into separate slices
	// instead of separate textures, and the picture should be identical.
	//
	// It is worth doing as its own step because it is what makes single-pass
	// possible at all: one draw can fill two slices of one resource, but it
	// can never fill two different resources. Measured beforehand rather than
	// assumed (see Notes/17): a plain Texture2D SRV over a 2-slice resource is
	// legal and reads slice 0, and a slice-1 view feeds a shader that declares
	// Texture2D, so the game's own shaders keep working untouched and no
	// per-frame copies are needed.
	extern bool gSliceTwins;

	// Called BEFORE the texture is created, to widen an eye-dependent target
	// to two array slices. Returns true if it changed the description, which
	// the caller must pass back to OnCreateTexture2D.
	bool WidenDescForStereo(D3D11_TEXTURE2D_DESC *desc);

	// True if this resource carries both eyes in its array slices.
	bool IsSliced(ID3D11Resource *resource);

	// Called with whatever device we are about to record something for. If it
	// is not the device everything else was recorded for, the whole cache is
	// dropped first.
	//
	// Changing a video setting makes Metro tear down its D3D device and build
	// a new one. Every map in here is then full of pointers into the old one,
	// and the allocator hands the new device the SAME addresses - so a lookup
	// returns a view belonging to a destroyed device and using it crashes. It
	// is not specific to any one setting; the quality change earlier survived
	// by luck. For a public release this has to be safe, because players will
	// change settings.
	void NoteDevice(ID3D11Device *device);

	// Called when a new swap chain is created, which is what Metro actually
	// does when a video setting changes - it keeps the DEVICE and rebuilds the
	// swap chain and every render target behind it.
	//
	// That is enough to be fatal on its own. The old textures and views are
	// destroyed, the allocator hands the new ones the SAME addresses, and our
	// maps are still keyed by those addresses - so a lookup for a brand-new
	// view returns the twin of a texture that no longer exists. Changing
	// tessellation crashed here every time; changing quality got away with it.
	void ResetForNewSwapChain();

	// A NULL view description over a 2-slice resource covers BOTH slices, so
	// the game's own views have to be given an explicit slice-0 description or
	// its post-processing would write the left eye into both. These return the
	// caller's description untouched unless the resource is sliced.
	const D3D11_RENDER_TARGET_VIEW_DESC *NarrowRTVDesc(ID3D11Resource *resource,
		const D3D11_RENDER_TARGET_VIEW_DESC *desc, D3D11_RENDER_TARGET_VIEW_DESC *storage);
	const D3D11_DEPTH_STENCIL_VIEW_DESC *NarrowDSVDesc(ID3D11Resource *resource,
		const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, D3D11_DEPTH_STENCIL_VIEW_DESC *storage);
	const D3D11_SHADER_RESOURCE_VIEW_DESC *NarrowSRVDesc(ID3D11Resource *resource,
		const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, D3D11_SHADER_RESOURCE_VIEW_DESC *storage);

	// AUTOMATIC BISECTION of where the second eye's 31 ms actually goes.
	//
	// Measured with GPU timestamps, standing still: alternate-eye costs 15.3 ms
	// a frame and true stereo 46.6 ms. The second eye therefore costs twice
	// what the whole first frame does - shadows, post-processing and all -
	// which no amount of "it draws everything twice" explains. Folding 467
	// draws a frame into single-pass moved that number by nothing at all, so
	// the cost is not in the geometry draws.
	//
	// Every previous attempt to find it compared framerates, which at 72 Hz can
	// only report about 72, 36 or 24 - three values, and every honest
	// improvement smaller than a whole step looked like "no change". This
	// cycles the configurations itself and logs real milliseconds for each, so
	// one run standing still produces the entire breakdown instead of one bit
	// of information per run.
	//
	// The picture is wrong in most phases. That is the point: each phase leaves
	// the second eye out of one category of draw, and what the frame time does
	// says what that category was costing.
	enum SkipCategory {
		kSkipNothing = 0,       // baseline
		kSkipPost = 1,          // second-eye draws that sample an eye-dependent texture
		kSkipInstanced = 2,
		kSkipTessellated = 3,
		kSkipPlainGeometry = 4,
		kSkipEverything = 5,    // upper bound: the whole second eye
		// Not draws at all. These are the per-frame costs that survive
		// removing every second-eye draw, which is where the missing 24 ms
		// has to be: 39.4 ms with no second-eye drawing against 15.4 ms for
		// alternate-eye, and only 0.34 ms of CPU to account for it.
		kSkipTwinClears = 6,    // the doubled full-resolution clears
		kSkipSecondSubmit = 7,  // handing the compositor a second eye texture
		// The per-draw BOOKKEEPING, not the draws: a twin lookup for every
		// bound render target and shader resource on every draw. In
		// alternate-eye mode none of it runs - the path exits on its first
		// line - so it is a genuine difference between the modes that survives
		// removing every second-eye draw, and Present-to-Present timestamps
		// include the GPU idling while the CPU works.
		kSkipTwinLookups = 8,
		kSkipCategoryCount = 9
	};
	extern int gBisectSkip;
	extern bool gBisectRunning;
	const char *SkipCategoryName(int category);

	// Ask to switch between true stereo and alternate-eye. The change is
	// applied at the next frame boundary, never mid-frame: flipping partway
	// through would leave one frame with some passes doubled and some not.
	void RequestStereoModeToggle();

	// Applied from Present. Returns true if the mode changed this frame.
	bool ApplyPendingStereoModeToggle();

	// The swap chain's back buffer is made by DXGI, not by CreateTexture2D,
	// so it needs registering by hand - and it must be twinned like
	// everything else or the second eye's final image has nowhere to land.
	void RegisterBackBuffer(ID3D11Device *device, ID3D11Texture2D *backBuffer);

	// `widened` is what WidenDescForStereo returned for this texture: the twin
	// is slice 1 of `created` itself rather than a separate texture.
	void OnCreateTexture2D(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *desc, ID3D11Texture2D *created, bool widened);
	void OnCreateRTV(ID3D11Device *device, ID3D11Resource *resource, const D3D11_RENDER_TARGET_VIEW_DESC *desc, ID3D11RenderTargetView *created);
	void OnCreateDSV(ID3D11Device *device, ID3D11Resource *resource, const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, ID3D11DepthStencilView *created);
	void OnCreateSRV(ID3D11Device *device, ID3D11Resource *resource, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, ID3D11ShaderResourceView *created);

	// Views covering BOTH array slices at once, made on demand from the game's
	// own single-slice view and cached. This is what a single-pass draw binds:
	// one target, two eyes, filled by one pass. NULL if the view is over a
	// resource that does not carry both eyes.
	ID3D11RenderTargetView *BothSlicesRTV(ID3D11Device *device, ID3D11RenderTargetView *gameView);
	ID3D11DepthStencilView *BothSlicesDSV(ID3D11Device *device, ID3D11DepthStencilView *gameView);

	// Return the twin of a view, or NULL if this view has none - which is
	// the signal that whatever is being drawn is eye-independent and should
	// be left alone.
	ID3D11RenderTargetView   *TwinRTV(ID3D11RenderTargetView *v);
	ID3D11DepthStencilView   *TwinDSV(ID3D11DepthStencilView *v);
	ID3D11ShaderResourceView *TwinSRV(ID3D11ShaderResourceView *v);
	ID3D11Resource           *TwinResource(ID3D11Resource *r);

	// One-time log of everything twinned and everything deliberately
	// shared, with the memory cost. Fires a few seconds into gameplay.
	// Hands back the Nth twinned texture of a given size, original and twin.
	// Used to photograph a scene target and its twin side by side, which
	// isolates "did the second eye render" from "did it reach the screen".
	bool GetTwinPair(unsigned width, unsigned height, int format, int index,
		ID3D11Texture2D **outOriginal, ID3D11Texture2D **outTwin);

	// Reports, for EVERY twinned target, how much its twin differs from the
	// original. The eye divergence has to appear somewhere in this list; the
	// first target where it does not is where the second eye stops being a
	// second eye.
	void CompareAllTwins(ID3D11Device *device, ID3D11DeviceContext *context);

	void MaybeDumpInventory();

	// How the frame's draws split between the two categories. The doubled
	// count is the real cost of true stereo; the shared count is what the
	// square-target rule is saving. Logged periodically so a framerate
	// complaint can be attributed instead of guessed at.
	extern unsigned gDoubledDraws;
	extern unsigned gSharedDraws;
	extern unsigned gEyeCBSwaps;
	void ReportFrameStats();

	void ReleaseAll();
}
