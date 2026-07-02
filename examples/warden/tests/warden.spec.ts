// Game-level behavioral test — the testing pillar exercising the whole design:
// deterministic ticks, synthetic input, state assertions, journal assertions, golden frame.
import {test, expect, sim, events} from 'midday/testkit'
import {Health} from '../components/health'

test('warden: perception → chase → attack; death mid-swing is clean', async () => {
  const run = await sim.load('examples/warden/scenes/arena.scene.yaml', {seed: 42})
  const warden = run.entity('Warden')

  // Drive the player toward the warden with synthetic input.
  run.input.hold('move', [1, 0, 0])
  await run.tickUntil(() => run.inState(warden, 'locomotion', 'Chase'), {max: 900})

  // He attacks when in range; the hitbox must be live only inside its span.
  await run.tickUntil(() => run.inState(warden, 'combat', 'SlashAttack'), {max: 900})
  expect(run.isActive(warden, 'combat/SlashAttack/HitboxLive')).toBe(false)  // t < 0.4s
  await run.tick(30)                                                          // ≈0.5s into sequence
  expect(run.isActive(warden, 'combat/SlashAttack/HitboxLive')).toBe(true)

  // Kill him mid-swing with a simultaneous stagger — the Appendix A golden scenario.
  run.at(() => {
    run.component(warden, Health).damage(999, run.entity('Player'))
    events.trigger('stagger.hit', {force: 12}, {key: warden})
  })
  await run.tick(1)

  expect(run.stateOf(warden, 'combat')).toBe('Dead')
  expect(run.isActive(warden, 'combat/SlashAttack/HitboxLive')).toBe(false)   // no zombie hitbox

  const t = run.journal.lastTick()
  expect(t.transitions.filter(x => x.region === 'combat')).toHaveLength(1)    // one transition/region/tick
  expect(t.voided).toContainEqual(expect.objectContaining({event: 'stagger.hit'}))
  expect(t.causeChain('boss.died')).toEqual(
    expect.arrayContaining(['damage.dealt', 'death.dealt']))                  // causality intact

  await expect(run.shot('MainCam')).toMatchGolden('goldens/warden_dead.png', {tolerance: 0.02})
})
