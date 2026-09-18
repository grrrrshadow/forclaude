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

Since the map is fixed (below), one run is enough and there is something to
diff it against: `battery_baseline.stable` here is a full run and the file to
compare a change to.

    RIG_DIR=... BATTERY_STABLE=run.stable ./battery.sh > run.out
    diff battery_baseline.stable run.stable

Six of its scenes need saves that are not in the repository and come out all
zeroes without them, so on a rig missing those the difference is those six
lines and nothing else. Take the baseline again when a change is meant to
move a counter, and say in the commit which counters moved and why.

Two things that look like fixes for the wobble and are not, both tried:
turning random breakdowns off steadies it but guts three scenes built on a
breakdown happening (`odtahotoc` drops to nothing at all), and turning
automatic servicing off changes nothing -- the wobble is in the tow scenes
themselves.

## The map every new-game scene is built on

A scene that does not load a save builds its own track, stations and road
stops on the land the generator gave it, and on hilly or watery land some of
that cannot be built at all: the scene ends early, every counter reads zero,
and the run looks like a change in the game. `battery.sh` therefore fixes the
map for those scenes -- one seed, the flattest land and the lowest sea level
the generator offers (`NEWGAME` at the top of the script). Three runs of the
same scene then come out identical line for line, and two whole runs of the
battery give the same stable file.

It is the flat land that does the work, not the seed: every seed tried builds
every scene once the land is flat. Changing any of the three settings makes a
different map, so the stable file has to be taken again -- that is a
re-baselining, not a regression. Scenes that load a save are untouched by
this; their map is in the save.

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

## Two tiny NewGRFs that fight over our cargo slot

`grf/slot63_blank.nfo` and `grf/slot63_taken.nfo` are complete NewGRFs of two
or three pseudo-sprites each. One blanks cargo slot 63 and the other puts a
cargo of its own there -- the slot the cargo for road vehicles on wagons is
put in (road_on_rail.h). They exist because the player's set does one of those
two things and nothing here reproduced it: with them, it reproduces in one
run.

    cd <dir with sprites/slot63_taken.nfo>
    grfcodec -e -p1 slot63_taken.grf
    cp slot63_taken.grf <rig home>/.local/share/openttd/newgrf/

then add a line `slot63_taken.grf =` under `[newgrf]` in that home's
`openttd.cfg`, start a game and read the last line of `dump_info cargotypes`.
Blanked, the cargo takes the slot back; taken, it moves down to 62. Either
way it must still be in the cargo mask and in every wagon's refit mask.

Worth keeping because the first of them found a crash: a cargo moved to
another slot after the NewGRFs had spoken never got the once-over that gives
every cargo a town production effect, and the next thing that sorted cargoes
walked into an assertion.

## A lorry with a trailer, which needs the player's own sets

The game's own road vehicles are all one piece, so nothing in a plain rig
run says what a lorry and trailer does on a wagon -- and that is the case
the wagon lengths are about (road_on_rail.h). `testautovlak 1 tirak` builds
the scene with one, and it needs a home set up with three of the player's
NewGRFs and a late enough year:

    <home>/.openttd/newgrf/   CZTR_Truck_SetBRYLE1-rozestupy-cisty.grf
                              4d490213-cztr_wagons_cargo-1.1.0.tar
                              4d490207-cztr_engines_steam-1.0.2.tar
    openttd.cfg               landscape = temperate, starting_year = 2030

Each of the three is needed for its own reason. The truck set as it is
published has no vehicle a company can buy -- the player's own build of it
does, and that is the one to use. The wagon set brings the long wagons: its
freight wagons are built of several pieces and run from 14 to 20 eighths,
where the game's own are 8, and a lorry and trailer measures 15. And the
steam engines are there only so that plain rail exists at all: a railtype is
available to a company once an engine of it has been introduced, and with the
wagon set loaded and no engine set, the scene cannot build so much as a shed
("depot failed"). The year has to be late enough for the long lorries and
early enough for a long wagon to still be in production; 2030 is both.

The scene then picks the longest wagon the game has -- the one built of the
most pieces -- rather than the car carrier, because nothing shorter than the
lorry can carry it. Four rides in nine thousand ticks, and the trailer is put
down on the road behind its lorry and drives on at its own length behind it.
