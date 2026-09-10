# Headless rig scripts

Scripts for the headless test rig (the `test*` console commands in
`openttd/src/console_cmds.cpp`). They drive a `-vnull` build of the game
through scripted scenes and count what the trace lines say.

Working directory layout, pointed to by `RIG_DIR`:

- `build/openttd` — a build of this tree,
- `ttdhome/`, `h2/`, `h3/` — three `HOME` directories (each with
  `.openttd/scripts/`, an `openttd.cfg` with `vlak123` tracing on, base
  graphics), so three scenes can run at once,
- the saves from `saves/` copied in (`rig.sav`, `vlak31.sav`, ...).

`battery.sh` — the regression battery, 64 scenes. Prints one line per scene:
`spojeno` (couplings), `odtazeno` (tows completed), `havaroval`, `srazka`,
`assert`, `vyjimka`, `depa` (arrivals in a depot).

The first six do not move between runs of the same build. **`depa` does** --
by one, on the long tow scenes -- so a plain diff of two runs reports
differences that are the rig's own timing and not the change under test. Set
`BATTERY_STABLE` to a path and the six load-bearing counters are written
there as well; diff those:

    RIG_DIR=... BATTERY_STABLE=run1.stable ./battery.sh > run1.out
    RIG_DIR=... BATTERY_STABLE=run2.stable ./battery.sh > run2.out
    diff run1.stable run2.stable

Two things that look like fixes for the wobble and are not, both tried:
turning random breakdowns off steadies it but guts three scenes built on a
breakdown happening (`odtahotoc` drops to nothing at all), and turning
automatic servicing off changes nothing -- the wobble is in the tow scenes
themselves.

`matrix_gen.py [mx] [mw] [me]` — the coupling matrix on `rig.sav`: four
waiters released slowly from one side of station 1, then a collector and
three clones (`testklon ... stoj`, released one by one with `testbrzda`)
from the left or from the right; four spacing variants per combination.
Families: `mx` waiting trains (17–20 from the right, 25–28 from the left;
collectors 5 / 14), `mw` rakes dropped by deliverers (9–12 / 1–4), `me`
dual-headed units (57–60 / 33–36; collectors 61 / 37), `ms` those same
units collected by a plain engine (5 / 14), `mv` dropped rakes collected
by a dual-headed unit (37 / 61). Writes
`mx_ttdhome.sh`, `mx_h2.sh`, `mx_h3.sh` into `RIG_DIR`; run them in
parallel. `matrix_baseline_ce1effd.out` is the result on the tree before
the crossed-coupling fix: same side 4/4 everywhere, crossed waiting trains
and crossed units 1 coupling + 3 collisions in every scene, crossed rakes
4/4. `matrix_after_fix.out` is the same matrix after the fixes (partner
chosen before the road is planned; order progress kept when a unit's
identity moves to its other head; the dual-head list normalisation left
to sheds): all 80 scenes 4/4, no collision and no assert, and the units
put down crossed go on to their next stop and home.

Scenes are to be re-run once the engine–wagons–engine depots (A/B) are
worked on again.
