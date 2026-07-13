// M2: despawn-linger API (#14, with #13) — world.despawn(entity, {after})
// entered the authoring surface at M2 0B (#12b); the linger RUNTIME lands
// with track D. DO NOT rewrite the call: the corpse-linger cascade below is
// m4 content design intent, and deleting it to appease a checker would
// trade a tracked gap for a silent one.
import {StateScript, Transform, events, world} from 'midday'

export default class Dead extends StateScript {
  onEnter(from: string) {
    // depth-2 cascade: global broadcast; UI/score listeners react, no transition
    events.trigger('boss.died', {
      boss: this.entity,
      at: this.entity.get(Transform).position,
    }, {key: 'global'})
    world.despawn(this.entity, {after: 4.0})   // corpse lingers, then structural-apply removes it
  }
}
