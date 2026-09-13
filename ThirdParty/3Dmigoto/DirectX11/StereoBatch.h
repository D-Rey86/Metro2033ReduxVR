#pragma once

// Metro2033ReduxVR: hold the second eye's draws back and replay them in one
// batch per render pass, instead of interleaving them draw by draw.
//
// Measured, not assumed: the doubled draws arrive in long consecutive runs -
// mean 32 per run, longest 529 - and today each one costs two render-target
// switches. Batching a run costs two for the whole run, which the numbers put
// at a 97% reduction (1,199,736 switches down to 37,118 over 600 frames).
// Removing the switches entirely took the town from 22 to the low 30s, so
// nearly all of that is available without breaking the picture.
//
// The awkward part is that a replayed draw has to be given back everything it
// depended on. Most of that is just pointers, but the per-object matrices are
// not: the engine recycles that constant buffer with Map(WRITE_DISCARD)
// between draws, so by replay time the bytes are long gone. Each held draw
// therefore carries its own copy of them.

#include <d3d11_1.h>

namespace StereoBatch {

	// Off until it is proven; the per-draw path stays available as a fallback
	// the whole time, since it is currently correct and shippable.
	extern bool gEnabled;

	// One deferred draw and the state needed to reissue it.
	struct Held {
		int kind;                 // 0 DrawIndexed, 1 Draw, 2 DrawIndexedInstanced, 3 DrawInstanced
		UINT p0, p1, p2, p3, p4;

		ID3D11InputLayout *layout;
		ID3D11VertexShader *vs;
		ID3D11PixelShader *ps;
		D3D11_PRIMITIVE_TOPOLOGY topology;

		ID3D11Buffer *vb[2];
		UINT vbStride[2];
		UINT vbOffset[2];

		ID3D11Buffer *ib;
		DXGI_FORMAT ibFormat;
		UINT ibOffset;

		ID3D11ShaderResourceView *psSrv[16];
		UINT psSrvCount;

		// Everything else the draw's appearance depends on. Omitting these
		// made the second eye render the right geometry with the LAST draw's
		// materials, blending and depth mode - washed out, wrongly lit, and
		// with shadows punched through as hard black shapes.
		ID3D11SamplerState *psSampler[16];
		UINT psSamplerCount;
		ID3D11Buffer *vsCB[14];
		ID3D11Buffer *psCB[14];
		ID3D11BlendState *blend;
		float blendFactor[4];
		UINT sampleMask;
		ID3D11DepthStencilState *depth;
		UINT stencilRef;
		ID3D11RasterizerState *raster;

		// Viewport and scissor. A replayed draw given the wrong one lands in
		// the wrong RECTANGLE of the target - which is exactly the shape of
		// the seam left in the second eye. The viewmodel pass in particular
		// uses its own compressed depth range.
		D3D11_VIEWPORT viewport;
		UINT viewportCount;
		D3D11_RECT scissor;
		UINT scissorCount;

		// Vertex-shader resources. Skinned meshes and anything doing
		// vertex-texture work read these, and leaving them out means a
		// replayed draw samples whatever the previous one left bound.
		ID3D11ShaderResourceView *vsSrv[8];
		UINT vsSrvCount;

		// The remaining programmable stages. Metro Redux ships tessellation,
		// so a hull or domain shader left bound from the last draw of the run
		// would be applied to every replayed draw - which looks like surface
		// noise rather than anything obviously geometric.
		ID3D11GeometryShader *gs;
		ID3D11HullShader *hs;
		ID3D11DomainShader *ds;

		// Offsets into the byte pool holding this draw's right-eye matrices.
		// -1 when that slot was not in play for this draw.
		int cb0Offset;
		int cb1Offset;
	};

	void Reset();
	bool Empty();
	int Count();

	// Records a draw for later replay. Returns false if the batch is full, in
	// which case the caller should fall back to drawing it inline.
	bool Hold(const Held &h, const unsigned char *cb0, UINT cb0Size,
		const unsigned char *cb1, UINT cb1Size);

	const Held *At(int i);
	const unsigned char *Bytes(int offset);
}
