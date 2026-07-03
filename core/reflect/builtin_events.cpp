#include "core/reflect/builtin_events.h"

#include "core/base/name.h"
#include "core/reflect/registry.h"
#include "core/reflect/type_model.h"

#include <string>
#include <utility>
#include <vector>

namespace midday::reflect {
namespace {

using base::Name;

EventFieldDesc field(std::string_view name, TypeKind kind, std::string doc) {
    return EventFieldDesc{Name(name), TypeDesc::scalar(kind), std::move(doc)};
}

EventDesc event(std::string_view name, std::string doc, std::vector<EventFieldDesc> payload) {
    EventDesc out;
    out.name = Name(name);
    out.doc = std::move(doc);
    out.payload = std::move(payload);
    return out;
}

} // namespace

void register_builtin_events(Registry& registry) {
    // Declaration order below IS the canonical enumeration order in
    // engine_api.json — append new vocabulary at the end, never reorder.

    registry.add_event(
        event("trigger.entered",
              "A body began overlapping a trigger volume. Key: the trigger entity.",
              {field("trigger", TypeKind::kEntityRef, "The trigger volume's entity."),
               field("other", TypeKind::kEntityRef, "The entity that entered.")}));

    registry.add_event(
        event("trigger.exited",
              "A body stopped overlapping a trigger volume. Key: the trigger entity.",
              {field("trigger", TypeKind::kEntityRef, "The trigger volume's entity."),
               field("other", TypeKind::kEntityRef, "The entity that exited.")}));

    registry.add_event(event(
        "contact.began",
        "Physics contact created between two bodies. Dispatched after the physics step in "
        "body-pair order (Appendix A phase 6). Key: each involved entity.",
        {field("self", TypeKind::kEntityRef, "The listening body's entity."),
         field("other", TypeKind::kEntityRef, "The other body's entity."),
         field("position", TypeKind::kVec3, "Contact point, world space."),
         field("normal", TypeKind::kVec3, "Contact normal, world space, from self toward other."),
         field("impulse", TypeKind::kFloat, "Total normal impulse of the first contact.")}));

    registry.add_event(
        event("contact.ended",
              "Physics contact between two bodies ceased. Dispatched after the physics step in "
              "body-pair order. Key: each involved entity.",
              {field("self", TypeKind::kEntityRef, "The listening body's entity."),
               field("other", TypeKind::kEntityRef, "The other body's entity.")}));

    registry.add_event(
        event("state.finished",
              "A sequence state's playhead reached its end (end mode 'finish'). Sequence chaining "
              "rides this event (spec 4.2). Key: the owning entity.",
              {field("entity", TypeKind::kEntityRef, "The entity owning the state machine."),
               field("region", TypeKind::kName, "The region containing the finished state."),
               field("state", TypeKind::kName, "The state whose sequence finished.")}));

    registry.add_event(
        event("entity.spawned",
              "An entity went live at structural apply (Appendix A phase 8), after its initial "
              "states entered. Key: the spawned entity.",
              {field("entity", TypeKind::kEntityRef, "The entity that spawned."),
               field("parent", TypeKind::kEntityRef, "The parent it was attached under.")}));

    registry.add_event(
        event("entity.despawned",
              "An entity was removed at structural apply, after its full exit chains ran; its "
              "handles read .alive == false. Key: the despawned entity.",
              {field("entity", TypeKind::kEntityRef, "The entity that despawned.")}));

    registry.add_event(
        event("action.pressed",
              "A named input action activated (Appendix A phase 2). Digital bindings report "
              "strength 1. Key: global.",
              {field("action", TypeKind::kName, "The action-map action name."),
               field("strength", TypeKind::kFloat, "Activation strength in [0, 1]."),
               field("device", TypeKind::kInt, "Originating device index; 0 is the primary.")}));

    registry.add_event(
        event("action.released",
              "A named input action deactivated. Key: global.",
              {field("action", TypeKind::kName, "The action-map action name."),
               field("device", TypeKind::kInt, "Originating device index; 0 is the primary.")}));
}

} // namespace midday::reflect
