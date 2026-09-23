#!/bin/bash
S=${RIG_DIR:?set RIG_DIR to the rig working directory (build/, ttdhome/, h2/, h3/, *.sav)}
H=$S/ttdhome

# One map for every scene that starts a new game, instead of a fresh random one
# each time. A scene builds its own track, stations and road stops on whatever
# land it is given, and on a hilly or watery map some of that simply cannot be
# built: the scene then ends early, every counter comes out zero, and the run
# reads as a change in the game when it is a change in the map. That was the
# standing wobble in nakladcekat, nakladsmer, zaloz and the two car scenes, and
# it cost a reading of the battery every few runs.
#
# A fixed seed alone is not enough -- it is the flat land that makes the scenes
# buildable, and the flattest, least watery setting the generator has. With
# both, three runs of the same scene come out identical line for line.
#
# Everything the generator reads belongs in here, and most of it was left to
# the home's own openttd.cfg: the game writes that file every time it exits,
# so a scene played from a savegame -- or a game of the player's opened in
# this home to look at something -- left the next run's new games with that
# game's climate, map size, year, towns and industries. Scenes then built
# themselves somewhere else on a different map: three of them came out with
# different numbers the first time this happened, and two failed to build at
# all the second. Both read as a regression and were nothing of the sort.
#
# Changing any of these makes a different map and therefore different numbers
# in the stable file: it is a re-baselining, not a regression.
# The seed is handed to the newgame command itself, not left in the settings
# below. That command carries a seed of its own, and when it is not given one
# it puts a fresh random number into the setting before generating: the seed
# line below was therefore overwritten every single time, and every scene that
# starts a new game has been playing a different map -- the very thing this
# block says it stops. The setting is kept as well, so anything that reads it
# rather than the command's argument sees the same number.
NEWGAME='setting_newgame game_creation.landscape toyland
setting_newgame game_creation.generation_seed 1
setting_newgame game_creation.map_x 8
setting_newgame game_creation.map_y 8
setting_newgame game_creation.starting_year 1950
setting_newgame game_creation.land_generator 1
setting_newgame game_creation.variety 0
setting_newgame game_creation.tree_placer 2
setting_newgame game_creation.amount_of_rivers 2
setting_newgame difficulty.terrain_type 0
setting_newgame difficulty.quantity_sea_lakes 0
setting_newgame difficulty.number_towns 2
setting_newgame difficulty.industry_density 4
newgame 1'

# This build keeps its config beside its own binary and under a name of its
# own, so that a player who installs it next to the game they already play
# keeps their own settings (fileio.cpp, DeterminePaths). The config the battery
# puts back is therefore build/openttdDecouple.cfg, and a home's own
# openttd.cfg -- the vanilla one, if the home has one at all -- is never read
# or written by a run. Everything else still comes out of the home: savegames,
# base graphics, NewGRFs.
#
# The game writes that openttd.cfg every time it exits, with whatever
# settings the game it just played had. A scene played from a savegame
# therefore hands the next scene that savegame's settings -- and not only the
# map's: a scene that funds an industry or founds a town reads settings the
# block above says nothing about. Rather than chase them one at a time, the
# config is put back the way it was after every scene, so each one starts from
# the same place whatever the one before it played.
# The kept copy is the battery's own, not whatever the home happens to hold
# when a run starts: anything played in this home between two runs -- a
# savegame opened to look at something -- leaves its settings behind, and
# snapshotting those would hand them to every scene of the next run. The first
# run ever takes the copy; every run after it puts that copy back first.
CFG=$S/build/openttdDecouple.cfg
CFG_KEEP=$S/battery_openttd.cfg
# A rig set up before the config moved has the settings the scenes are written
# against in the home; take them from there the one time build/ has none yet.
if [ ! -f "$CFG_KEEP" ] && [ ! -f "$CFG" ] && [ -f "$H/.config/openttd/openttd.cfg" ]; then
  cp "$H/.config/openttd/openttd.cfg" "$CFG"
fi
if [ -f "$CFG_KEEP" ]; then cp "$CFG_KEEP" "$CFG"; else cp "$CFG" "$CFG_KEEP"; fi

run_scene() { # name scr-content ticks extra-args
  local name=$1 scr=$2 ticks=$3; shift 3
  # The rig runs the breakdown length long (a quarter of a year, the value the
  # scenes were written against); the player's default is a fortnight.
  printf 'setting vehicle.rescue_wait_days 90\n%s\n' "$scr" > $H/.openttd/scripts/game_start.scr
  # A scene played from a save must not have autoexec start a new game over it;
  # that silently ran every save scene on a fresh map for weeks.
  case "$*" in *-g*) : > $H/.openttd/scripts/autoexec.scr ;; *) printf '%s\n' "$NEWGAME" > $H/.openttd/scripts/autoexec.scr ;; esac
  HOME=$H timeout 300 $S/build/openttd -vnull:ticks=$ticks -snull -mnull "$@" > $S/reg_$name.log 2>&1
  cp "$CFG_KEEP" "$CFG"
  local spoj=$(grep -c 'spojeno' $S/reg_$name.log)
  local hav=$(grep -c 'HAVAROVAL' $S/reg_$name.log)
  local srz=$(grep -c 'Srazka' $S/reg_$name.log)
  local ast=$(grep -ci 'assert' $S/reg_$name.log)
  local odt=$(grep -c 'odtah dokoncen' $S/reg_$name.log)
  local exc=$(grep -ci 'terminate\|exception' $S/reg_$name.log)
  local dep=$(grep -c 'vjel do depa' $S/reg_$name.log)
  # The anomaly record (anomaly_log.h): lines the game writes when it had to
  # work around something. Always on, so a scene that starts writing them is
  # saying something changed even when every other counter holds.
  # The temporary consist dump (VYPIS, see LogConsistState()) writes to the
  # same record and would drown this counter, which is here to say that
  # something happened that should not have. It goes out with the dump.
  local zaz=$(grep 'ZAZNAM:' $S/reg_$name.log | grep -vc 'VYPIS')
  # Road vehicles boarding and leaving whatever carries them -- a train, a ship
  # or an aircraft (road_on_rail.h): boardings plus alightings, so a scene where
  # the ride works counts an even number of them.
  local aut=$(( $(grep -c 'nalozeno na' $S/reg_$name.log) + $(grep -c 'slozeno z' $S/reg_$name.log) ))
  # Rig commands the game refused (written ODMITNUTO by the command): a scene
  # that asks the game to do something and is told no, where the numbers
  # above would go on looking the same.
  local odm=$(grep -c 'ODMITNUTO' $S/reg_$name.log)
  # Two lines, one file each. The seven counters above are the load-bearing
  # ones and do not move between runs of the same build; the depot-arrival
  # tally does, by one, on the long tow scenes -- it is worth reading and
  # not worth diffing, so it is kept out of the file meant for diffing.
  echo "$name: spojeno=$spoj odtazeno=$odt havaroval=$hav srazka=$srz assert=$ast vyjimka=$exc zaznam=$zaz auto=$aut odmitnuto=$odm depa=$dep"
  echo "$name: spojeno=$spoj odtazeno=$odt havaroval=$hav srazka=$srz assert=$ast vyjimka=$exc zaznam=$zaz auto=$aut odmitnuto=$odm" >> ${BATTERY_STABLE:-/dev/null}
}
: > ${BATTERY_STABLE:-/dev/null}
printf '%s\n' "$NEWGAME" > $H/.openttd/scripts/autoexec.scr
run_scene zakl "vlak123 on
testspoj
testza 6000 testokna" 8000
run_scene couvej "vlak123 on
testspoj couvej" 8000
# The collector breaks down on its way to the rake and nobody comes for it,
# so the breakdown mends itself after the wait. It used to come back as a
# plain stop: waiting to be fetched had taken "go to couple" off the order it
# carries and mending gave back only half, so it booked its road to the
# platform and not to the wagons, stood against them and never coupled.
# spojeno=1 is the pass, the same as zakl.
run_scene poruchaspoj "setting vehicle.rescue_wait_days 7
vlak123 on
testspoj
testza 1000 testporucha 2" 10000
# "Brake, fail to brake and crash" (vehicle.train_braking, on: 4 = the
# driver sees 20 tiles with ETCS, the longest sight there is). A light
# engine stands braked at a platform; a train comes up behind it towards the
# path signal guarding that block, and the player's stop is pressed on it
# three tiles short. With the setting on nobody drives a stopped train: it
# brakes the gentle way, 30 % weaker, cannot stop, runs past the red and hits
# the engine (srazka=1, and both are wrecks). A tow fetches the one that
# failed to brake (spojeno=1 odtazeno=1); a helicopter lands by it and leaves
# when the tow has it; the papers write it up twice.
run_scene nedobrzdil "setting vehicle.train_braking 4
vlak123 on
testnedobrzdil cesta stopka 3 odtah" 9000
# The same with the setting off, the game as it was: the stop is the game's
# own brake and the train stands short of the red (srazka=0).
# The same with forest planted all round: nowhere in the papers' picture to
# land, so the helicopter circles over the wreck until the tow has it.
run_scene nedobrzdilles "setting vehicle.train_braking 4
vlak123 on
testnedobrzdil cesta stopka 3 odtah les" 9000
# How far the driver sees from the cab, the same setting's "sees 5 tiles"
# (vehicle.train_braking 1). He starts braking too late for a ten-wagon train
# and runs past the red into the engine at the platform (srazka=1).
run_scene nedobrzdilvidet5 "setting vehicle.train_braking 1
vlak123 on
testnedobrzdil blok vozu 10 odtah" 9000
run_scene nedobrzdilvyp "vlak123 on
testnedobrzdil cesta stopka 3 odtah" 9000
# Setting on, nobody touches the stop: the train is driven, ten wagons behind
# it. It learns that the platform is taken only when it books up to the path
# signal, four tiles short, and brakes by the physics -- seventeen tiles from
# its top speed, as long as it takes to pull away, no longer a tenth of its
# speed a tick -- cannot stop and runs past (srazka=1). The player's own
# save does the same (TEMATA_ODTAH 81.8). Three wagons brake in four tiles
# and would stop.
run_scene nedobrzdilbez "setting vehicle.train_braking 4
vlak123 on
testnedobrzdil cesta vozu 10 odtah" 9000
# The player's three block signals in a row, the stop pressed eight tiles
# short of the last one. The game's own cut to a crawl on the last tile
# before a red is left out with the setting on; this light train brakes by
# the physics in a few tiles and stands short of the red (srazka=0).
run_scene nedobrzdilblok "setting vehicle.train_braking 4
vlak123 on
testnedobrzdil blok stopka 8 odtah" 9000
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
# A collector whose engine is one unit of three equal pieces (the shape a set
# draws its shunters in, MakeEngineOfPieces()), meeting the rake nose first:
# its pieces trade places on the ground and it couples at its nose, where the
# same unit of unequal pieces is refused. The record's picture check
# (PictureKeptAfterJoin()) runs on every coupling; a line from it here is the
# flipped pieces being drawn wrong. 'couvej' is the same engine backing on.
run_scene clanky "vlak123 on
testspoj clanky
testza 6000 testnatoceni" 8000
run_scene clankycouvej "vlak123 on
testspoj clanky couvej
testza 6000 testnatoceni" 8000
# A mixed rake -- wagons of two cargoes -- and a collector asking for one of
# them when it is full. The empty wagon of the other cargo must not keep the
# rake from counting as full: the fullness question is the named cargo's.
run_scene smes "vlak123 on
testspoj smes
testfiltr 0
testfiltr plne
testza 1500 testfiltr zkouska
testza 2000 testnalozit rada 0
testza 2400 testfiltr zkouska" 12000
run_scene rad "vlak123 on
testspoj rad" 10000
run_scene blok "vlak123 on
testspoj blok" 8000
# A collector standing at the far end waiting for wagons it cannot have yet
# (the dropper still holds the platform) is called off to a depot by hand.
# The depot order must not come out still carrying the coupling errand: with
# it the train stood where it was, its window saying "heading for depot", and
# only skipping the order ever freed it -- the player's report. It drives in,
# so depa 2: the dropper's own arrival and this one.
run_scene dodepa "vlak123 on
testspoj blok
testza 600 testdodepa 2" 8000
run_scene vlek "vlak123 on
testspoj vlek
testza 9000 testokna" 8000
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
# Two rescue engines, one five tiles from the casualty and one thirty-four
# away. Each used to ask only which casualty was nearest to itself and then
# take it, so whose turn came first decided who went -- and the far one went
# (the player's report of 21. 9.). They compare themselves with each other now.
run_scene odtahdveblizsi "vlak123 on
testodtah daleko 0 dve" 16000
run_scene odtahbezdepa "vlak123 on
testodtah daleko 0
testza 1000 testdepo pryc
testza 9000 testdepo zpet" 22000
run_scene odtahvagony "vlak123 on
testodtah vagony
testza 5000 testokno 0
testza 6000 testodvoz vse" 20000
# A train the player sells to the scrapyard out on the line: the other reason a
# rescue engine is sent for something. The tow fetches it and the depot breaks
# it up instead of repairing it, so this scene counts a tow like the breakdown
# scenes do -- what tells them apart is the line saying the train was scrapped.
# Wagons an order puts down and sells, the two ways round. In a depot they are
# sold on the spot; at a platform a tow is called for them and they are sold
# when it brings them in, so that scene counts a tow like the others do. The
# price is the ordinary one -- the sale goes through the same command the
# player's own sell button uses.
# A depot order told to buy the wagons the shed has not got: the shed starts
# empty, the order wants six, so six are bought into it, made up into a rake
# and collected. The buying is switched on before the train sets off, because
# an order already being worked is a copy the train is carrying -- the same
# thing that is true of every other order flag.
run_scene koupitvagonky "vlak123 on
testspoj depo
testpocet 2 0 6
testkoupit 2 0" 12000
# The wagon list opened from an order, and its button pressed, the way the
# player does it. The only scene that looks into that window at all: it opened
# with no button in it when it was reached from a station order (no depot
# behind it, so nothing to answer with), and its list still held locomotives
# because the list was made before the window knew what it was being asked.
# Both were found by hand, neither by any counter.
run_scene vybervagonu "vlak123 on
testspoj depo
testpocet 2 0 3
testvybervagonu 2 0" 3000
# The same window with a cargo already on the order, which is the other way in
# and the one that has to keep working: the list opens narrowed to that cargo
# and the press writes both halves of the answer, the type and the cargo.
#
# Without a cargo the window stands on "every cargo" -- a number above the
# cargoes, not a cargo -- and it used to send that to the order as though it
# were one. The order refused it, the type had already gone in by then, and
# what the player got was the type set and a refusal on the screen at the same
# moment. Every counter here said the press had worked, so the probe now asks
# outright whether a refusal popped up.
run_scene vybervagonunaklad "vlak123 on
testspoj depo
testfiltr 0
testvybervagonu 2 0" 3000
# And the wagon named on the order is the only one it will couple: the shed
# is given three wagons of one kind by the deliverer and the order asks for
# another, so it leaves them alone and buys three of its own. A wagon does not
# carry just any road vehicle, which is why which one it is has to count.
run_scene koupitjinytyp "vlak123 on
testspoj depo
testpocet 2 0 3
testkoupit 2 0 28" 12000
# "Any number" on an order that buys its own wagons: a full train's worth, the
# player having set his own limit to fifteen tiles and read any as fifteen
# tiles. The limit is put down to five here so the number is small enough to
# read: the engine is one tile, so eight wagons fit behind it and nine are
# bought -- one to measure a wagon by and eight by division. Bought and
# collected are one question, not two: wagons stand in a shed one by one, so
# an order that bought nine and then collected "any rake" would leave with one
# of them.
# The same reading without any buying: "if he leaves any number, it buys the
# maximum allowed length, or it couples what is in the shed". The limit is put
# down to two tiles, which is the engine plus three wagons, and three is what
# stands in the shed -- so the whole of it leaves, and the arithmetic that says
# so is the same one the buying uses.
run_scene depocelyvlak "vlak123 on
setting vehicle.max_train_length 2
testspoj depo" 12000
run_scene koupitcelyvlak "vlak123 on
setting vehicle.max_train_length 5
testspoj depo
testkoupit 2 0" 12000
# The wagon type and the cargo filter said together -- "this model, carrying
# that". They used to rule each other out; now the cargo list is narrowed to
# what the named wagon can be fitted for, and both stand on the order at once.
# The probe line is the point: no counter can show what an order holds.
run_scene typsnakladem "vlak123 on
testspoj depo
testfiltr 0
testkoupit 2 0
testza 200 testfiltr zkouska" 12000
# And the way back out, which is one press: "every cargo" lets go of the named
# wagon as well, so the cargo list is the whole of it again and the order takes
# whatever comes. The probe must then say no cargo and no wagon.
run_scene typvsechny "vlak123 on
testspoj depo
testfiltr 0
testkoupit 2 0
testza 200 testfiltr
testza 400 testfiltr zkouska" 12000
# The filter row drops out of the window downwards instead of taking a line
# from the order list. Nothing in the rig had ever measured a window's height,
# and that is exactly where this went wrong: pressing "couple" cost the player
# a line of orders and pushed every button above it up. The scene reads three
# heights three times -- row off, on, off -- and what it watches is the list
# staying the same size while the window grows by the row and shrinks back.
run_scene oknorozkazu "vlak123 on
testspoj depo
testza 100 testoknorozkazu 1 0" 12000
# Which refit button an order shows and whether it can be pressed. Greying is
# invisible to every counter and this one has now been decided three different
# ways: at a platform a train is never refitted, because what it hauls is
# decided by what it couples and lets go of, and both happen at a platform; in
# a shed it always can be, because the test that used to grey it asks the
# engine whether anything behind it can be refitted and a collecting engine
# arrives with nothing behind it at all.
run_scene prestavbacudlik "vlak123 on
testspoj
testza 200 testprestavba 2 0
testza 400 testprestavba 2 1" 3000
# What the refit offers a collecting order, which is now a question of the
# order and not of the train: the wagons it is going to fetch, not the ones
# behind the engine at that moment. A collecting engine arrives with nothing
# behind it, so asked of the train the answer could only ever be "nothing".
# Three readings in a row -- no type named, a type named, the type taken away
# again -- and the last one is the point: the player saw a list that stayed
# narrowed after he had removed the type, because it was being read off the
# wagons he still had coupled.
run_scene prestavbanabidka "vlak123 on
testspoj depo
testza 200 testprestavba 2 0
testza 300 testkoupit 2 0
testza 400 testprestavba 2 0
testza 500 testmof 2 0 16 255
testza 600 testprestavba 2 0" 3000
# The type list of a depot order, read line by line the way the player walks
# it: the wagon named and bought, the buying switched off from the list (the
# type stays and only filters), the cargo let go to "every cargo" (the type
# stays -- the cargo does not reach into it), and the buying switched back on
# from the list, which writes the wagon's first cargo into the filter, because
# what is bought has a cargo. Each change prints what the order then holds.
# A shed stores only so many wagons (DEPOT_WAGON_LIMIT, 420): past that an
# order neither buys into it nor puts wagons down in it, and the train stands
# and says why. The shed is filled by hand to 418 with wagons of another model,
# so the collector buys two and stops at the limit, and the deliverer arriving
# with three stands in the shed with its split owed. Ten are then sold by hand:
# the collector buys the rest and leaves, the deliverer puts its three down.
# The last line reads the shed: 410 + 4 bought - 6 taken + 3 put down = 411.
run_scene depoplne "vlak123 on
testspoj depo
testpocet 2 0 6
testkoupit 2 0
testdepovagony 2 0 418 jiny
testza 3000 testdepovagony 2 0 -10
testza 7000 testdepovagony 2 0 0" 8000
# An order told to buy, that takes only full wagons. A wagon is bought empty
# and nothing loads it in a shed, so it could never take one: it used to buy
# the whole shortfall again on every tick. Now it buys nothing and says why.
run_scene koupitplne "vlak123 on
testspoj depo
testpocet 2 0 6
testkoupit 2 0
testmof 2 0 15 2" 4000
run_scene rolovaktypu "vlak123 on
testspoj depo
testza 200 testkoupit 2 0
testza 300 testmof 2 0 28 0
testza 400 testmof 2 0 16 255
testza 500 testmof 2 0 28 1
testza 600 testprestavba 2 0" 3000
# The two ways a collecting order can read its filters: "find a rake like
# this" asks the whole rake and "search the rake for this" asks wagon by wagon.
# One rake of three at the platform, two of them loaded and one left empty,
# and the order says empty + this cargo + at least one. Read of the whole rake
# that is a no, the rake is not empty; read wagon by wagon it is a yes, one
# empty wagon is in it. The player's own case, with his tanker and his one
# empty flat: he wanted "five of them are loaded, so go", and nothing he could
# set said it. The collector is held with "at least 99" until the rake is half
# loaded, or it would take the empty rake before the question is asked at all.
# spojeno=1 is the point: the switch is made on the waiting train, and a
# waiting train reads its own copy of the order, which used to miss most of
# the description.
run_scene hledejvrade "vlak123 on
testspoj
testfiltr prazdne
testfiltr 0
testminimalne 2 0 99
testza 1000 testnalozit rada 0 vagonkazdy2
testza 1200 testminimalne 2 0 1
testza 1300 testfiltr zkouska
testza 1300 testrezim 2 0
testza 1400 testmof 2 0 29 1
testza 1500 testfiltr zkouska
testza 1500 testrezim 2 0" 10000
# Founding with the search reading switched on, which the player asked to keep
# together: "when it finds no rake, it founds one". The founding order is the
# second order of the founder, a station one, and the switch is made on it
# before anything moves. The numbers have to come out exactly as in zaloz --
# the search reading changes which rakes count as something to couple to, and
# in this scene every rake there is counts, so nothing may change.
run_scene zalozhledej "vlak123 on
testspoj zaloz 6
testmof 2 1 29 1
testza 300 testrezim 2 1
testza 30000 testbrzda 3" 40000
run_scene prodatvagonkydepo "vlak123 on
testspoj depo
testprodatvagonky 1 0 1" 12000
run_scene prodatvagonkyperon "vlak123 on
testodtah vagony
testprodatvagonky 1 0 1" 20000
# The sell icon in the vehicle's own window -- the row where refitting stands
# dark out on the line. The only scene that looks into that window at all:
# which of the two buttons is in the row, and whether it can be pressed, are
# things no counter can see. The wagon list had exactly that hole once (a
# window opened with no way to answer it) and the player found it by hand.
run_scene ikonaprodatvlak "vlak123 on
testodtah prodat
testza 100 testikonaprodat 1" 16000
# And the wagons' own icon, pressed: it marks them sold, the tow comes for them
# as it does for any rake, and the shed is where they are sold.
run_scene ikonaprodatvagonky "vlak123 on
testodtah vagony
testza 3000 testikonaprodat vagonky" 20000
run_scene odtahprodat "vlak123 on
testodtah prodat" 16000
# A sold train the tow turns out not to be able to take: joined, the two would
# be longer than the game allows, so the coupling is refused on the first tick
# and on every tick after it. The engine used to stand against it asking for
# the rest of the game, silently. It gives the case up now and goes home, and
# the sold train -- which nobody else will be able to take either, by the same
# rule -- disappears, because the player has been paid and the line is his
# again. The record lines are the point of the scene, so zaznam is not zero.
run_scene odtahprodatdlouhy "vlak123 on
testodtah prodatdlouhy" 24000
# And the rule that keeps the two apart: nobody buys a breakdown. The scene
# breaks the train down and tries to sell it a moment later, so odmitnuto=1 is
# the pass here -- a zero would mean the scrapyard took it.
run_scene odtahprodatporucha "vlak123 on
testodtah prodatporucha" 16000
run_scene okruh "vlak123 on
testokruh" 16000
# The player's reverse button on a tow standing on call in its shed. The tow
# used to be put straight again every tick it stood at home, so the button
# turned it round for one tick and no longer: a tow could not be sent out tail
# first on purpose, which any other engine can. Turned three times, read after
# each, and then left to fetch the casualty the way it now faces -- the
# whole errand must still come off (odtazeno=1, as in odtahrovina).
run_scene odtahcudlik "vlak123 on
testodtah rovina
testzatik 20 testotoc 2
testzatik 30 testcouva 2 ano
testzatik 40 testotoc 2
testzatik 50 testcouva 2 ne
testzatik 60 testotoc 2
testzatik 70 testcouva 2 ano" 16000
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
run_scene nakladcil "vlak123 on
setting linkgraph.distribution_default asymmetric
testnaklad cil
testza 4000 testbrzda 1
testza 9000 testcil" 12000
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
# This scene builds its own second platform, and used to be the noisiest in
# the battery: on a random map there was sometimes nowhere to put it ("druhe
# nastupiste se nepodarilo postavit"), which gave spojeno=0 instead of 7 with
# no crash and no change of build -- 7, 7, 0 on three runs of one binary. The
# fixed flat map at the top of this file is what stopped that; if this ever
# comes out zero again, look in the scene's log for that line before believing
# anything else.
run_scene zaloz "vlak123 on
testspoj zaloz 6
testza 30000 testbrzda 3" 40000
# The player's arrangement: one station waypoint on the throat, two platforms
# behind it, and a founding order behind the waypoint. The rake fills at four,
# and the feeder then founds the next one on the platform that is free instead
# of standing and waiting for a collector -- which is the whole point of the
# scene, so the line about it in reg_zalozsmerperon.log is what to read. It
# also exercises two things that used to stop it dead: leaving the shed at all,
# and coming out of it wagons first.
run_scene zalozsmerperon "vlak123 on
testspoj zaloz smer 4" 40000
# Player's save: founding a rake behind a station waypoint while standing at
# a plain one; the feeder must go through "peron3" and found on the platform
# behind it, not on the loading platforms, and must not roll off the stub
# unbooked into the junction (TEMATA 2.38, 4.22). Two couplings, no crash.
run_scene zalozsmer "testpauza
vlak123 on" 12000 -g $S/new1.sav
# The same save run long enough for the yard to work through several rounds
# of drop and collect. A collector's road used to be allowed to end on the
# tile before its rake's platform with the platform tiles up to the rake
# nobody's; the engine that had just put the rake down rolled onto the first
# of them and came to a stand at the signal nose to nose with the collector,
# about 25000 ticks in (TEMATA 20, trains 2 and 4). No crash, and the traffic
# keeps running rather than stopping at the wreck: dozens of couplings, not two.
run_scene zalozsmerdlouho "testpauza
vlak123 on" 27000 -g $S/new1.sav
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

# A bore with signals on its mouths, put there the way the player does it:
# a drag along the line that runs across the bridge, then a train let out of
# the depot to cross it. Watched for the usual -- nothing crashes, nothing
# asserts, nobody hits anybody -- and the trace in reg_tunel.log says whether
# the train got over. Skipped where the save is not in the working directory,
# so the battery still runs without it.
if [ -f $S/brzda.sav ]; then run_scene tunel "testtunel 48 66 tah
teststartdepo 48 51
testsleduj 3 100" 12000 -g $S/brzda.sav; fi

# Two trains down one long signalled tunnel, one behind the other, on the
# player's own save. The second one now follows the first in rather than
# waiting outside for the bore to empty: it comes down to the leader's speed
# on the way to the mouth and goes in behind it. What is watched here is what
# the battery always watches -- nothing crashes, nothing asserts, nobody hits
# anybody -- with the positions in reg_tunel2.log saying whether both were in
# there at once. Skipped where the save is not in the working directory.
if [ -f $S/brzda2.sav ]; then run_scene tunel2 "testpauza
testbrzda 4
testbrzda 5
testza 2000 testkde
testza 6000 testkde" 12000 -g $S/brzda2.sav; fi

# The same two trains, with the first stopped by hand while it is inside the
# tunnel with the second one behind it. Nothing in a bore brings a train to a
# stand -- no tile boundary to refuse to cross, no signal to stand at -- so
# this is the scene that says the hold behind the train in front is a real
# stop and not merely a slower speed. It crashed both trains until it was.
if [ -f $S/brzda2.sav ]; then run_scene tunelstop "vlak123 on
testpauza
testbrzda 4
testbrzda 5
testzatik 1500 testbrzda 4
testzatik 2600 testkde" 2800 -g $S/brzda2.sav; fi

# Turning a train round in a depot doorway, which used to freeze it: the
# reverse button on a moving train sets a mark and lets it brake, and the
# turn itself was refused on the doorstep, so the mark stayed and the train
# stood there for good. Three moments on the player's save, all with the
# seven-vehicle train 2: on its way out with one vehicle through the door
# (otocvyjezd), started but still wholly inside (otocuvnitr), and on its way
# back in with part of it hidden already (otocvjezd -- it is sent back by an
# earlier turn out on the line). Each ends with the train running again;
# testdelka says the spacing survived and the log says whether it moved.
if [ -f $S/brzda2.sav ]; then run_scene otocvyjezd "vlak123 on
testpauza
testbrzda 2
testzatik 40 testotoc 2
testzatik 300 testkde
testzatik 300 testdelka 2
testzatik 900 testkde
testzatik 900 testdelka 2" 1000 -g $S/brzda2.sav; fi
if [ -f $S/brzda2.sav ]; then run_scene otocuvnitr "vlak123 on
testpauza
testbrzda 2
testzatik 5 testotoc 2
testzatik 300 testkde
testzatik 300 testdelka 2" 400 -g $S/brzda2.sav; fi
if [ -f $S/brzda2.sav ]; then run_scene otocvjezd "vlak123 on
testpauza
testbrzda 2
testzatik 300 testotoc 2
testzatik 830 testotoc 2
testzatik 1000 testkde
testzatik 1000 testdelka 2
testzatik 1500 testkde
testzatik 1500 testdelka 2" 1600 -g $S/brzda2.sav; fi

# And on a bridge: turned round halfway across, the train has to say it is
# now going the other way -- the mouths read that off its vehicles -- come
# back out of the mouth it went in by, and drive home into its shed. Skipped
# where the save is not in the working directory.
if [ -f $S/brzda.sav ]; then run_scene otocmost "vlak123 on
testtunel 48 66 tah
teststartdepo 48 51
testzatik 480 testotoc 3
testzatik 560 testmapa 48 66 48 82
testzatik 1400 testkde" 1500 -g $S/brzda.sav; fi

# The wreck the tow was sent for clears itself off the line while the tow is
# still on its way (a two-day wait, the tow far off). The tow must end its
# errand there and then and come home -- not drive on to where the wreck was
# and take whatever stands there. Measured as: no coupling, the tow back in
# its shed (depa).
run_scene vrakzmizi "vlak123 on
setting vehicle.rescue_wait_days 2
testodtah daleko 0
testzatik 300 testvrak 1
testzatik 2400 teststav" 5000

# The engine sent for a casualty is stopped by the player while still in its
# shed. Parked, it is never going to leave, so it lets the case go and the
# other engine on call fetches it instead (odtazeno=1).
run_scene stopkadepo "vlak123 on
testodtah daleko 0 dve
testzatik 260 testbrzda 2" 8000

# And stopped out on the line it keeps what it was sent for -- stopping is not
# standing down. Started again, it finishes the job (odtazeno=1).
run_scene stopkatrat "vlak123 on
testodtah daleko 0
testzatik 700 testbrzda 2
testzatik 1720 testbrzda 2" 6000

# A road vehicle ordered to board a train at one station and ride it to the
# next (road_on_rail.h): a bus goes to the first station's stop, waits for
# the shuttle, rides the wagon to the second station, gets off onto its stop
# there, works the stop, and drives back by road to do it again. Two full
# rounds: auto=4. Twelve thousand ticks and not eight: how long a round takes
# depends on how far the map put the road stop from the station, and at eight
# thousand the second alighting fell off the end on the longer maps, which
# read as a change in the feature when it was only a change in the map. The
# map is fixed now (see the top of this file), so the length is comfort rather
# than necessity -- and cheap.
run_scene autovlak "vlak123 on
testautovlak
testzatik 200 testrozkazokna
testzatik 200 testokno rozkazy auto
testzatik 400 testauta" 12000

# A train that vanishes with a car still standing on it. The car used to be cut
# loose and left in the state a carried vehicle is parked in -- the wormhole
# state, on a rail tile -- and the first thing the ordinary road code asks of a
# vehicle in a wormhole is which bridge it is on. There is none, and the game
# went down there (the player's crash of 2026-09-21). It goes with the wagon
# now, and the record says so; the scene reads that line.
run_scene autozanik "vlak123 on
setting vehicle.rescue_wait_days 1
testautovlak
testzatik 2400 testprodat 1" 9000
# Selling a train with a lorry still standing on it. A wagon with a lorry on
# its back is not for sale -- the lorry would be left on nothing -- and that is
# right when the player is doing the selling: he gets it off first. An engine
# sent to sell a train in a shed cannot, and the refusal left the whole train
# standing there sold and unsellable (the player's report of 21. 9., twice).
# The lorries go first and the train after them, which is his own reading.
run_scene prodatsautem "vlak123 on
testautovlak
testzatik 2400 testdodepa 1
testzatik 5000 testprodat 1" 12000
# Two road vehicles and one wagon, so one of them always has to wait its turn:
# the queue the "how many are waiting for a train" condition is about. The
# first rides twice and the second once, so auto=6. On a random map this one
# used to come out 4, when the road was long enough that the second car's turn
# fell past the end of the scene, or 0 when the town's local authority refused
# the road stop and the scene never got built at all ("road stop failed" in
# its log) -- both are what the fixed map at the top of this file is for.
# The condition sits at the head of the second one's list and is asked as it
# comes round; the answers are in the scene's own log, next to what the cars
# were doing.
run_scene autodve "vlak123 on
testautovlak 2
testpodminka auto 2 0 12 4 0 2
testzatik 400 testauta
testzatik 420 testpodminka auto 2 zkus 12 4 0
testzatik 1700 testpodminka auto 2 zkus 12 4 0
testzatik 3400 testpodminka auto 2 zkus 12 4 0" 12000

# The other way of boarding (road_on_rail.h): the train is a shunter with one
# order, the first station, where it then stands for good. "Load onto wagons
# or a shunter" boards it anyway -- the car goes wherever the wagon goes, and
# this one goes nowhere, so auto=1: one boarding, no alighting. The control
# gives the same shunter and the same car the "by train" order: the shunter
# does not go to the car's next stop, so the car rightly refuses it, auto=0.
run_scene autoposun "vlak123 on
testautovlak 1 posun
testzatik 400 testauta" 6000
run_scene autoposunne "vlak123 on
testautovlak 1 vlakem
testzatik 400 testauta" 6000

# Riding in an aircraft and in a ship (road_on_rail.h). Both count boardings
# plus alightings, so a working ride comes out even. The aircraft takes one
# car; the ship in this scene takes two and both get on at once, which is what
# the counter is really watching -- a carrier that holds several is the one
# thing rails never had. The ship scene digs its own canal and raises its own
# shore, since the rig's map is generated flat and has neither.
# testrozkazokna opens the orders window of every vehicle and repaints it. A
# ship's and an aircraft's window is built from a different set of widgets than
# a train's and has none of ours, and a line that touched one of ours without
# asking whether it is there took the game down the first time the player
# opened an aircraft's orders. Nothing in the rig had ever opened a window --
# every scene drives vehicles, none of them looks at one -- so it ran for days
# unseen. A crash here ends the scene, and every counter after it is missing.
run_scene autoletadlo "vlak123 on
testautoletadlo
testzatik 2000 testrozkazokna
testzatik 3000 testauta" 12000
run_scene autolod "vlak123 on
testautolod 2
testzatik 2000 testrozkazokna
testzatik 3000 testauta" 12000

# The fitting put on through the train's front, the way the refit window does
# it, with the train held in the shed. It once was refused -- the check meant
# for ships and aircraft was asked of the locomotive -- and nothing in the
# battery noticed, because the scenes buy their wagons fitted. The wagon here
# is a car carrier by birth (in toyland it takes nothing else), so the refit
# changes nothing and the ride goes on as in autovlak; what the scene watches
# is the refusal itself: odmitnuto=0.
run_scene autonaauta "vlak123 on
testautovlak
testbrzda 1
testnaauta 1
testbrzda 1
testzatik 400 testauta" 12000


# Where the carried vehicle's picture lands beside its wagon's, across the
# screen, in all eight directions -- worked out from the two bounding boxes and
# the two sprites rather than looked at. The point of keeping it: it says in
# black and white that a step across the rails moves the picture four pixels
# sideways facing north or south, two on the slants, and NOTHING at all facing
# east or west, where across the rails is straight up and down the screen. A
# sideways knob that cannot move two of the eight directions is worth having
# written down, and the numbers change the moment anything touches how either
# of them is drawn.
run_scene autosmery "vlak123 on
testautovlak
testza 2990 testbrzda 1
testza 3000 testsmery 1
testzatik 3100 testauta" 4000
