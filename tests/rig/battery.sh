#!/bin/bash
S=${RIG_DIR:?set RIG_DIR to the rig working directory (build/, ttdhome/, h2/, h3/, *.sav)}
H=$S/ttdhome
run_scene() { # name scr-content ticks extra-args
  local name=$1 scr=$2 ticks=$3; shift 3
  # The rig runs the breakdown length long (a quarter of a year, the value the
  # scenes were written against); the player's default is a fortnight.
  printf 'setting vehicle.rescue_wait_days 90\n%s\n' "$scr" > $H/.openttd/scripts/game_start.scr
  # A scene played from a save must not have autoexec start a new game over it;
  # that silently ran every save scene on a fresh map for weeks.
  case "$*" in *-g*) : > $H/.openttd/scripts/autoexec.scr ;; *) printf 'newgame\n' > $H/.openttd/scripts/autoexec.scr ;; esac
  HOME=$H timeout 300 $S/build/openttd -vnull:ticks=$ticks -snull -mnull "$@" > $S/reg_$name.log 2>&1
  local spoj=$(grep -c 'spojeno' $S/reg_$name.log)
  local hav=$(grep -c 'HAVAROVAL' $S/reg_$name.log)
  local srz=$(grep -c 'Srazka' $S/reg_$name.log)
  local ast=$(grep -ci 'assert' $S/reg_$name.log)
  local odt=$(grep -c 'odtah dokoncen' $S/reg_$name.log)
  local exc=$(grep -ci 'terminate\|exception' $S/reg_$name.log)
  local dep=$(grep -c 'vjel do depa' $S/reg_$name.log)
  echo "$name: spojeno=$spoj odtazeno=$odt havaroval=$hav srazka=$srz assert=$ast vyjimka=$exc depa=$dep"
}
printf 'newgame\n' > $H/.openttd/scripts/autoexec.scr
run_scene zakl "vlak123 on
testspoj" 8000
run_scene couvej "vlak123 on
testspoj couvej" 8000
run_scene depo "vlak123 on
testspoj depo" 8000
run_scene depopocet "vlak123 on
testspoj depo pocet" 8000
run_scene depostoji "vlak123 on
testspoj depo stoji" 8000
run_scene depozrus "vlak123 on
testspoj depo
testza 400 testzrus" 8000
run_scene depovagony "vlak123 on
testspoj depo
testza 400 testzrus
testza 1200 testvagony 3" 12000
run_scene depooboji "vlak123 on
testspoj depo oboji" 12000
run_scene sklad2 "vlak123 on
testspoj depo sklad 2" 12000
run_scene sklad6 "vlak123 on
testspoj depo sklad 6" 12000
run_scene sklad20 "vlak123 on
testspoj depo sklad 20" 12000
run_scene skladdve "vlak123 on
testspoj depo sklad 3" 13000
run_scene koupitpri "vlak123 on
testspoj depo sklad 2
testza 300 testvagony 3" 12000
run_scene filtrspatny "vlak123 on
testspoj depo pocet
testza 200 testfiltr 6" 12000
run_scene filtropraveny "vlak123 on
testspoj depo pocet
testza 200 testfiltr 6
testza 6000 testfiltr" 12000
run_scene rad "vlak123 on
testspoj rad" 10000
run_scene blok "vlak123 on
testspoj blok" 8000
run_scene vlek "vlak123 on
testspoj vlek" 8000
run_scene vlekblok "vlak123 on
testspoj vlek blok" 8000
run_scene odtahrovina "vlak123 on
testodtah rovina" 14000
run_scene odtahkrizeni "vlak123 on
testodtah krizeni" 14000
run_scene odtahjednosmer "vlak123 on
testodtah jednosmer 0" 14000
run_scene odtahdaleko "vlak123 on
testodtah daleko 0" 16000
run_scene odtahbezdepa "vlak123 on
testodtah daleko 0
testza 1000 testdepo pryc
testza 9000 testdepo zpet" 22000
run_scene odtahvagony "vlak123 on
testodtah vagony
testza 5000 testokno 0
testza 6000 testodvoz vse" 20000
run_scene okruh "vlak123 on
testokruh" 16000
run_scene naklad "vlak123 on
testnaklad
testza 4000 testbrzda 1" 12000
run_scene nakladcekat "vlak123 on
testnaklad cekat
testza 6000 testbrzda 2" 12000
run_scene nakladsmer "vlak123 on
testza 3000 testokno smer 0
testza 3000 testokno smer 1
testza 3000 testokno 1
testnaklad smerovani
testza 2500 testbrzda 2" 20000
printf '' > $H/.openttd/scripts/autoexec.scr
run_scene nakladsav "vlak123 on
unpause
testza 50 testbrzda 1" 6000 -g $S/s.sav
run_scene emu "testpauza
vlak123 on
testza 10 testbrzda 1
testza 10 testbrzda 2
testza 10 testbrzda 3
testza 10 testbrzda 4
testza 10 testbrzda 5
testza 10 testbrzda 6
testza 10 testbrzda 7
testza 10 testbrzda 8" 20000 -g $S/emu.sav
run_scene emujz "testpauza
vlak123 on
testza 10 testbrzda 9
testza 10 testbrzda 10
testza 10 testbrzda 11
testza 10 testbrzda 12
testza 10 testbrzda 13
testza 10 testbrzda 14
testza 10 testbrzda 15
testza 10 testbrzda 16" 20000 -g $S/emu2.sav
run_scene save91 "testpauza
vlak123 on
testza 10 testbrzda 1
testza 10 testbrzda 2
testza 10 testbrzda 3
testza 10 testbrzda 4
testza 10 testbrzda 5
testza 10 testbrzda 6
testza 10 testbrzda 7
testza 10 testbrzda 8
testza 10 testbrzda 9
testza 10 testbrzda 10
testza 10 testbrzda 11
testza 10 testbrzda 12" 12000 -g $S/save91.sav
run_scene save91rev "testpauza
vlak123 on
testza 10 testbrzda 1
testza 10 testbrzda 2
testza 10 testbrzda 3
testza 10 testbrzda 4
testza 10 testbrzda 5
testza 10 testbrzda 6
testza 10 testbrzda 7
testza 10 testbrzda 8
testza 10 testbrzda 9
testza 10 testbrzda 10
testza 10 testbrzda 11
testza 10 testbrzda 12
testotoc" 12000 -g $S/save91.sav
# Player's save: a collector with a wagon behind it comes in nose first onto a
# waiting train -- the joined chain has no engine at its head. It must wait to
# be collected (order at the station), not stand there with nothing.
run_scene mess "testpauza
vlak123 on
testbrzda 17
testbrzda 18
testbrzda 19
testbrzda 20
testza 3 testklon 24 3
testza 28 testvozy vse" 30000 -g $S/umak.sav
# The same save a step later, four such chains standing at the platforms: an
# ordinary engine with a couple order fetches one and puts it down in a depot.
run_scene messodvoz "testpauza
vlak123 on
testrada vse
testbrzda 28
testza 14 testvozy vse" 15000 -g $S/umins.sav
# ... and the tow, called from the rake's window, takes one to the depot.
run_scene messodtah "testpauza
vlak123 on
testrada vse
testodtahovka 28
testbrzda 28
testza 2 testodvoz vse
testza 16 testvozy vse" 17000 -g $S/umins.sav
# Player's save: the tow is held in its shed by a casualty it cannot book a
# road to; wagons called for opposite the door must still be fetched.
run_scene ekaodtah "vlak123 on
testpauza
testodvoz 120 78
testza 5000 testodvoz 120 79
testza 14000 testvozy vse" 15000 -g $S/eka.sav
# Player's save: a train sent through a red into one entering a depot must
# crash (two wrecks), and the tow must not become the third.
run_scene vlak31 "vlak123 on
testpauza" 12000 -g $S/vlak31.sav
# Player's save: a train broken down half inside a depot is pushed in by the
# tow, put down and serviced there, and leaves on its own orders.
run_scene protlacit "vlak123 on
testpauza
testprojet 31
testporucha 32
testbrzda 32" 12000 -g $S/vlak31.sav
# Player's save: a breakdown bent across the points at a depot door; the tow
# must not creep over it (that brought the game down), it stays home. A second
# breakdown on the curve at (98,60) is then straightened and towed in; that
# used to tear the casualty (close-up ran out of steps, TEMATA 4.25) and is
# expected snug now: odtazeno 1, no assert.
run_scene poruchavrata "vlak123 on
testpauza
testbrzda 3
testbrzda 4" 15000 -g $S/porucha.sav
# Player's save: a breakdown standing on a platform; the tow's destination is
# a tile in the middle of a platform, which the search steps over in one go.
run_scene poruchanastup "vlak123 on
testpauza" 12000 -g $S/porucha2.sav
# Player's save: a casualty on a platform with a second engine stopped behind
# it; the tow has to book the whole road round, in through the one-way signals
# from the front. Then the repaired train must not run into the stopped one.
run_scene poruchazavlakem "vlak123 on
testpauza" 12000 -g $S/porucha3.sav
# Player's save: a casualty standing in the middle of a platform, a parallel
# platform of the same station beside it. The tow must stop on the casualty's
# own platform right before it, not on the platform next door, and must not
# write the station visit down as an order.
run_scene odtahperon "vlak123 on
testpauza
testodtahovka 36
testbrzda 36" 12000 -g $S/back2.sav
# Player's save: locomotive-both-ends trains, four waiting, one collector
# cloned three times. Default order: keep nought -- the collector leaves alone,
# a headless rake with locomotives inside stays (the player's choice).
run_scene lokonula "testpauza
vlak123 on
testza 10 testbrzda 17
testza 10 testbrzda 18
testza 10 testbrzda 19
testza 10 testbrzda 20
testza 8000 testbrzda 21
testza 8000 testklon 21 3" 30000 -g $S/loko2.sav
# Same, with the decouple order switched to "drop the whole coupled train":
# every collector keeps its three, every waiter wakes with its three.
run_scene lokocely "testpauza
vlak123 on
testcelyvlak 21 2
testza 10 testbrzda 17
testza 10 testbrzda 18
testza 10 testbrzda 19
testza 10 testbrzda 20
testza 8000 testbrzda 21
testza 8000 testklon 21 3" 30000 -g $S/loko2.sav
# Founding a rake: the feeder fetches pairs from the west shed and pushes them
# onto the rake at the platform (2, 4 -- the four-tile platform has no room for
# the feeder plus a tile after that), then waits with "rake full"; the collector,
# released at 30000, takes the four; the feeder founds the next rake.
run_scene zaloz "vlak123 on
testspoj zaloz 6
testza 30000 testbrzda 3" 40000
# Player's save: founding a rake behind a station waypoint while standing at
# a plain one; the feeder must go through "peron3" and found on the platform
# behind it, not on the loading platforms, and must not roll off the stub
# unbooked into the junction (TEMATA 2.38, 4.22). Two couplings, no crash.
run_scene zalozsmer "testpauza
vlak123 on" 12000 -g $S/new1.sav
# Player's save: the tow fetches a breakdown, is turned round on the way home
# (a tick-timed testotoc, after the coupling) and backs into the depot wagons
# first; autoreplace at the depot must leave the joined train alone and the
# casualty must be put down and replaced on its own (TEMATA 4.24). No assert.
run_scene odtahotoc "testpauza
vlak123 on
testza 10 testbrzda 5
testzatik 2450 testotoc 5" 6000 -g $S/obmenaporucha.sav
# Player's save, the tow let off the brake at once (the casualty gives up
# waiting ten days in): the casualty stands with its tail on the points at the
# platform throat, the tow straightens it across the platform, couples snug,
# is turned back by the red signal and pushes it into the depot (139,167).
# Coupled, delivered, no assert (TEMATA 4.25).
run_scene odtahvyhybka "testpauza
vlak123 on
testzatik 10 testbrzda 5" 6000 -g $S/obmenaporucha.sav
# A train torn open on purpose (14 px behind its leading vehicle, the rig's
# testmezera) drives into a depot: the follower steps onto the depot tile
# after the vehicle ahead is already hidden inside, which used to be the
# "krok ROZBITY" assert in the doorway. Expected: it goes in, the hole
# closes inside, it comes out snug. depa 1, no assert (TEMATA 4.27).
run_scene mezera "setting difficulty.vehicle_breakdowns 0
vlak123 on
testpauza
testzatik 10 testskip 1
testzatik 300 testmezera 1 6
testzatik 1500 testbrzda 1" 4000 -g $S/obmena.sav
# Automatic departure, single-engine half (TEMATA 2.41): on obmena.sav the
# wait flags are cleared and trains 1 (came in pushing) and 3 (pulling) get
# "automatic" on their station order, 2 gets reverse-out and 4 nothing, then
# all four are skipped out. 1 turns so its engine leads, 3 does not, 2 and 4
# behave as before; the toggles are also shown to exclude each other. All
# four reach a depot: depa 5 (train 1 laps once), no assert, no collision.
run_scene auto "setting difficulty.vehicle_breakdowns 0
vlak123 on
testpauza
testmof 1 1 11 0
testmof 2 1 11 0
testmof 3 0 11 0
testmof 4 0 11 0
testauto 1 1
testmof 1 1 14 1
testrozkazy
testauto 1 1
testrozkazy
testauto 3 0
testmof 2 1 14 1
testza 10 teststav
testzatik 50 testskip 1
testzatik 50 testskip 2
testzatik 50 testskip 3
testzatik 50 testskip 4" 3000 -g $S/obmena.sav
# Player's polygon (rig.sav), depot C: station 1 then back to the depot; the
# way on is a long loop through station 2, the way back is straight. Released
# with spacing (six at once lock up in the depot junction). Dual-headed
# units: 67 (automatic) turns because back is shorter, 66 (reverse out)
# turns, 65 (no flag) carries on round the loop. Three home, no assert.
run_scene rigC2h "setting difficulty.vehicle_breakdowns 0
vlak123 on
testpauza
testza 10 teststav
testzatik 10 testbrzda 67
testzatik 1200 testbrzda 66
testzatik 2400 testbrzda 65" 9000 -g $S/rig.sav
# Same with light engines 76/75/74: a lone engine can lead from both ends,
# so 76 (automatic) also takes the shorter way back.
run_scene rigC1m "setting difficulty.vehicle_breakdowns 0
vlak123 on
testpauza
testza 10 teststav
testzatik 10 testbrzda 76
testzatik 1200 testbrzda 75
testzatik 2400 testbrzda 74" 9000 -g $S/rig.sav
# The player's known tests on the polygon, one depot each: release the four
# deliverers/waiters, then let one collector out and clone it three times.
run_scene rigD0 "vlak123 on
testpauza
testza 10 testbrzda 1
testza 10 testbrzda 2
testza 10 testbrzda 3
testza 10 testbrzda 4
testza 8000 testbrzda 5
testza 8000 testklon 5 3" 30000 -g $S/rig.sav
run_scene rigD3 "vlak123 on
testpauza
testza 10 testbrzda 9
testza 10 testbrzda 10
testza 10 testbrzda 11
testza 10 testbrzda 12
testza 8000 testbrzda 13
testza 8000 testklon 13 3" 30000 -g $S/rig.sav
run_scene rigD1 "vlak123 on
testpauza
testza 10 testbrzda 17
testza 10 testbrzda 18
testza 10 testbrzda 19
testza 10 testbrzda 20
testza 8000 testbrzda 21
testza 8000 testklon 21 3" 30000 -g $S/rig.sav
run_scene rigD2 "vlak123 on
testpauza
testza 10 testbrzda 25
testza 10 testbrzda 26
testza 10 testbrzda 27
testza 10 testbrzda 28
testza 8000 testbrzda 29
testza 8000 testklon 29 3" 30000 -g $S/rig.sav
run_scene rigD5 "vlak123 on
testpauza
testza 10 testbrzda 33
testza 10 testbrzda 34
testza 10 testbrzda 35
testza 10 testbrzda 36
testza 8000 testbrzda 37
testza 8000 testklon 37 3" 30000 -g $S/rig.sav
run_scene rigD4 "vlak123 on
testpauza
testza 10 testbrzda 57
testza 10 testbrzda 58
testza 10 testbrzda 59
testza 10 testbrzda 60
testza 8000 testbrzda 61
testza 8000 testklon 61 3" 30000 -g $S/rig.sav
# Automatic departure on a couple order (polygon, TEMATA 2.41): a dual-headed
# waiter is collected by a dual-headed collector whose next stop is back the
# way it came. 62 with no flag carries on round the loop; 63 with "automatic"
# asks the pathfinder at the coupling's conclusion, hears back is shorter,
# turns. Both drop the waiter at station 3 and both go home: depa 2.
run_scene cplnic "setting difficulty.vehicle_breakdowns 0
vlak123 on
testpauza
testmof 62 1 14 0
testzatik 10 testbrzda 58
testzatik 1500 testbrzda 62" 8000 -g $S/rig.sav
run_scene cplauto "setting difficulty.vehicle_breakdowns 0
vlak123 on
testpauza
testmof 63 0 14 0
testauto 63 0
testzatik 10 testbrzda 59
testzatik 1500 testbrzda 63" 8000 -g $S/rig.sav
# Same with a single engine collecting an engine-plus-wagons waiter: the end
# the partner hung off cannot lead, so "automatic" turns the train engine
# first, as reversing out would.
run_scene cplauto1 "setting difficulty.vehicle_breakdowns 0
vlak123 on
testpauza
testmof 21 1 14 0
testauto 21 1
testzatik 10 testbrzda 17
testzatik 1500 testbrzda 21" 8000 -g $S/rig.sav
