// core/tick/frame_packet.h — the sim -> render extraction seam (Appendix
// A.1 phase 9; ENGINE_ARCHITECTURE_RESEARCH.md, Destiny frame packets):
// at tick-end the sim's writes are CLOSED and everything the renderer may
// read is published as one FramePacket into a double-buffered slot pair.
// The renderer never touches live sim state — it consumes the front packet
// while the sim fills the back one.
//
// Threading contract (designed now, exercised single-threaded until
// m2-jobs): the writer owns the back slot between begin_write() and
// publish(); publish() flips front/back. Under m2-jobs, publish() becomes a
// release-store of the front index and acquire() an acquire-load — the slot
// data itself never needs a lock because a slot is only ever touched by one
// side (that is the entire point of double buffering). Nothing here
// allocates, ever.
//
// Interpolation contract: the renderer draws BETWEEN ticks — it holds the
// front packet (tick N end-state) and blends toward it from its previous
// captured state using alpha = leftover-accumulator / dt, which the pacing
// side gets from TickLoop::advance() / interpolation_alpha(). The packet
// carries the alpha DENOMINATOR (dt_seconds) plus the tick number; the
// numerator lives with the pacer, since it changes per render frame while
// the packet changes per sim tick.

#pragma once

#include <array>
#include <cstdint>

namespace midday::tick {

// One published sim frame, opaque-for-now (m3 render extraction widens it;
// the SEAM — capture point, double buffering, tick stamping — is the
// contract, the payload is not yet).
struct FramePacket {
    std::uint64_t tick = 0;  // sim tick whose end-state this packet captures
    double dt_seconds = 0.0; // the fixed dt (interpolation-alpha denominator)
    double alpha_hint = 0.0; // leftover accumulator / dt at capture (advance()
                             // pacing only; 0 under direct tick() stepping)
    // Opaque transform-snapshot handle. Today: the tick number (world
    // transforms are propagated at phase 8, so "state as of tick N" IS the
    // handle). m3 replaces this with a real snapshot/extraction reference —
    // consumers must treat it as an opaque version token.
    std::uint64_t transform_snapshot = 0;
    std::uint64_t sequence = 0; // publish sequence, 1-based (stamped by the buffer)
};

class FramePacketBuffer {
public:
    // The back slot, for the writer to fill. Never aliases the front slot,
    // so a held front() pointer stays valid and untouched across a full
    // begin_write()/publish() of the NEXT packet (the reader reads N while
    // N+1 is written — pinned by tick.frame_packet).
    [[nodiscard]] FramePacket& begin_write() { return slots_[1 - front_]; }

    // Seal the back slot and flip: it becomes the front (readable) packet.
    // Stamps the packet's publish sequence. m2-jobs: this flip becomes the
    // release-store.
    void publish() {
        slots_[1 - front_].sequence = ++published_;
        front_ = 1 - front_;
    }

    // The latest published packet; nullptr before the first publish.
    [[nodiscard]] const FramePacket* front() const {
        return published_ == 0 ? nullptr : &slots_[front_];
    }

    [[nodiscard]] std::uint64_t published_count() const { return published_; }

private:
    std::array<FramePacket, 2> slots_{};
    std::uint32_t front_ = 0;     // index of the readable slot
    std::uint64_t published_ = 0; // total publishes (0 = nothing readable yet)
};

} // namespace midday::tick
