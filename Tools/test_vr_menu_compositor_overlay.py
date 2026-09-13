from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
POSE = (ROOT / "ThirdParty/3Dmigoto/DirectX11/VRPose.cpp").read_text(encoding="utf-8")
CONTEXT = (ROOT / "ThirdParty/3Dmigoto/DirectX11/HackerContext.cpp").read_text(encoding="utf-8")


def require(source: str, fragment: str, message: str) -> None:
    if fragment not in source:
        raise AssertionError(message)


menu_start = CONTEXT.index("void HackerContext::DrawVRMenu()")
menu_end = CONTEXT.index("void HackerContext::DrawWatchDisplay()", menu_start)
menu = CONTEXT[menu_start:menu_end]

require(menu, "menuDesc.Width = 2048; menuDesc.Height = 2048;", "menu needs a dedicated square render target")
require(menu, "IDR_ARIAL", "menu must use the embedded scalable font resource")
require(menu, "DirectX::SpriteFont", "menu must use SpriteFont rather than bitmap-pixel geometry")
require(menu, "VRPose::PresentMenuOverlay(mVRMenuTexture)", "menu texture must be submitted through the compositor overlay")
require(menu, "OMSetRenderTargets(1,&oldRTV,oldDSV)", "menu rendering must restore Metro's render targets")
if "static const unsigned char font" in menu or "BeginTwinPass(false)" in menu:
    raise AssertionError("retired 5x7/two-eye scene menu path is still active")

require(POSE, '"metro2033reduxvr.settings_menu"', "menu needs a stable OpenVR overlay key")
require(POSE, "SetOverlayTransformTrackedDeviceRelative(sMenuOverlay", "menu overlay must be head-relative")
require(POSE, "SetOverlayTexture(sMenuOverlay", "menu texture must be submitted directly to OpenVR")

capture_start = POSE.index("bool CaptureHighResolutionScene")
capture_end = POSE.index("bool GetHighResolutionOverlayTarget", capture_start)
if "VRMenu::IsOpen()" in POSE[capture_start:capture_end]:
    raise AssertionError("opening the settings menu must not activate the high-resolution stereo scene path")

print("VR menu compositor overlay guard passed.")
