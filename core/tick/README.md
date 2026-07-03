# core/tick

The engine's heartbeat (m0-tick-loop): the fixed tick loop running Appendix A.1's nine phases —
tick-begin, input, watchers, sequences, update, physics, post, structural-apply, tick-end — in
contractual order, every tick. Each phase journals a FLIGHT `tick.phase` marker; subsystems
attach to the five open phases via ordered hooks (`tick_loop.h`); structural mutation applies
exclusively at phase 8 (`World::flush_structural` + transform propagation); tick-end publishes
the double-buffered frame packet (`frame_packet.h`, the sim → render extraction seam, consumed
at m3). Stepping is deterministic fixed-dt (`tick()`, `run_to_tick`) plus a real-time
accumulator with loud max-catch-up clamping (`advance()`); the loop never reads a wall clock.
