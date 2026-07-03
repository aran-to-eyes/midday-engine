// midday/batch — the batch-first binding surface (spec §7, m0-batch-bindings).
// Scripts request typed-array-backed SoA views ONCE; the engine refreshes
// them once per phase and commits writable columns back at one deterministic
// point. Boundary crossings are O(exposed buffers), never O(entities).
//
// Protocol: re-read view.buffers.<field> at each tick entry — on capacity
// growth the engine detaches the old ArrayBuffers (stale references become
// length-0, deterministically) and publishes fresh ones under the same keys.

export type BatchBufferArray = Float32Array | Float64Array | Uint8Array;

export interface BatchView {
    readonly component: string;
    readonly fields: readonly string[];
    /** Active-join row count this phase; buffers may have spare capacity. */
    readonly count: number;
    readonly buffers: Readonly<Record<string, BatchBufferArray>>;
}

export interface BatchEnvelope {
    readonly envelope_version: number;
    /** Stamped by the engine at every refresh. */
    readonly tick: number;
    /** One view per requested component, request order; rows are aligned
     *  across views (row i is the same entity in every view). */
    readonly views: readonly BatchView[];
}

export interface BatchRequest {
    components: { component: string; fields: string[] }[];
}

declare function __midday_batch_request(desc: BatchRequest): {
    request: number;
    envelope_version: number;
};
declare const __midday_batch_envelopes: BatchEnvelope[];

// Request a live envelope. Call once at module setup; the same object is
// refreshed in place every phase. Throws on spec drift: version 0 is the
// generated placeholder and must never batch (bindings_spec.json contract).
export function request(desc: BatchRequest): BatchEnvelope {
    const granted = __midday_batch_request(desc);
    if (granted.envelope_version !== 1)
        throw new Error(
            "bindings.envelope_version: engine speaks batch envelope version " +
                granted.envelope_version +
                "; midday/batch requires version 1 (version 0 must be refused)",
        );
    return __midday_batch_envelopes[granted.request];
}

// Install the per-tick entry the engine invokes after each refresh.
export function onTick(fn: (tick: number) => void): void {
    (globalThis as { __midday_batch_tick?: (tick: number) => void }).__midday_batch_tick = fn;
}
