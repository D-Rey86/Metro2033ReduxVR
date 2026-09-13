"""Guard the OpenVR frame boundary used by Metro's DXGI Present hook."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
pose = (ROOT / "ThirdParty/3Dmigoto/DirectX11/VRPose.cpp").read_text(
    encoding="utf-8"
)
dxgi = (ROOT / "ThirdParty/3Dmigoto/DirectX11/HackerDXGI.cpp").read_text(
    encoding="utf-8"
)

submit_start = pose.index("void SubmitFrameToCompositor(")
wait_start = pose.index("void WaitForCompositorFrame()", submit_start)
submit_body = pose[submit_start:wait_start]
wait_body = pose[wait_start:]

assert "WaitGetPoses(" not in submit_body, (
    "WaitGetPoses must not block Metro before its completed frame is presented"
)
assert wait_body.count("WaitGetPoses(") == 1, (
    "OpenVR pacing must have one owned WaitGetPoses call"
)

present = dxgi.index("mOrigSwapChain1->Present(SyncInterval, Flags)")
wait = dxgi.index("VRPose::WaitForCompositorFrame();", present)
pose_update = dxgi.index("VRPose::UpdateVRPose();", wait)
assert present < wait < pose_update, (
    "WaitGetPoses must pace the next frame immediately after Present and before pose consumption"
)

assert 'CompatibilityLog("wait_get_poses=enter-after-present' in wait_body
assert 'CompatibilityLog("wait_get_poses=first-return' in wait_body

print("OpenVR frame pacing guard passed.")
