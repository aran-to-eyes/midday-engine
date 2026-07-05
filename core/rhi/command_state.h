// core/rhi/command_state.h — THE record -> submit state machine, shared by
// every backend (m0-rhi-vulkan). A command list is Initial -> (begin) ->
// Recording -> (end) -> Ready -> (submit) -> Initial; render passes,
// pipeline/vertex/texture bindings are tracked inside the Recording state.
// Backends call the transition checks BEFORE touching their API, so the
// seam's validation semantics are identical across NullDevice, Vulkan, and
// Metal by construction — the rhi.null tests prove the machine once, for all
// of them.

#pragma once

#include "core/base/error.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace midday::rhi {

enum class CommandListPhase : std::uint8_t {
    kInitial,   // freshly created, or reset by a completed submit
    kRecording, // between begin() and end()
    kReady,     // ended; exactly one submit consumes it back to kInitial
};

class CommandListState {
public:
    [[nodiscard]] CommandListPhase phase() const { return phase_; }

    [[nodiscard]] bool pass_open() const { return pass_open_; }

    // begin(): kInitial -> kRecording. Beginning a kReady list is a
    // deliberate RESET (backends reset the underlying buffer); only a list
    // that is mid-recording refuses.
    [[nodiscard]] std::optional<base::Error> begin() {
        if (phase_ == CommandListPhase::kRecording)
            return err("rhi.already_recording", "command list is already recording");
        phase_ = CommandListPhase::kRecording;
        reset_pass_state();
        return std::nullopt;
    }

    [[nodiscard]] std::optional<base::Error> begin_render_pass() {
        if (auto error = require_recording())
            return error;
        if (pass_open_)
            return err("rhi.pass_active", "a render pass is already open on this command list");
        pass_open_ = true;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<base::Error> bind_pipeline(bool needs_texture) {
        if (auto error = require_pass("bind_pipeline"))
            return error;
        pipeline_bound_ = true;
        pipeline_needs_texture_ = needs_texture;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<base::Error> bind_vertex_buffer() {
        if (auto error = require_pass("bind_vertex_buffer"))
            return error;
        vertex_bound_ = true;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<base::Error> bind_texture() {
        if (auto error = require_pass("bind_texture"))
            return error;
        texture_bound_ = true;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<base::Error> draw() {
        if (auto error = require_pass("draw"))
            return error;
        if (!pipeline_bound_)
            return err("rhi.no_pipeline", "draw requires a bound pipeline");
        if (!vertex_bound_)
            return err("rhi.no_vertex_buffer", "draw requires a bound vertex buffer");
        if (pipeline_needs_texture_ && !texture_bound_)
            return err("rhi.texture_missing",
                       "the bound pipeline samples a texture but none is bound");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<base::Error> end_render_pass() {
        if (auto error = require_recording())
            return error;
        if (!pass_open_)
            return err("rhi.no_pass", "no render pass is open");
        reset_pass_state();
        return std::nullopt;
    }

    // end(): kRecording (no open pass) -> kReady.
    [[nodiscard]] std::optional<base::Error> end() {
        if (auto error = require_recording())
            return error;
        if (pass_open_)
            return err("rhi.pass_active", "end() with a render pass still open");
        phase_ = CommandListPhase::kReady;
        return std::nullopt;
    }

    // submit(): kReady -> kInitial (the synchronous submit completed; the
    // list is reusable via a fresh begin()).
    [[nodiscard]] std::optional<base::Error> submit() {
        if (phase_ != CommandListPhase::kReady)
            return err("rhi.not_ready", "submit requires an ended (ready) command list");
        phase_ = CommandListPhase::kInitial;
        reset_pass_state();
        return std::nullopt;
    }

private:
    static base::Error err(std::string_view code, std::string_view message) {
        return base::Error{.code = std::string(code), .message = std::string(message)};
    }

    [[nodiscard]] std::optional<base::Error> require_recording() const {
        if (phase_ != CommandListPhase::kRecording)
            return err("rhi.not_recording", "command list is not recording (call begin first)");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<base::Error> require_pass(std::string_view what) const {
        if (auto error = require_recording())
            return error;
        if (!pass_open_)
            return err("rhi.no_pass", std::string(what) + " requires an open render pass");
        return std::nullopt;
    }

    void reset_pass_state() {
        pass_open_ = false;
        pipeline_bound_ = false;
        vertex_bound_ = false;
        texture_bound_ = false;
        pipeline_needs_texture_ = false;
    }

    CommandListPhase phase_ = CommandListPhase::kInitial;
    bool pass_open_ = false;
    bool pipeline_bound_ = false;
    bool vertex_bound_ = false;
    bool texture_bound_ = false;
    bool pipeline_needs_texture_ = false;
};

} // namespace midday::rhi
