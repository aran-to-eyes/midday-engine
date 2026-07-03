# core/physics

The minimal deterministic physics server over vendored JoltPhysics
(m0-jolt-minimal; spec §6). M0 surface: dynamic boxes + a static ground
plane behind opaque `PhysicsBodyId` handles (bridged to entities via the
`PhysicsBody` component), a fixed-dt Jolt step on tick phase 6, transform
sync into hierarchy local transforms (body-id order, before structural
apply), and `contact.began`/`contact.ended` on the bus — collected during
the step, triggered after it, sorted by body-pair id, cause = the phase-6
marker.

Determinism: the thread config is LOCKED in deterministic mode
(`physics.config_locked` on divergence); Jolt itself builds with
`JPH_CROSS_PLATFORM_DETERMINISTIC` under the repo FP contract
(third_party/CMakeLists.txt, D-BUILD-073). No Jolt type escapes
`physics_server.cpp`. m4-physics-full expands to the whole rigid-body
surface behind the same seam.
