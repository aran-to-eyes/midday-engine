# core/bus

The keyed, journaled, immediate event bus (m0-event-bus) — the game's nervous
system and its ONLY transition mechanism (spec §4.2).

- `bus.h` / `bus.cpp` — `EventKey` (named group channels + entity-private
  channels; keys ARE capabilities), `EventListener`, `Bus`:
  `subscribe(listener, key)` / `trigger(key, event, payload, cause_id)` with
  IMMEDIATE registration-order dispatch on the same call stack, re-entrant
  cascades capped at depth 32 (`bus.cascade_depth` to the offending call, no
  unwinding), subscribe/unsubscribe deferred to dispatch end while
  dispatching, and typed payload validation against the reflect event
  vocabulary. Every accepted trigger writes a FLIGHT `event.trigger` record
  BEFORE dispatch; its id is the cause id listeners chain their effects from.
- `entity_listener.h` — the ECS bridge: `subscribe_component<T>` binds
  (entity, component-type) via a re-fetching thunk (pool rows move; pointers
  are never cached) with lazy generation-check auto-unsubscribe on despawn
  and active-by-default delivery (dormant components hear nothing).
- `test_support.h`, `bus_test.cpp`, `bus_journal_test.cpp` — the `bus.*`
  selftests: registration order (incl. unsubscribe/resubscribe), key
  isolation, depth-33 structured error with the journaled chain, byte-pinned
  trigger records, cause-chain walks, and dual-run record identity.

Semantics and cost model: the `bus.h` header comment. Decisions:
D-BUILD-046..049. Next consumers: m0-statechart-core (transition evaluation
subscribes here), m0-tick-loop (drives `set_tick`), m0-jolt-minimal
(contact.* in phase-6 order).
