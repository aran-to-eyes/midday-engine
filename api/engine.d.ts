// engine.d.ts -- GENERATED from engine_api.json. DO NOT EDIT.
// engine_version 0.1.0, api_compat_hash a30db5ed89f735df (signatures only; docs excluded).
// Formatting rules + the TypeDesc -> TypeScript mapping table: api/CODEGEN.md.
// Structural (pre-tsc) validation conventions: formats/engine_dts.meta.md.

declare namespace midday {
    // -- Value types (fixed preamble; scalar TypeDesc spellings map per api/CODEGEN.md) --

    /** TypeDesc "vec2": 2D float vector. */
    interface Vec2 {
        x: number;
        y: number;
    }

    /** TypeDesc "vec3": 3D float vector. */
    interface Vec3 {
        x: number;
        y: number;
        z: number;
    }

    /** TypeDesc "vec4": 4D float vector. */
    interface Vec4 {
        x: number;
        y: number;
        z: number;
        w: number;
    }

    /** TypeDesc "quat": rotation quaternion; JSON spelling [x, y, z, w]. */
    interface Quat {
        x: number;
        y: number;
        z: number;
        w: number;
    }

    /** TypeDesc "color": linear RGBA; JSON spelling [r, g, b, a]. */
    interface Color {
        r: number;
        g: number;
        b: number;
        a: number;
    }

    /** TypeDesc "entity_ref": generational entity handle; a stale handle reads alive == false. */
    interface EntityRef {
        readonly alive: boolean;
    }

    /** TypeDesc "asset_ref": project-root-relative asset path. */
    type AssetRef = string;

    // -- Reflected classes (engine_api.json "classes", registration order) --

    /** Class name -> reflected interface. */
    interface Classes {}

    // -- Event payloads (engine_api.json "events", registration order) --

    /** A body began overlapping a trigger volume. Key: the trigger entity. */
    interface TriggerEnteredEvent {
        /** The trigger volume's entity. */
        trigger: EntityRef;
        /** The entity that entered. */
        other: EntityRef;
    }

    /** A body stopped overlapping a trigger volume. Key: the trigger entity. */
    interface TriggerExitedEvent {
        /** The trigger volume's entity. */
        trigger: EntityRef;
        /** The entity that exited. */
        other: EntityRef;
    }

    /** Physics contact created between two bodies. Dispatched after the physics step in body-pair order (Appendix A phase 6). Key: each involved entity. */
    interface ContactBeganEvent {
        /** The listening body's entity. */
        self: EntityRef;
        /** The other body's entity. */
        other: EntityRef;
        /** Contact point, world space. */
        position: Vec3;
        /** Contact normal, world space, from self toward other. */
        normal: Vec3;
        /** Total normal impulse of the first contact. */
        impulse: number;
    }

    /** Physics contact between two bodies ceased. Dispatched after the physics step in body-pair order. Key: each involved entity. */
    interface ContactEndedEvent {
        /** The listening body's entity. */
        self: EntityRef;
        /** The other body's entity. */
        other: EntityRef;
    }

    /** A sequence state's playhead reached its end (end mode 'finish'). Sequence chaining rides this event (spec 4.2). Key: the owning entity. */
    interface StateFinishedEvent {
        /** The entity owning the state machine. */
        entity: EntityRef;
        /** The region containing the finished state. */
        region: string;
        /** The state whose sequence finished. */
        state: string;
    }

    /** An entity went live at structural apply (Appendix A phase 8), after its initial states entered. Key: the spawned entity. */
    interface EntitySpawnedEvent {
        /** The entity that spawned. */
        entity: EntityRef;
        /** The parent it was attached under. */
        parent: EntityRef;
    }

    /** An entity was removed at structural apply, after its full exit chains ran; its handles read .alive == false. Key: the despawned entity. */
    interface EntityDespawnedEvent {
        /** The entity that despawned. */
        entity: EntityRef;
    }

    /** A named input action activated (Appendix A phase 2). Digital bindings report strength 1. Key: global. */
    interface ActionPressedEvent {
        /** The action-map action name. */
        action: string;
        /** Activation strength in [0, 1]. */
        strength: number;
        /** Originating device index; 0 is the primary. */
        device: number;
    }

    /** A named input action deactivated. Key: global. */
    interface ActionReleasedEvent {
        /** The action-map action name. */
        action: string;
        /** Originating device index; 0 is the primary. */
        device: number;
    }

    /** Event name -> payload type. */
    interface EventPayloads {
        "trigger.entered": TriggerEnteredEvent;
        "trigger.exited": TriggerExitedEvent;
        "contact.began": ContactBeganEvent;
        "contact.ended": ContactEndedEvent;
        "state.finished": StateFinishedEvent;
        "entity.spawned": EntitySpawnedEvent;
        "entity.despawned": EntityDespawnedEvent;
        "action.pressed": ActionPressedEvent;
        "action.released": ActionReleasedEvent;
    }

    // -- Expression functions (engine_api.json "functions"): expression-language signatures for editor tooling, not TS-callable --

    namespace expr {
        /** Convert float to int, truncating toward zero. NaN or a value outside int64 range is a runtime expression error. */
        function int(x: number): number;
        /** Convert int to float32 (IEEE round-to-nearest). */
        function float(x: number): number;
        /** Absolute value. */
        function abs(x: number): number;
        /** 1 for positive, -1 for negative, 0 for zero and NaN. */
        function sign(x: number): number;
        /** Largest integral value <= x (exact). */
        function floor(x: number): number;
        /** Smallest integral value >= x (exact). */
        function ceil(x: number): number;
        /** Nearest integral value, halves away from zero (exact). */
        function round(x: number): number;
        /** Integral value toward zero (exact). */
        function trunc(x: number): number;
        /** Fractional part: x - floor(x). */
        function fract(x: number): number;
        /** Square root (IEEE correctly rounded). Negative input yields NaN; comparisons against NaN are false. */
        function sqrt(x: number): number;
        /** Smaller of a and b (b < a selects b; NaN operands select a). */
        function min(a: number, b: number): number;
        /** Larger of a and b (a < b selects b; NaN operands select a). */
        function max(a: number, b: number): number;
        /** x clamped to [lo, hi]. */
        function clamp(x: number, lo: number, hi: number): number;
        /** x clamped to [0, 1]. */
        function saturate(x: number): number;
        /** Linear blend a + (b - a) * t. */
        function lerp(a: number, b: number, t: number): number;
        /** Construct a vec2 from components. */
        function vec2(x: number, y: number): Vec2;
        /** Construct a vec3 from components. */
        function vec3(x: number, y: number, z: number): Vec3;
        /** Construct a vec4 from components. */
        function vec4(x: number, y: number, z: number, w: number): Vec4;
        /** Construct a quaternion from raw components (NOT normalized; rotation use requires unit length — core/math policy). */
        function quat(x: number, y: number, z: number, w: number): Quat;
        /** Dot product. */
        function dot(a: Vec3, b: Vec3): number;
        /** Cross product (right-handed). */
        function cross(a: Vec3, b: Vec3): Vec3;
        /** Euclidean length. */
        function length(v: Vec3): number;
        /** Squared length (no sqrt — cheaper for radius checks). */
        function length_squared(v: Vec3): number;
        /** Unit vector; the zero vector normalizes to zero (core/math policy). */
        function normalize(v: Vec3): Vec3;
        /** Euclidean distance between points. */
        function distance(a: Vec3, b: Vec3): number;
        /** Squared distance (no sqrt — cheaper for radius checks). */
        function distance_squared(a: Vec3, b: Vec3): number;
        /** Rotate v by the UNIT quaternion q. */
        function rotate(q: Quat, v: Vec3): Vec3;
    }

    // -- CLI verbs (engine_api.json "verbs"): midday argv schemas as types, manifest order --

    /** print engine name, version, and build info */
    interface VersionVerbArgs {}

    /** run the doctest registry embedded in the engine binary */
    interface SelftestVerbArgs {
        /** doctest test-case filter pattern (e.g. 'cli.*') */
        filter?: string;
    }

    /** show the verb list or one verb's flags and usage */
    interface HelpVerbArgs {
        /** verb to describe; omit for the full verb list */
        verb?: string;
    }

    /** emit, diff, or generate from engine_api.json, the canonical API document */
    interface ApiVerbArgs {
        /** dump: write the document to this path instead of printing it */
        out?: string;
        /** codegen: directory for the four generated artifacts */
        "out-dir"?: string;
        /** codegen: TS toolchain content-hash cache (regenerable, never committed) */
        "cache-dir"?: string;
        /** codegen: run the self-hosted TS-on-QuickJS generator (the default) */
        selfhost?: boolean;
        /** codegen: run the TEMPORARY native bootstrap generator instead */
        bootstrap?: boolean;
        /** codegen: run BOTH generators and byte-compare all four artifacts */
        "verify-equivalence"?: boolean;
        /** dump | diff | codegen */
        action: string;
        /** diff: baseline engine_api.json; codegen: input document (default api/engine_api.json) */
        input?: string;
    }

    /** typecheck, lint, transpile, and benchmark TypeScript on the embedded runtime */
    interface ScriptVerbArgs {
        /** content-hash cache directory (regenerable, never committed) */
        "cache-dir"?: string;
        /** build: report {transpiled, cache_hits} counters in the payload */
        stats?: boolean;
        /** bench: entity count for the budget sweep */
        entities?: number;
        /** bench: measured ticks (after warmup) */
        ticks?: number;
        /** bench: unmeasured warmup ticks before the window */
        warmup?: number;
        /** bench: per-field host-hook accessors (the chatty comparison mode) */
        naive?: boolean;
        /** check | build | bench */
        action: string;
        /** TypeScript source file (bench: overrides the committed fixture) */
        path?: string;
    }

    /** load a scene and step the deterministic sim headless (FLIGHT-recorded) */
    interface RunVerbArgs {
        /** run exactly N fixed ticks */
        ticks?: number;
        /** run until the sim tick reaches N */
        "to-tick"?: number;
        /** sim seed (journal identity + RNG streams) */
        seed?: number;
        /** run.mrj bundle path (default: the .midday-cache/run/last.mrj scratch bundle) */
        record?: string;
        /** TS build cache directory (default: .midday-cache/ts) */
        "cache-dir"?: string;
        /** drive + verify a registered assertion pack: case=<name> (available: appendix_a_golden, determinism_kata) */
        assert?: string;
        /** the *.scene.yaml to load and run */
        scene: string;
    }

    /** interrogate run.mrj bundles (diff: first-divergent-tick over two runs) */
    interface JournalVerbArgs {
        /** operation: diff */
        op: string;
        /** first run.mrj bundle */
        a: string;
        /** second run.mrj bundle */
        b: string;
    }

    /** GPU seam tools (probe: device availability/caps; render: M0 scenes to PNG + decoded-pixel hashes) */
    interface RhiVerbArgs {
        /** seam implementation: vulkan | metal (metal is macOS-only) */
        backend?: string;
        /** enable the Vulkan validation layer (refuses if not installed) */
        validation?: boolean;
        /** require a software rasterizer (lavapipe class; golden lane sets this) */
        software?: boolean;
        /** render one scene: clear | triangle | textured_quad (default: all) */
        scene?: string;
        /** write <scene>.png + <scene>.hash + driver.txt here (render) */
        "out-dir"?: string;
        /** compare decoded-pixel hashes against this golden dir (render) */
        goldens?: string;
        /** operation: probe | render */
        op: string;
    }

    /** screenshot tools (compare: two-tier golden comparison — decoded-pixel hash + explicit-threshold tolerance, optional diff image) */
    interface ShotVerbArgs {
        /** per-channel delta a pixel may carry without counting as over (0-255) */
        tolerance?: number;
        /** percent of pixels allowed over --tolerance before tier 2 fails */
        "max-pct-over"?: number;
        /** mean absolute channel delta budget (perceptual drift bound) */
        "max-mean"?: number;
        /** write an amplified per-pixel delta image (x8, saturated) to this PNG path */
        diff?: string;
        /** operation: compare */
        op: string;
        /** first PNG (golden/reference) */
        a: string;
        /** second PNG (candidate) */
        b: string;
    }

    /** validate a strict-YAML file against a schema_manifest.json format entry */
    interface ValidateVerbArgs {
        /** format name to look up in the schema manifest's formats[] table */
        schema?: string;
        /** schema manifest path, used with --schema (default: api/schema_manifest.json) */
        manifest?: string;
        /** a standalone format-entry JSON document (bypasses the manifest) */
        "schema-file"?: string;
        /** the strict-YAML file to validate */
        file: string;
    }

    /** canonicalize a strict-YAML file (schema-agnostic; see: midday validate) */
    interface FmtVerbArgs {
        /** rewrite the file in place with its canonical form */
        write?: boolean;
        /** exit 1 without writing if the file is not already canonical */
        check?: boolean;
        /** the strict-YAML file to canonicalize */
        file: string;
    }

    /** audit {uid, path} asset references against the project's .uid sidecars */
    interface CheckVerbArgs {
        /** repair fixable drift/missing-uid findings in place */
        fix?: boolean;
        /** uid registry cache directory (default: <root>/.midday-cache/uid) */
        "cache-dir"?: string;
        /** project directory to scan */
        root: string;
    }

    /** move an asset (+ its .uid sidecar) and rewrite referencing paths; the uid never changes */
    interface MvVerbArgs {
        /** directory to scan for referencing files (default: the current directory) */
        root?: string;
        /** uid registry cache directory (default: <root>/.midday-cache/uid) */
        "cache-dir"?: string;
        /** the asset's current path */
        src: string;
        /** the asset's new path */
        dst: string;
    }

    /** Verb name -> parsed-argument type. */
    interface VerbArgsByName {
        "version": VersionVerbArgs;
        "selftest": SelftestVerbArgs;
        "help": HelpVerbArgs;
        "api": ApiVerbArgs;
        "script": ScriptVerbArgs;
        "run": RunVerbArgs;
        "journal": JournalVerbArgs;
        "rhi": RhiVerbArgs;
        "shot": ShotVerbArgs;
        "validate": ValidateVerbArgs;
        "fmt": FmtVerbArgs;
        "check": CheckVerbArgs;
        "mv": MvVerbArgs;
    }
}
