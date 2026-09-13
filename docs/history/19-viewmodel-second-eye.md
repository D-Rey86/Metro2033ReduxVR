# The viewmodel's missing second eye

The gun, hands and watch rendered only in the left eye from the moment true
stereo started. Three separate bugs stacked on top of each other, and each one
hid the next.

## 1. The second eye's draw was never issued

The viewmodel takes its own early-return path in `DrawIndexedInstanced`: it
substitutes its instance buffer, draws, restores, and returns - **before
reaching `BeginTwinPass`**. So the second eye's weapon draw simply never
happened. The code that re-runs the weapon transform with the right eye's
offset lives inside `BeginTwinPass`, was correct, and had never once been
called for the object it was written for.

## 2. Adding it broke the left eye

Every other draw path calls `RestoreEye0IfNeeded()` before drawing; this one
never had to. With a twin pass now running, the constant buffer was left
holding the RIGHT eye's matrices, so the NEXT viewmodel mesh drew its left eye
with them. The viewmodel is several meshes - gun, hands, watch - which is
exactly why they came apart from one another.

## 3. The real one: D3D11 unbinds a vertex buffer used as a UAV

Even with the draw issued into properly twinned targets, and even drawn with
the LEFT eye's own transform so it could not be out of frustum, the twin image
had no gun.

The re-dispatch binds the weapon's private instance buffer as a UAV so the
compute shader can write the transform into it. **D3D11 will not have a
resource bound as a vertex buffer and as a UAV simultaneously - binding it as a
UAV silently unbinds it from the input assembler, and clearing the UAV does not
put it back.** The second eye's draw ran with no per-instance data and produced
nothing.

It never showed before because the dispatch used to happen during a LATER
draw's twin pass, long after the viewmodel had been drawn. Issuing the second
eye's draw immediately after it is what exposed the unbind. Fixed by
re-binding the buffer to IA slot 1 after the dispatch.

## What actually found it

Three hypotheses were wrong first - stale caches, an untwinned colour target,
and the eye offset throwing the gun out of view - and each cost a run. What
settled it was two diagnostics that split the possibilities rather than testing
a guess:

- **counters** for viewmodel meshes drawn per eye and transform redispatches:
  11 / 11 / 11 proved everything was executing, killing "the draw never
  happens";
- **`kBothEyesFrom = 1`**, showing the twin image to both eyes: no gun there
  either, killing "it is drawn but out of view" and leaving only "the draw
  produces nothing".

Guessing causes cost three runs; splitting the space cost two and ended it.

## Still open

The viewmodel now renders in both eyes but is too SMALL and sits ABOVE the
controller, in alternate-eye and true stereo alike, dating from when the stereo
work began. See task #27: the camera patch compensates for the
culling-cancellation pitch (HackerContext.cpp:1771, :1809) but the weapon
transform path never references it, and the viewmodel transform is built in
view space - so it is carried by a camera pitched away from the real head pose,
which displaces it upward and pushes it away.
