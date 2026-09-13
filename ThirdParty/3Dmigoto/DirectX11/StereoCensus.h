#pragma once

// Metro2033ReduxVR: a one-shot census of the frame's render graph, taken as
// the first step toward true (both-eyes-per-frame) stereo.
//
// Alternate-eye rendering gives each eye a frame from a different instant, so
// anything that moves is seen twice - NPCs double, everything flickers. Real
// stereo means rendering the whole frame twice per game frame, once per eye,
// into a duplicated set of render targets so each eye's post-processing reads
// its own image rather than the other eye's.
//
// The matrices are already solved: every eye-dependent value in this engine
// funnels through cb_main_matrices0 and cb_main_matrices1, both of which we
// rewrite wholesale. What is NOT known is the shape of the render graph - how
// many render targets exist, how big they are, how many passes run, how many
// draws and dispatches would have to be doubled, and which passes are
// eye-independent (shadow maps are in light space and can be shared, which is
// where the performance comes from).
//
// Designing that blind is exactly how this project has wasted test runs before.
// So: measure first. This captures a few complete frames and dumps the graph.
// It arms itself once the player is demonstrably in gameplay with a weapon
// drawn, runs for a handful of frames, and then stops for good - there is no
// cost to leaving it in a shipped build.

struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11Resource;

namespace StereoCensus {

	// True while a capture is in progress. Every Note* call below is guarded
	// on this at the call site so the draw path pays one predictable-branch
	// bool test when idle.
	extern bool gActive;

	// Set the moment Arm() is first called, so the arming test in the draw
	// path costs one bool read forever after.
	extern bool gArmedOnce;

	// Arms the capture. Called when a viewmodel pass is recognised, which is
	// a reliable "in the world, weapon out" signal - a menu or loading screen
	// would give a census of the wrong thing entirely.
	void Arm();

	// Frame boundary, from Present.
	void EndFrame();

	void NoteRenderTargets(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv);
	void NoteViewport(float width, float height, float minZ, float maxZ);
	void NoteDraw(unsigned indexCount, unsigned instanceCount);
	void NoteDispatch();
	void NoteClear(bool depth);
	void NoteCopy();
	void NoteMapDiscard();
}
