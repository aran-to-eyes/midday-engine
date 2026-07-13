// examples/lifecycle/states/armed.ts — the Armed substate's brain (M2 node
// 0B golden, FUSED-SPEC D6). Hook-only, like parent.ts: exit:Armed is the
// exit chain's SECOND line (substates complete deepest-first, after the
// outer brain's onExit, before any component onExit).
import {StateScript} from 'midday'

export default class Armed extends StateScript {
  onEnter(from: string) {
    void from // fires at tick 0, AFTER the state components seated (D2)
  }

  onExit(to: string) {
    void to // line 2 of the golden's 7-line exit chain
  }
}
