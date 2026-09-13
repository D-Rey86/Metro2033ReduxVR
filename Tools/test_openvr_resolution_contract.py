from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
POSE = (ROOT / "ThirdParty/3Dmigoto/DirectX11/VRPose.cpp").read_text(
    encoding="utf-8"
)
HEADER = (ROOT / "ThirdParty/3Dmigoto/DirectX11/VRCompatibility.h").read_text(
    encoding="utf-8"
)
CONTEXT = (ROOT / "ThirdParty/3Dmigoto/DirectX11/HackerContext.cpp").read_text(
    encoding="utf-8"
)


def require(fragment: str, message: str) -> None:
    if fragment not in POSE and fragment not in HEADER and fragment not in CONTEXT:
        raise AssertionError(message)


require(
    "SelectSubmissionTargetSize(",
    "final eye submission must use the centralized OpenVR target policy",
)
require(
    "sRecommendedEyeWidth, sRecommendedEyeHeight,",
    "the final submission target must be derived from OpenVR's recommendation",
)
require(
    "const UINT width = submissionTarget.width;",
    "the conversion target width must not be derived from Metro's source aspect",
)
require(
    "const UINT height = submissionTarget.height;",
    "the conversion target height must not be derived from Metro's source aspect",
)
require(
    "targetMatchesSource && neutralBrightness && !sourceIsLinear",
    "the fast path must be disabled when source and runtime dimensions differ",
)
require(
    "ReleaseScaledSubmissionResources();",
    "a failed conversion allocation must release only conversion resources",
)
require(
    "ShowVideoOverlay(submissionSourceLeft)",
    "the widescreen video overlay must retain its original source aspect",
)
require(
    "HEADSET OUTPUT  %u X %u",
    "the menu must distinguish runtime output from Metro's internal canvas",
)
require(
    "METRO RENDER  %u X %u",
    "the menu must identify its legacy resolution readout as internal rendering",
)

prepare_start = POSE.index("ID3D11Texture2D *PrepareSubmissionTexture")
prepare_end = POSE.index("static bool sEyeDumpRequested", prepare_start)
prepare = POSE[prepare_start:prepare_end]
if "VRMenu::CompositorResolutionScale()" in prepare:
    raise AssertionError(
        "final OpenVR dimensions must not be multiplied by the legacy compositor scale"
    )
if "ReleaseCompositorResources();" in prepare:
    raise AssertionError(
        "conversion allocation failure must not destroy the accepted source-eye path"
    )

print("OpenVR resolution contract guard passed.")
