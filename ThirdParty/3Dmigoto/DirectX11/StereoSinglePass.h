#pragma once

// Metro2033ReduxVR: single-pass stereo - transform the geometry ONCE and fill
// both eyes, instead of drawing everything twice.
//
// Measured motivation: the second geometry pass costs 33.7 ms while the game's
// entire frame costs 13.9 ms. Turning it off took the mess hall from 21 fps to
// 72. Pixels, buffer maps and render-target switches were each ruled out by
// bisection - it is the vertex work, doubled.
//
// How it works: each draw is issued once with twice the instances. The vertex
// shader reads SV_InstanceID to know which eye it is, writes
// SV_RenderTargetArrayIndex to pick that eye's slice, and moves the right
// eye's clip position with a single matrix.
//
// That last part is what makes patching 191 shaders tractable. Both eyes are
// projections of the SAME world point, so
//
//     clip_right = M * clip_left,   M = (P1*V1) * (P0*V0)^-1
//
// is one 4x4, constant for the whole frame. Every shader therefore gets the
// same two-line append no matter how it computed its position - no need to
// understand each one.
//
// The patch happens at shader CREATION, not from a folder of pre-patched
// files. A file-based set was tried and is disqualifying for a release: those
// files only cover shaders the game happened to load while we were dumping, so
// the mod would be correct in the rooms we tested and quietly wrong elsewhere.

#include <d3d11_1.h>

namespace StereoSinglePass {

	// Master switch. Single-pass is the permanent default; proven-unsafe
	// shaders still use the exact per-shader double-draw fallbacks below.
	extern bool gEnabled;

	bool ForceDoubleDrawForShader(UINT64 hash);

	// Shadow-caster isolation diagnostic. F6 enables/disables preserving the
	// game's object matrices during depth-only passes (including non-square
	// shadow atlases that the older square-viewport guard misses).
	ID3D11PixelShader *ShadowDiagnosticVariantOf(ID3D11PixelShader *original);
	bool ShadowMatrixAutoEnabled();
	bool ShadowCasterIsolationEnabled();
	bool LegacyDeferredSpaceEnabled();

	// DIAGNOSTIC. Fold the draw, then let the double-draw path redraw both
	// eyes conventionally over the top of it.
	//
	// The left eye is correct and 395 draws a frame fold, so the mechanism
	// works, but the right eye loses large surfaces and blows out to white.
	// There are several candidates - reprojected depth, view-dependent
	// interpolants still holding left-eye values, a temporal-antialiasing
	// history diverging per eye - and testing them one at a time costs a run
	// each. This separates all of them at once:
	//
	//   right eye correct -> what the fold WRITES is wrong, and the fault is
	//     in the reprojection or the shader patch;
	//   right eye still white -> the fold is disturbing something a redraw
	//     cannot repair - pipeline state, or a resource read later in the
	//     frame - and the shader output is innocent.
	//
	// Slower than either path alone, so it is a measurement, not a mode.
	extern bool gVerifyByRedraw;

	// Builds a stereo variant of a vertex shader as the game creates it:
	// decompile to HLSL, append the eye selection, recompile. Safe to call
	// for every shader - anything that cannot be patched cleanly is simply
	// skipped and the original is used unchanged.
	void OnCreateVertexShader(ID3D11Device *device, const void *bytecode,
		SIZE_T length, ID3D11VertexShader *created);

	// The stereo variant of a shader, or NULL if it has none.
	ID3D11VertexShader *VariantOf(ID3D11VertexShader *original);

	// Which texture slots a pixel shader actually SAMPLES, as a t0..t31
	// bitmask taken by reflection when it is created.
	//
	// Needed because D3D shader-resource bindings are sticky: post-processing
	// leaves the scene texture bound in some slot and every later draw
	// inherits it, whether or not its shader looks at it. Asking "is anything
	// eye-dependent bound?" therefore answers yes for nearly every draw in the
	// frame - it declined 288 of 291 - and single-pass never got to run. Ask
	// the shader instead of the pipeline.
	void OnCreatePixelShader(ID3D11Device *device, const void *bytecode,
		SIZE_T length, ID3D11PixelShader *created);
	unsigned SampledSlotsOf(ID3D11PixelShader *ps);
	bool IsDeferredLightShader(ID3D11PixelShader *ps);

	// First of the four IniParams entries holding the reprojection matrix, one
	// per row. Kept clear of entry 0, which games' own ini files use.
	static const int kReprojectionParam = 8;

	// Per-frame: the matrix that carries a left-eye clip position to the
	// right eye. Handed to the shaders through 3Dmigoto's IniParams, because
	// this engine leaves no free vertex-shader constant buffer slot - all
	// fourteen are occupied.
	void SetReprojection(const float m[16]);

	// Pushes the reprojection to the GPU, at most once per change. Called from
	// the first single-pass draw that needs it rather than from the frame
	// boundary, so a frame that folds no draws pays nothing.
	void UploadIfDirty(ID3D11DeviceContext1 *context, ID3D11Resource *iniTexture);

	// How the frame's eye-dependent draws actually split. gDraws is the win;
	// the two declined counts say what is still being drawn twice and why, so
	// a disappointing framerate can be attributed instead of guessed at.
	extern unsigned gDraws;
	extern unsigned gDeclinedNoVariant;
	extern unsigned gDeclinedReadsEye;
	extern unsigned gDeclinedTessellated;
	// The rest of the reasons a draw is not folded, so "folded 0" can be
	// attributed to an exact gate instead of guessed at.
	extern unsigned gDeclinedDisabled;
	extern unsigned gDeclinedPrivateCB;
	extern unsigned gDeclinedInstanced;
	extern unsigned gDeclinedShaderList;
	extern unsigned gDeclinedNoObjectCB;
	extern unsigned gDeclinedEyeIndependent;
	extern unsigned gDeclinedDepthTested;
	extern unsigned gDeclinedSceneBuffer;
	extern unsigned gDeclinedNoBothSliceView;
	// Draws that fail ONLY the instanced gate, or ONLY the depth-tested one
	// (with or without the scene-buffer gate that hangs off it). The ordinary
	// counters above are first-match, so they are upper bounds: a draw counted
	// as "instanced" may also fail three later gates. These two say how many
	// draws would really fold if that one gate were opened, which is the number
	// the work is worth doing for - or not.
	extern unsigned gFoldableIfInstanced;
	extern unsigned gFoldableIfDepth;
	// SV_RenderTargetArrayIndex from the vertex shader. Without it a folded
	// draw puts both eyes in slice 0 instead of one per eye.
	extern bool gVPRTSupported;
	// Fold depth-tested scene geometry too. Off unless a file named
	// vr_fold_depth.txt sits beside the game executable, so it can be tried
	// and reverted without a rebuild and cannot ship on by accident.
	bool FoldDepthTestedEnabled();
	// Fold the scene-resolution buffers too - the G-buffer and lighting chain,
	// i.e. the geometry that actually costs the frame. Off unless
	// vr_fold_scene.txt sits beside the game executable; that file also opens
	// the depth gate, since this geometry is depth-tested by definition.
	bool FoldSceneBufferEnabled();
	// A marker file beside d3d11.dll (not the process working directory).
	bool FlagFilePresent(const wchar_t *name);

	// Fold already-instanced draws too. The input assembler fetches
	// per-instance data by the real instance index, so a doubled instance count
	// needs a companion input layout whose per-instance elements step every 2
	// instances. Off unless vr_fold_instanced.txt sits beside the DLL.
	bool FoldInstancedEnabled();
	void OnCreateInputLayout(ID3D11Device *device, const D3D11_INPUT_ELEMENT_DESC *elements,
		UINT count, const void *signature, SIZE_T signatureLength, ID3D11InputLayout *created);
	// For a draw using `layout`: false when it cannot be folded; otherwise
	// *bind is the layout to use during the folded draw (NULL: keep `layout`,
	// it has no per-instance elements).
	bool InstanceFoldLayout(ID3D11InputLayout *layout, ID3D11InputLayout **bind);
	// Per-shader opt-out, one hex vertex-shader hash per line of
	// vr_fold_exclude.txt, so a surface that breaks can be put back on the
	// proven double-draw path without a rebuild.
	bool ShaderFoldExcluded(UINT64 vsHash);
	// Reverts the Unmap-body cuts (palette phase retest throttle, lazy palette
	// snapshots) when vr_unmap_legacy.txt sits beside the DLL.
	bool UnmapLegacyEnabled();
	// One line naming the active policy, for the profiler summary.
	const char *ReportFoldPolicy();
	// Accumulates the counters and, every 600 frames, returns one line for the
	// profiler summary (NULL in between). LogInfo is not used: d3dx.ini logging
	// is off in the builds this matters for.
	const char *ReportFrameStats();

	// Real GPU milliseconds per frame, from timestamp queries at Present.
	//
	// Every performance number in this project so far came from the framerate
	// Virtual Desktop reports, and at 72 Hz that is quantised: the compositor
	// locks to a half or a third of refresh when a frame misses its budget, so
	// the only values observable are about 72, 36 and 24. "13.9 ms for the
	// whole frame" was the 72 Hz frame INTERVAL, not a cost, and "33.7 ms for
	// the second eye" was the difference between two such quantised readings.
	// Optimising against a number with three possible values is why several
	// correct changes in a row appeared to do nothing.
	//
	// This measures the GPU directly, so a change of one millisecond is
	// visible whether or not it happens to cross a threshold.
	void FrameTiming(ID3D11Device *device, ID3D11DeviceContext1 *context);

	// Time the game spends inside CreateVertexShader/CreatePixelShader (our
	// hook included), and the stereo-variant builds. The report returns one
	// line for a frame in which anything was created, NULL otherwise.
	void NoteShaderCreate(bool pixel, LONGLONG ticks);
	const char *ReportShaderCreation();

	void ReportPatchStats();
	void ReleaseAll();
}
