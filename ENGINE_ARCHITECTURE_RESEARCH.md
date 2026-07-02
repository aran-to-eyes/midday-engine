# Engine Architecture & System Design — Verified Research Report

**Provenance**: deep-research run (183 agents, 8 search angles, 30 sources fetched, 3-vote
adversarial verification per claim). 36 claims survived verification (most 3-0, a few 2-1);
4 were refuted and are listed for transparency. Synthesis note: the run's synthesis stage
failed twice (stub output); this report was compiled directly from the journaled verified
claims by the session lead. Every factual statement below carries its vote and source.

Purpose: feed the Midday Engine architecture planning protocol. Each section ends with
**→ Midday**, mapping lessons onto the spec.

---

## 1. Job systems & frame architecture (the strongest material found)

**Naughty Dog — "Parallelizing the Naughty Dog Engine" (GDC 2015, Christian Gyrling)**
[gdcvault.com/play/1022186](https://gdcvault.com/play/1022186/Parallelizing-the-Naughty-Dog-Engine) · [slides PDF](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf) — all claims 3-0
- Fiber-based job system built to hit 60 fps on PS4 (TLOU Remastered): **6 worker threads
  locked to cores, 160 fibers (128×64 KiB + 32×512 KiB stacks), 3 priority queues, explicitly
  NO job stealing**.
- Synchronization exclusively via **atomic counters + one WaitForCounter primitive** — OS
  mutexes/semaphores can't be used because fibers migrate between threads; spin locks almost
  everywhere plus a special job-mutex that sleeps the job for longer holds.
- **Frame-centric design** (2-1): parallel work is structured around discrete frame boundaries,
  not free-running threads. Moving from a sequential game→render pipeline to a **feed-forward
  pipeline** (game logic, render logic, GPU each working on different frame numbers) cut
  critical-path CPU time **25 ms → 15.5 ms**.

**Bungie — "Multithreading the Entire Destiny Engine" + Destiny renderer (GDC 2015, Tatarchuk)**
[gdcvault.com/play/1022164](https://gdcvault.com/play/1022164/Multithreading-the-Entire-Destiny) · [renderer slides](https://advances.realtimerendering.com/destiny/gdc_2015/Tatarchuk_GDC_2015__Destiny_Renderer_web.pdf) — 3-0
- Whole game loop restructured as a **job graph on fibers**, minimizing thread preemption.
- Renderer organized by **views** (camera/shadow/reflection): each view is a data-driven job
  chain (visibility → populate render nodes → extract → prepare → submit), created only when
  that view is actually needed this frame.
- All per-frame dynamic render data lives in a **double-buffered "frame packet"** with
  cache-coherent render-node arrays; **write access closes after the prepare phase** — no job
  may mutate it once GPU command generation begins.
- A **runtime resource tracking/validation system** guarantees all reads/writes/creates/destroys
  across the job graph happen only at safe points in the frame.
- Feature engineers write plain per-object extract/prepare/submit entry points; the core
  **auto-jobifies and batches them** — threading complexity is encapsulated, not exported.

**→ Midday**: our Appendix A tick phases are the sim-side analogue of frame-centric design —
the render side should mirror it: a **frame-packet extraction boundary** (sim closes writes,
render reads a snapshot) is the natural partner to our tick-boundary snapshots. Bungie's
safe-point validation system is the same idea as our phase-8 structural-apply, generalized —
worth adopting as a debug-build invariant checker. The fiber-vs-thread-pool decision is a
planning-protocol node; either way, "atomic counters + WaitForCounter, no OS primitives in
jobs" is the proven synchronization vocabulary.

## 2. Renderer architecture: frame graphs, backends, bindless

**Frostbite — "FrameGraph: Extensible Rendering Architecture" (GDC 2017)**
[gdcvault.com/play/1024612](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in) — 3-0 / 2-1
- A frame is an **explicit graph of render passes + resource dependencies** rather than
  imperative draw/dispatch; the graph enables automatic resource lifetime/aliasing and pass
  culling while keeping feature code modular — the core architectural pitch.

**The Forge** — [github.com/confettifx/the-forge](https://github.com/confettifx/the-forge) — 3-0
- Unified renderer interface over DX12/Vulkan/Metal across desktop/mobile/console — a working
  reference for one-API-over-many-backends.
- Ships as **modular building blocks** (resource loader, Lua scripting, Ozz-based animation,
  math, memory, input, flecs-based ECS, filesystem C API, ImGui UI) rather than a monolith.

**bgfx** — [bkaradzic.github.io/bgfx](https://bkaradzic.github.io/bgfx/overview.html) — 3-0
- Explicitly "Bring Your Own Engine": a graphics-API-agnostic rendering *library* meant to be
  embedded — the cleanest example of renderer-as-component.

**Bindless Vulkan — "Setting up a bindless rendering pipeline" (Vulkanised 2023)**
[slides](https://vulkan.org/user/pages/09.events/vulkanised-2023/vulkanised_2023_setting_up_a_bindless_rendering_pipeline.pdf) — 3-0 / 2-1
- Four descriptor pool types (Buffer, Texture, RwTexture, Tlas) + immutable samplers, sized
  against Vulkan 1.2's guaranteed 4 descriptor sets.
- Upper-bound pools (~100k descriptors/type) with UPDATE_AFTER_BIND / PARTIALLY_BOUND /
  VARIABLE_DESCRIPTOR_COUNT; **one shared DescriptorSetLayout + PipelineLayout for everything**;
  per-draw resource indices via push constants.
- **Resource handles as packed 32-bit generational indices** (23-bit index · 2-bit type tag ·
  1-bit RW · 6-bit generation) to catch stale/use-after-free GPU references.

**→ Midday**: FrameGraph independently validates our pipeline-as-data SRP — the render-graph
document IS a frame graph; aliasing + pass culling should be listed as validator/compiler
features of it. The bindless talk's generational GPU handles are literally our entity-handle
pattern applied to GPU resources — one handle discipline engine-wide. The Forge and bgfx are
the reference codebases to read when shaping the RHI seam.

## 3. ECS storage — the sparse-set vs archetype fork, from the authors

**EnTT (sparse-set)** — [wiki](https://github.com/skypjack/entt/wiki/Entity-Component-System) — 3-0 / 2-1
- Paged sparse + packed arrays; **no compile-time or runtime declaration of the component set**.
- Pay-per-use philosophy: iteration is deliberately NOT globally optimized at the cost of
  add/remove; expensive layouts (**groups**, which take ownership of pools and physically
  rearrange them) are opt-in against cheap non-owning **views**.

**flecs (archetype)** — [FAQ](https://www.flecs.dev/flecs/md_docs_2FAQ.html) — 3-0
- Entities with identical component sets stored in contiguous tables → cache utilization,
  vectorization, fast queries.
- **ECS operations from inside a running system are deferred (queued) and applied when safe** —
  never mutated mid-iteration.

**Survey** — AFIT thesis on ECS storage ([scholar.afit.edu](https://scholar.afit.edu/cgi/viewcontent.cgi?article=6355&context=etd)) — 3-0:
systematic documentation of archetype vs sparse-set models with relational-database parallels —
the neutral map of the design space.

**→ Midday**: flecs's deferred-ops rule is exactly our phase-8 structural apply — independent
convergence, good sign. Our statechart model (components toggling active/inactive per state,
never constructed/destroyed) interacts differently with the two storage models: state toggles
are archetype *moves* in archetype storage (costly if frequent) but cheap flag/pool operations
in sparse-set storage — **this is a load-bearing input to the storage decision the planning
protocol must weigh**. EnTT groups offer a hybrid: sparse-set base + opt-in packed layouts for
hot queries.

## 4. Physics — Jolt's internals (from its author's Architecting Jolt Physics talk)

[jrouwe.nl notes PDF](https://jrouwe.nl/architectingjolt/ArchitectingJoltPhysics_Rouwe_Jorrit_Notes.pdf) · [repo](https://github.com/jrouwe/JoltPhysics) — all 3-0
- **Lock-free quad-tree broad phase**: replacing 1 global RW-mutex with 32 reduced per-thread
  mutex wait ~500 µs → ~10 µs (PS5, 13 worker threads); no benefit beyond 32.
- **Island generation as lock-free Union-Find** running concurrently during the narrow phase;
  cut single-threaded island building by 80% (1 thread) / 70% (4 CPUs).
- Core principle: **bodies are unaware of each other** (no direct contact/constraint tracking
  between objects) — eliminating shared mutable state that would need cross-object locks.
- **Determinism is a design goal**: simulation replicable to a remote client by replicating
  inputs only — directly the property our replay contract relies on.

**→ Midday**: validates the Jolt decision with measured evidence. The "no shared mutable state
between bodies" principle is the same instinct as our keyed-bus capability scoping — worth
stating as a general engine principle. The lock-free island approach is the reference when the
planning protocol weighs replay-mode vs play-mode threading configs.

## 5. The canon — books, verified for what they actually teach

- **Game Engine Architecture, 4th ed. (Gregory)** — 3-0: the 4th edition adds two new chapters
  on GPU programming/rendering/lighting (mesh & amplification shaders, GI, radiosity, ray
  tracing) — the edition to buy for Vulkan-era relevance. [gameenginebook.com](https://www.gameenginebook.com/)
- **Game Programming Patterns (Nystrom)** — 3-0: Component pattern as a first-class decoupling
  chapter; Data Locality and Spatial Partition treated as named architectural patterns
  (alongside Dirty Flag, Object Pool). [gameprogrammingpatterns.com](https://gameprogrammingpatterns.com/contents.html)
- **Data-Oriented Design (Fabian)** — 3-0: core message "It's all about the data" — data
  primacy over object abstraction, cache-friendly sequential layouts. **Citation warning
  (2-1)**: `dataorienteddesign.com/dodmain/` is an explicitly superseded 2013 beta draft kept
  for link preservation — cite the canonical `dodbook` edition instead.
- **Foundations of Game Engine Development (Lengyel)** — 3-0: Vol 1 = Mathematics, Vol 2 =
  Rendering; the from-first-principles complement to Gregory's breadth.

**→ Midday**: Gregory 4th ed + GPP + DOD (canonical edition) + Lengyel Vol 1–2 are the agent
context library for engine-building sessions.

## 6. Where the practitioner archives survive (fetched and confirmed live)

- **Our Machinery blog** (original site gone): mirrors at
  [ruby0x1.github.io/machinery_blog_archive](https://ruby0x1.github.io/machinery_blog_archive/)
  (incl. the fiber-based-job-system post), [github.com/vorg/machinery-blog-archive](https://github.com/vorg/machinery-blog-archive),
  and [archive.org/details/our-machinery-blog](https://archive.org/details/our-machinery-blog).
- **Bitsquid/Stingray blog** — live at [bitsquid.blogspot.com](http://bitsquid.blogspot.com/)
  (the data-driven renderer walkthrough series, e.g. part 7, fetched this run).
- **Molecular Matters / Molecule Engine** — live at
  [blog.molecular-matters.com](https://blog.molecular-matters.com/) (job-system tag series).
- GDC primaries: the Naughty Dog talk also survives on
  [archive.org](https://archive.org/details/GDC2015Gyrling_201508); Insomniac's "SIMD at
  Insomniac Games" at [gdcvault.com/play/1022249](https://gdcvault.com/play/1022249/SIMD-at-Insomniac-Games-How).

## 7. Refuted claims (transparency — killed by adversarial verification)

- Gregory's book scope framed as "the canonical subsystem decomposition" (1-2) — overreach.
- DOD's "There is no Entity" as *the* book's central stance (0-3) — misread of the source.
- bgfx "11 backends incl. GNM/WebGPU-via-Dawn" (0-3) — enumeration didn't match current docs.
- Jolt "narrow phase runs in the background across frames" (0-3) — misread of its pipeline.

## 8. Coverage caveats

Verification concentrated on job systems, renderer architecture, ECS storage, physics
internals, and the book canon. Thinner or unverified this run: asset-pipeline/content-build
literature, hot-reload write-ups, scripting-runtime embedding, rollback-netcode/fighting-game
determinism literature, and editor-architecture material — worth targeted reads during
planning rather than another research pass. Unreal/Godot/Bevy/O3DE architecture claims did not
survive to the confirmed set this run (Godot is already covered in depth by
GODOT_REFERENCE_SPEC.md).
