# Headless rig scripts

Scripts for the headless test rig (the `test*` console commands in
`openttd/src/console_cmds.cpp`). They drive a `-vnull` build of the game
through scripted scenes and count what the trace lines say.

Working directory layout, pointed to by `RIG_DIR`:

- `build/openttd` — a build of this tree,
- `ttdhome/`, `h2/`, `h3/` — three `HOME` directories (each with
  `.openttd/scripts/`, an `openttd.cfg` with `vlak123` tracing on, base
  graphics), so three scenes can run at once,
- the saves from `saves/` copied in, under the names the battery calls them
  by: `eka.sav`, `emu.sav`, `emu_reverz.sav` as `emu2.sav`,
  `loko_obou_stran.sav` as `loko2.sav`, `new1.sav`, `obmena.sav`,
  `obmenaporucha.sav`, `odtah_peron.sav` as `back2.sav`, `porucha.sav`,
  `porucha_nastupiste.sav` as `porucha2.sav`, `porucha_za_vlakem.sav` as
  `porucha3.sav`, `rig.sav`, `vlak31.sav`.

**Copy them before the first run.** A scene whose save is not there does not
fail: the game says "Game load failed", the scene ends after six lines, and
every counter comes out zero. Zero equals zero, so the stable file matches
the last one and the battery reports the run as clean while a third of it
never happened. Anything the battery says about a build is only worth as much
as the number of scenes that actually loaded -- if a run looks suspiciously
unchanged, count `Game load failed` across `reg_*.log` first.

A save made with NewGRFs needs those sets in the rig's `HOME` as well
(`<home>/.openttd/newgrf/`, the `.tar` files as the game downloaded them),
or the game disables them, replaces every vehicle with a default one of
another length and shape, and the save is a different game. The player's
`sivy2.sav` (nose-first coupling of a steam engine with its tender) needs
CZTR Rails 2.2.4, CZTR Engines Steam 1.0.2 and CZTR Wagons Cargo 1.1.0.

The rig has **only those three** sets; the player plays with about a dozen
(road set, diesel, electric and EMU engines, passenger wagons, stations,
rail add-ons). A save of his therefore loads here with most of its sets
disabled, and the log says so: `NewGRF ... not found`. Count those lines
before concluding anything about a save of his, and do not read "it works
here" as "it works for him" when the question is about what a set does.

## Memory errors: the rig does not see them

The battery measures behaviour, not memory. A write past the end of an
array can leave every scene green on one compiler and break the game on
another, which is exactly what happened with the cargo for road vehicles
(TEMATA8 §37). When a change touches tables, arrays or indices, build with
the sanitizers and run a scene or two through that build:

    cmake <srcdir> -DCMAKE_BUILD_TYPE=Debug -DOPTION_DEDICATED=ON \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"

in a build directory of its own (it is slow and the binary is huge, so not
the one the battery uses), then run it with `ASAN_OPTIONS=detect_leaks=0`.
Generating `openttd.grf` needs `media/baseset/openttd/sprites/` from a
working build directory copied in, and the baseset graphics beside the
binary, or the build stops at GRFCodec.

Four saves the battery asks for are the player's and are not in `saves/`:
`s.sav` (scene `nakladsav`), `save91.sav` (`save91`, `save91rev`),
`umak.sav` (`mess`) and `umins.sav` (`messodvoz`, `messodtah`). Those six
scenes cannot run without them.

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
