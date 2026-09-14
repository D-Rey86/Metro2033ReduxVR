from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "ThirdParty/3Dmigoto/DirectX11/HackerDevice.cpp").read_text(
    encoding="utf-8"
)
POSE = (ROOT / "ThirdParty/3Dmigoto/DirectX11/VRPose.cpp").read_text(
    encoding="utf-8"
)
DXGI = (ROOT / "ThirdParty/3Dmigoto/DirectX11/HookedDXGI.cpp").read_text(
    encoding="utf-8"
)
WRAPPED_DXGI = (ROOT / "ThirdParty/3Dmigoto/DirectX11/HackerDXGI.cpp").read_text(
    encoding="utf-8"
)


def require(source: str, fragment: str, message: str) -> None:
    if fragment not in source:
        raise AssertionError(message)


require(
    DXGI,
    "VRMenu::SetRequestedOutputResolution(\n\t\tpDesc->BufferDesc.Width",
    "swap-chain creation must preserve Metro's requested desktop mode",
)
require(
    DXGI,
    "VRMenu::ForceVRPresentationResolution(\n\t\t&pDesc->BufferDesc.Width",
    "legacy swap-chain creation must use the fixed VR presentation surface",
)
require(
    DEVICE,
    "SelectMonitorIndependentSceneTarget(",
    "Metro's renderer-wide scene setter must use the monitor-independent policy",
)
require(
    DEVICE,
    "VRMenu::RequestedOutputResolutionWidth()",
    "scene normalization must use the original requested mode, not the forced backbuffer",
)
require(
    DEVICE,
    "sMetroSceneSourceWidth",
    "the first in-flight monitor-sized descriptor must be tracked for replacement",
)
require(
    DEVICE,
    "const bool canNormalize = target.monitorIndependent &&\n\t\t\ttrampoline_MetroSetRenderResolution;",
    "monitor normalization must fail closed when Metro's renderer setter is unavailable",
)
activation = DEVICE.index("if (trampoline_MetroSetRenderResolution &&")
exchange = DEVICE.index(
    "InterlockedCompareExchange(&sMetroSceneScaleActivated, 1, 0)", activation
)
if exchange < activation:
    raise AssertionError("activation state must only be claimed after prerequisites pass")

capture_start = POSE.index("bool CaptureHighResolutionScene")
capture_end = POSE.index("bool GetHighResolutionOverlayTarget", capture_start)
capture = POSE[capture_start:capture_end]
require(
    capture,
    "VRMenu::EffectiveSceneResolutionScale() <= 1.0001f",
    "1x must preserve the accepted final-backbuffer submission path",
)

submission_start = POSE.index("const bool useHighResolutionScene")
submission_end = POSE.index("ID3D11Texture2D *submissionSourceLeft", submission_start)
submission = POSE[submission_start:submission_end]
require(
    submission,
    "VRMenu::EffectiveSceneResolutionScale() > 1.0001f",
    "isolated scene submission must remain restricted to the proven above-1x path",
)

resize_start = WRAPPED_DXGI.index("STDMETHODIMP HackerSwapChain::ResizeBuffers")
resize_end = WRAPPED_DXGI.index("STDMETHODIMP HackerSwapChain::ResizeTarget", resize_start)
resize = WRAPPED_DXGI[resize_start:resize_end]
require(
    resize,
    "VRMenu::ForceVRPresentationResolution(&Width, &Height);",
    "later buffer rebuilds must retain the fixed VR presentation surface",
)
require(
    resize,
    "(Width != 2560 || Height != 1440)",
    "a repeated forced buffer size must not replace the last physical mode",
)

target_start = WRAPPED_DXGI.index("STDMETHODIMP HackerSwapChain::ResizeTarget")
target_end = WRAPPED_DXGI.index("STDMETHODIMP HackerSwapChain::GetContainingOutput", target_start)
target = WRAPPED_DXGI[target_start:target_end]
require(
    target,
    "VRMenu::ClampOversizedOutputResolution(&new_desc.Width, &new_desc.Height);",
    "physical display mode changes must retain the existing oversized-mode safety guard",
)
require(
    target,
    "VRMenu::SetRequestedOutputResolution(new_desc.Width, new_desc.Height);",
    "ResizeTarget must authoritatively track the user's physical mode",
)
if "ForceVRPresentationResolution" in target:
    raise AssertionError("the fixed VR backbuffer must not force the physical monitor mode")

print("Monitor-independent resolution integration guard passed.")
