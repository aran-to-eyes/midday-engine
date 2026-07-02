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
