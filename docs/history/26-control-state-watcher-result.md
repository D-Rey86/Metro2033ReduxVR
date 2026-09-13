# Control-state watcher result

Date: 2026-08-17

The persisted pointer shortlist was watched automatically during one normal
run containing the opening scripted sequence, gameplay, and later scripted
events. The watcher produced many reversible transitions, but they occurred
at frame-scale intervals across large clusters of pointer slots. This is
allocation/object-lifetime churn, not a stable player-versus-script ownership
state. No candidate was promoted to a control-state hook.

The diagnostic watcher and sampler are disabled again in `VRPose.cpp`, and a
clean Release DLL was rebuilt and deployed. No further F7/F8 or pointer-watch
tests should be run.

The ARKTIKA.1 reference remains useful conceptually: it confirms that a native
4A VR implementation can separate VR-root, weapon, and runtime state. It did
not reveal a transferable Metro address or justify changing Metro's camera
logic.

Next work should use a direct engine call/state transition or an existing
weapon/player object path, not another broad memory correlation scan.
