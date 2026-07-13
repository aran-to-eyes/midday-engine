// examples/lifecycle/states/parent.ts — the Alive state's brain (M2 node
// 0B golden, FUSED-SPEC D6). Deliberately hook-only: the golden pins the
// A.2.1 exit chain by JOURNAL ORDER (statechart.hook invocation records),
// so both hooks must exist — exit:Alive is the chain's FIRST line (the
// brain exits while its parts are still live).
import {StateScript} from 'midday'

export default class Parent extends StateScript {
  onEnter(from: string) {
    void from // seated before initial entry (D2 split) — fires at tick 0
  }

  onExit(to: string) {
    void to // line 1 of the golden's 7-line exit chain
  }
}
