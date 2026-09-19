# Headless rig scripts

Scripts for the headless test rig (the `test*` console commands in
`openttd/src/console_cmds.cpp`). They drive a `-vnull` build of the game
through scripted scenes and count what the trace lines say.

Working directory layout, pointed to by `RIG_DIR`:

- `build/openttd` — a build of this tree, and beside it
  `build/openttdDecouple.cfg`, the config this build writes (it keeps its
  config next to its own binary and under a name of its own, so that a
  player's vanilla config is left alone — see `DeterminePaths()`). The settings the scenes are written against, `vlak123`
  tracing among them, live there; the battery puts its kept copy back after
  every scene,
- `ttdhome/`, `h2/`, `h3/` — three `HOME` directories (each with
  `.openttd/scripts/`, base graphics), so three scenes can run at once. A
  home's own `openttd.cfg`, if it has one, is no longer read or written by a
  run,
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

## A steam engine with its tender, which needs a set that has one

The game's own engines are all one vehicle, so nothing in a plain rig run
ever built an engine that comes with a tender. Such an engine used to be one
vehicle with one articulated part, and that shape was the one coupling the
game refused outright: its list cannot be turned round, so meeting a rake
nose first it would have ended at the tender while the engine is what stands
against the wagons. Now the pair is made into a two-headed engine at build
time (MakeTenderRearHead() in train_cmd.cpp): the tender keeps its own
picture and contributes nothing, and the list turns round like any other.
`testspoj tendr` is where that is measured. It needs the same home as the
lorry scene, or any home with a set of steam engines in it:

    <home>/.openttd/newgrf/   4d490207-cztr_engines_steam-1.0.2.tar
                              4d490213-cztr_wagons_cargo-1.1.0.tar
    openttd.cfg               landscape = temperate, starting_year = 1930

Without such a set the scene says so and stops, which is why it is not in the
battery: on the game's own engines there is nothing to build it with.

Two runs to make:

    testspoj tendr            the collector meets the rake nose first
    testspoj tendr couvej     it backs onto the rake instead

Both couple clean and both trains reach their depot: spojeno=1, no broken
step, nothing in the record, depa=3. Nose first, the joined train runs
tender first to the far depot, the way a steam engine backs a train. Before
the pair, the first run broke seventeen steps, the same seventeen every
time, and that number is what the refusal had been holding back.

`testtvar <unit>` shows the pair: `par masinka s N` on the engine and
`par tendr s N` on the tender, N being the other's index. A tender with
`BEZ PARTNERA` is a pair that came apart on load, which is the one way this
can quietly fail; save the scene (`save x`) and load it back to check.

The refusal itself stays only for an engine built of *unequal* articulated
parts -- deliberately not made into a pair, since its parts would have to
trade places on the ground and for such an engine that shows. No such engine
is in the rig's sets; the refusal is read, not measured.

## An engine of equal pieces, and a wagon of pieces drawn flipped

The player's sets draw every engine as three pieces -- an invisible stub, the
body, an invisible stub -- and every wagon likewise. Two things follow from
that shape, and the rig measures both.

**The engine couples at its nose.** A unit whose pieces are the same length two
by two from the ends is the same shape read from either end, so its pieces can
trade places on the ground with nothing visibly moving (`MirrorUnitPieces()`),
and the list turns round like any other (`ConsistCanBeRelinked()`). The rig's
own sets have no such engine, so `testspoj clanky` makes one: the collector's
engine gets two articulated pieces of its own kind behind it in the shed
(`MakeEngineOfPieces()`, console `testclanky <unit> [pieces]`). They are full
length, since only a set can say a piece is short, so every piece shows the
engine's own picture -- the list and the ground can be measured on it, the
picture cannot.

    testspoj clanky           three-piece engine meets the rake nose first
    testspoj clanky couvej    the same engine backs onto it

Both: spojeno=1, no broken step, nothing in the record, depa=3. Before the
rule the first was the refusal, and the player's log shows what the refusal
did to his shunter: it drove into a shed to turn, came back tail first, and
coupled a lap late.

**The wagon keeps its picture.** A rake met head on has every wagon's
direction reversed and Flipped set, which for a one-piece wagon leaves the
picture as it was. A wagon of several pieces was drawn wrong: each piece
painted its own cut of the picture mirrored, in place, after the pieces had
traded places -- the front of the wagon at its back. Now a flipped piece
draws its mirror piece (`PieceDrawnAs()`), and every coupling checks the
promise that nothing on the screen changes: `PictureKeptAfterJoin()` reads
the sprites at every spot before and after and writes `SPOJENI PREKRESLILO`
into the record for any spot that looks different. That check needs wagons of
several pieces, so it is measured in the lorry-scene home (h_tir), whose
wagon set draws them:

    testspoj kloub            three such wagons, met head on: every wagon flips
    testspoj kloub jeden      one such wagon: a single unit turned round inside itself
    testspoj kloub couvej     backing on: nothing flips

All three: spojeno=1, record empty. With `testzrcadlo off` (the old drawing)
the first writes six `SPOJENI PREKRESLILO` lines, one per end piece of each
wagon; `testzrcadlo on` writes none. `testkresba <unit>|<x> <y>|vse` prints
every piece's sprites, box, facing, flip, which piece it draws as, and what it
carries -- the whole of what a picture is made of, since the rig cannot look
at one; `testvse` includes it.

`testnatoceni` reports a piece only when it faces a right angle or more away
from its head. A piece in a bend stands 45 degrees off and is right to; the
player's log had eight of those reported as faults.

A lorry riding on a wagon is part of that wagon's picture and keeps the same
promise, so `testkresba` prints it too: its facing, the facing the wagon under
it is drawn with, and `sedi` or `NESEDI` between them. It went wrong in
exactly the place the wagons did -- a flipped wagon laid its lorry out from
the recorded direction, so the lorry spun round on the spot and moved to the
wagon's other end while the wagon itself did not move at all. On the player's
save that was 24 `NESEDI` against 0 after the fix.

`pozn <text>` writes a line of the player's own into the record, with the tick
on it, for saying what was on the screen at that moment.

`testpaluba [pixels]` moves the deck a carried lorry stands on, while the game
runs, and redraws every one of them at once. How high a wagon's deck is is
written nowhere -- no wagon says, and every set draws its own at its own
height -- so that number is chosen by eye at the screen and then written into
the source. Where along the wagon the lorry stands is not chosen by eye: its
chain's middle goes on the wagon's middle, which needs no number and comes out
right for a wagon of any length.

## A wagon holding an order list, and a window left on the wrong vehicle

Two faults the rig cannot see by counting, because both are a state that
hurts only when something asks about it: a vehicle that is not the head of
any train yet carries an order list (a rake head that was collected and never
gave its list up), and a window left open on a vehicle that has since stopped
being a head (a train turned round in the list keeps its identity on the
other head). Cargo distribution asks the first with `IsStoppedInDepot()` on
its stale-link sweep, the player's screen asks the second on the next
refresh, and both are an assert, `this == this->First()`.

`testokna` asks both questions itself: it lists every open vehicle window
and every order list's first shared vehicle, says which are not heads, writes
a record line for each, and then refreshes every vehicle window the way a
livery change does. `testspoj ... okno` opens the collector's window before it
sets off, so there is a window to leave behind. The battery runs `testokna`
on `zakl` and `vlek`; its record counter is what catches a regression.

Found on the player's game with a steam engine coupling nose first and cargo
distribution on. The rig ran that coupling clean, because the rig plays with
distribution off and has no screen; the probe was written to make the rig
ask what the player's game asked.

## Which half of "load onto wagons" a scene measures

Boarding a train used to be two choices, by train or onto wagons, and it is
now five: by train to my next stop, by train or shunter wherever it goes, on
wagons meant for my next stop, on any wagons, and not at all. `testautovlak`
takes the same two words it always did, and they now name two of the five:
`posun` is by train or shunter wherever it goes, `vlakem` is by train to my
next stop. `posun` is not the wagon choice, although it reads like one --
the scene builds a shunter, an engine with a wagon, and a shunter is a
train. The wagon choices want a rake standing with no engine, which this
scene does not build.

That is worth knowing before reading `autoposun` as a regression: when one
choice is split into two, what the scene measured is split with it, and the
scene has to be told which half it still measures. The battery caught this
as `autoposun: auto=1` turning into `auto=0`, and nothing in the game was
wrong.
