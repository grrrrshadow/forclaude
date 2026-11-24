# 3D: the map view turned to any angle

*OpenTTD Decouple by Karel Mácha. The player's word: "3D", "otáčení herní
plochy". Game side in `src/three_d.h`, `three_d.cpp` (the angle, the turn of a
world point, the keys and the mouse), the second renderer in `src/viewport.cpp`
(`ViewportDoDrawTurned()` and the functions above it), the vehicles' pictures in
`src/vehicle.cpp` (`DoDrawVehicle()`); the rig probe `test3d` in
`src/console_cmds.cpp`.*

The game has always looked at its world from one side, from the south-west,
every sprite drawn for that one view. This lets the player turn the view
smoothly to any angle. Facing north (angle 0) the game draws as it always did,
to the pixel; at any other angle the second renderer draws the same things
turned. What it has pictures of from several sides turns for real (vehicles,
eight directions; later our zin16 buildings and houses); what it has one
picture of stands as a billboard, facing the player from every side, the way a
tree in an old game does.

## In the game

- **Game Options > Graphics: "3D: turning previews and turning the map
  view"**, on by default, the row under Mitchell. Off, nothing of this exists:
  no turning, no previews, the game as it was. Switching it off puts the view
  back to north.
- **Turning.** The **middle mouse button** held and dragged sideways turns the
  view, four pixels to a degree; the cursor stays where the button went down,
  the way it does when the right button scrolls, so the mouse never runs off
  the screen (the right button scrolls as it always has). **Two clicks of the
  middle button** one after the other, without a drag (the player's idea: the
  same button as the turn), the **Home** key, or the **space bar** when
  there is no news or error window for it to close (the player asked for
  the space bar; the game binds it to those two, and with neither open it is
  free), put the view back to north; a single click does nothing, so
  pressing the button to turn never resets the view. The turned view is for
  looking; building is done facing north, in 2D, and the help text of the
  setting says so. So opening a construction toolbar (rails, roads, water,
  airports, landscaping, trees, and the editor's) or picking one of their tools
  puts the view back to north too (`ThreeDViewForBuilding()`; the player,
  15. 10.: "when he clicks construction, straighten the view to 0 so he can
  build"); rig: `test3d okno vystavba`. The keys
  **Y** and **N** turn by 15 degrees to the left and to the right, with
  **Ctrl** by 45, the next of the eight directions vehicles have pictures
  for; Ctrl+, and Ctrl+. do the same. Y and N are the only two letters no
  window of the game binds (`rotate_view_left`, `rotate_view_right`,
  `rotate_view_left_45`, `rotate_view_right_45`, `rotate_view_north` in the
  hotkeys file, under the global ones). The view turns round and round
  without a stop; the angle is counted from -180 to 180 and goes on past
  either end (the player: the stops at 180 were in the way).
- The view turns about the ground point under the middle of the screen, which
  stays where it is. Scrolling, zooming, clicking on tiles, the town and
  station names, the floating texts all work turned. Turned, the view may
  scroll on past the edge of the map, 42 tiles into the black void around
  it, for the camera to get round the corner of the map; facing north the
  edge holds as it always did. The angle is nobody's but this client's: not
  in the savegame, not in the config; every game starts facing north. Only
  the main view turns; the extra viewports and the ones in vehicle windows
  keep facing north.
- Vehicles turn with the map: a train heading north on a view turned by 90
  degrees runs along what is now a horizontal track and looks west. Turned,
  a vehicle is drawn as its **3D likeness made from its eight sprites** (the
  section on the likeness below, the same one the purchase preview turns),
  at the angle of its direction plus the turn, so it lies along the track at
  every degree in between the eight directions instead of standing still
  while the map turns under it and jumping at the next direction (the
  player: "they go the other way than the map and then jerk round"). The
  likeness is made from the vehicle's own sprites as the map draws them --
  its load, its livery, the colours its set paints on it, an engine coupled
  turned round -- so it is the vehicle, not its purchase-list picture. An
  articulated vehicle -- a truck with its trailer, a locomotive of three
  pieces -- has one likeness made from all its pieces, cut into parts, one
  for each piece that has a picture of its own (a set paints the cab on the
  piece behind the front one and the tail of a trailer over the pieces
  behind it; those go with it), and each part is drawn at the place and in
  the direction of its piece, so the vehicle bends on a curve as its
  sprites do, the front sits where the game has the front and the trailer
  where it has the trailer (the player: in build 267 the whole vehicle was
  one straight likeness, "the front sat as if nailed, the rear did not
  stay on the road"); the two heads of a double-headed engine stand at
  either end of the train; each wagon is its own. A vehicle whose likeness
  cannot be made (no sprites in the set, a crashed one, an 8bpp blitter)
  shows the sprite of the nearer of the eight directions as seen from the
  turn, as before.
- Stations turn too, any set's: the platforms as flat slabs laid over the
  turned tiles, the buildings and roofs as boxes, each from its own sprite.
- Bridges turn for real (the player: "the bridges fall apart when the view
  turns" -- drawn as billboards, the pieces of the deck over each tile stood
  north-up at their turned tiles and never joined up): over each tile the
  deck is a slab at the bridge's height with the floor sprite's pixels on
  top, the railings thin walls along its edges with the sprite's pylons and
  cables on them, the pillars columns where their pixels stand, and each
  end's ramp a deck rising from the ground to the bridge in the ramp's
  sprite; none of it ever snaps.
- Depots turn too (the player: "the depots do not turn with the map"): a
  train shed is a box over its tile with a ridge along the track, a road and
  a ship depot a flat-topped one, as high as the depot's sprite reaches,
  dressed in the shed's sprite with the door's frame drawn into it; any
  set's depot, a road type's or a rail type's, the same way. It never
  snaps, so the door stays on the side the track or the road comes in by.
- Game Options: the fences along the rails are **off by default** (the
  setting the game already had, under the graphics options, to be turned
  back on at will), since turned they are billboards and stand about the
  track at every angle but north.

## How it is drawn

The world is turned, not the camera. A world point is turned about the pivot
(the ground point under the middle of the screen when the turn began) by the
angle, and then drawn by the game's own projection, `RemapCoords()`. So the
main viewport goes on being a plain viewport of a world that happens to be
turned: the scroll position and the zoom mean what they always did, and only
the two directions world -> screen (`ProjectTurned()`) and screen -> world
(`InverseRemapCoordsTurned()`, which walks down the line of sight to the
ground like `InverseRemapCoords2()`) know the angle. Turning moves the pivot
to the point under the middle and shifts the scroll position so that the
point stays put. What the game marks for repainting (a vehicle that moved, a
tile that changed) it marks in the coordinates of the view facing north; a
turned viewport puts that rectangle through the turn first
(`TurnDirtyRect()`: its corners taken as ground points, turned and projected
again, grown by twice the height things can have, which the rectangle does
not know), so a vehicle far from the pivot is repainted where it is drawn.

For a block of the screen the second renderer takes the same lists the usual
path takes -- what every tile's draw procedure and every vehicle says to draw
-- and draws them turned:

1. **The ground.** The ground sprites of every plain tile of the block (the
   tile sprites) are drawn north-up, as the game draws them, into one texture
   of the whole block, where neighbouring tiles fit together; then every tile
   is laid from that texture over its four corners where the turned view puts
   them, as four triangles meeting in the middle so slopes keep their folds.
   Since the neighbours' pixels are right there, the edges of the tiles meet
   without a seam at any zoom. A tile with a foundation is a box drawn by
   itself from its own small texture: its ground (the foundation's child
   sprites) on top, and where the foundation raised the ground, walls from the
   land up to the ground with the pixels of the foundation sprite; the two
   walls the game draws lend their pixels, mirrored through the middle, to
   the two it never shows. Roads, rails, fields, water, the station platforms:
   all of this is ground and turns with the tile. The black the ground is
   drawn under a house or an industry (for the building to cover) is grown
   over from the ground beside it, since the building no longer covers it
   exactly.
2. **Everything standing** -- trees, foundations' walls, bridges, station
   roofs, houses without a table -- is a billboard: the sprite as it
   is, hung from its anchor point turned (`ParentSpriteToDraw::ax, ay, az`,
   the world point `AddSortableSpriteToDraw()` was given), drawn far to near
   by the turned depth of its bounding box. The game's own sprite sorter is
   not used here. **A vehicle** is its 3D likeness from its eight sprites
   (`AddThreeDVehicleToViewport()` in `src/three_d_vehicle.cpp`, called from
   `DoDrawVehicle()` in place of the sprite when the view is turned). The
   likeness of a vehicle on the map is made from the vehicle's own sprites
   (`Vehicle::GetImage()`, read as the game draws them: the colours its set
   paints on, its load, its shift, `LoadSpriteAsDrawn()` in
   `spritecache.cpp`), one model per look (`VehicleLook()`: the sprites of
   two directions of all its pieces hashed), so two vehicles of one engine
   that look alike share a model and a loaded and an empty wagon do not.
   It is rendered at the angle of the vehicle's direction plus the turn,
   foreshortened along itself between the directions the way the sprites of
   the track directions are (`on_track`), kept per whole degree, and hung
   from the vehicle's position with its bounding box, sorted with the rest
   by turned depth (`box_kind` 5, `AddTurnedVehiclePicture()` in
   `viewport.cpp`). A vehicle of several pieces -- an articulated one, a
   double-headed engine -- has one model made from its pieces composed as
   the chain spaces them (`PartPositions()`: a train's pieces centred,
   each half its length and half the next one's apart; a road vehicle's
   each the whole length of the one before behind it, as
   `IndividualRoadVehicleController()` lets them out of the shed), and
   knows where each part lies along it (`part_x`, `part_cut`,
   `piece_part`, `part_piece`): a part is a piece with a picture and the
   pieces without one that go with it (the player's lorry set has a front
   piece of two eighths with no picture and paints the cab on the piece
   behind it, the trailer on one piece with two empty ones behind), and
   the model is cut where the last piece of a part ends (a train's half
   its length behind its position, a road vehicle's its whole length).
   Each part is drawn about the position of its piece, in that piece's
   direction, so the vehicle bends on a curve as its sprites do, and the
   rear head of a double-headed engine stands at the end of the train.
   While the pictures of the eight directions are read, the set is told
   the vehicle faces the direction asked and runs straight
   (`_resolve_vehicle_direction`, `newgrf_engine.cpp`: variables 9F, 45
   and the direction part of 62), since a set may place a layer by the
   direction the vehicle really has -- the player's lorry set shifts the
   cab by variable 9F -- and read with the vehicle facing another way,
   the picture of one direction came out with the shift of another, every
   view different from the next, and the hull carved from them was a few
   specks scattered along the road (builds 268 to 270). The same is told
   for the purchase list's pictures, which have no vehicle, so that set
   gets a direction there instead of none. A vehicle without a likeness is
   a billboard of the sprite of its turned direction, as before.
   **A bridge** is built from the sprites `DrawBridgeMiddle()` draws for
   it, marked by the same hook the cab view uses (`MarkCabBridgeSprites()`,
   `TurnedBridgeOverTile()`): the floor sprite becomes the deck, a slab
   two units thick over the whole tile at the bridge's height (`box_kind`
   8, `DrawTurnedBridgeDeck()`), and besides it the far railing, an entry
   of its own at the far edge so that it sorts there against what runs on
   the deck; the near part is the near railing (9,
   `DrawTurnedBridgeRailing()`): a wall a tile long and forty units high
   along the edge, the sprite's pylons and cables laid on it with the gaps
   between the cables left open (`DrawTexturedTriangle()` otherwise fills
   a pixel the texture has nothing for from the pixels about it, for the
   edges of the ground's diamonds, and filled the cables into a sheet); a
   surface laid on the deck (a rail type's own deck, a reservation) is a
   slab a step above it (6); a piece of a pillar is a column a level high
   where the sprite's pixels stand (11, `DrawTurnedBridgePillar()`: its
   box says only whether it is the near or the far pillar, the game draws
   the far one so many units back that its pixels land at the far edge;
   where along the bridge it stands is read off the sprite's middle). A
   bridge's end (`TurnedBridgeHead()`) has its ramp and whatever lies on
   it drawn as the deck rising from the ground at the outer end to the
   bridge at the inner one, or level with it on a slope or a foundation,
   dressed in the sprite, with its sides down to the ground (10,
   `DrawTurnedBridgeRamp()`, the geometry of the cab view's
   `CabDrawBridgeRamp()`). None of a bridge ever snaps (period 360).
3. **The houses of the original graphics with a hand table**
   (`table/three_d_houses.h`, 26 temperate houses read off their one sprite
   by hand: a few boxes each with flat, gable or hip roofs) are boxes. The
   sprite is drawn north-up into a texture and laid over the faces of the
   boxes: a face the sprite shows (the walls facing south-west and
   south-east, the roof slopes facing them, a flat top) takes its pixels
   where the game draws it, a face it never shows takes them from the
   opposite face of the same box, mirrored. Only the front of a house turns,
   by up to 45 degrees either way: past that the house snaps a quarter turn
   back and its front turns again, so the back of a house of one sprite is
   never on show (the player: it is only background). A house of several
   tiles is drawn tile by tile, the boxes cut to the tile, each from its own
   sprite. The black the ground is drawn under a house (for the sprite to
   cover) is grown over from the ground beside it. **Anything else standing
   on a house, industry or object tile** -- a house without a table, an
   industry's building, a lighthouse -- is one box with a flat top, its
   bounding box, turned the same way; where the sprite is narrower than the
   bounding box's footprint on the screen (a tower on a wide base) the box is
   narrowed to the sprite's width. Low things (fences, heaps, under a height
   level) stay billboards. **A station tile** (any set's: the layout gives
   every sprite its box) turns the same way: a low flat thing -- a
   platform, a bench -- is a slab, its whole box with the sprite laid over
   the top and its low walls, which turns for real and never snaps, since a
   flat top shows at any angle and the platform's edge must stay by the
   track (`box_kind` 6, `DrawTurnedSlab()`); anything of a height -- a
   building, a roof -- is a box as above. The rails under the platforms are
   ground and turn with the tile as they did.
4. **The buildings rendered from eight directions with depth and normals**
   are gone since 15. 10.: every one of them is a real 3D object now (the
   houses in `objekty/mirne/`, our industry in `objekty/prumysl/`, below).
   What they were: (`src/three_d_model.cpp`, the table `table/three_d_models.h`: ten
   houses of the temperate climate, the school, the statue of stone and of
   bronze, the machine and the hut) turned for real. Every pixel of a render
   is a world point -- the camera that took it says where, the depth
   sprite says how far along the line of sight -- so the eight renders are
   eight point clouds in one frame, the building's. At any angle the two
   renders either side of it are put, point by point, through the game's
   turned projection and splat where they land, the nearest point winning
   its pixel; where both renders have a point at a pixel within 0.4 m of
   each other it is the same surface and their colours mix, by the angle
   and by how squarely each render saw the surface (its normal against the
   line of sight); where one has it, it alone. At the eight directions
   themselves the picture is the render. The building is drawn once, whole,
   at the north corner of its north tile; the sprites of its tiles are left
   out. A render is read from its sprites when first needed and kept per
   level of zoom, a pixel of a level standing for a block of pixels of the
   render (the nearest surface in the block), so the work goes with the
   pixels on the screen, not with the render; up to 320 MB of renders are
   kept, the least recently used going first. The theatre's picture takes
   12 ms at 8x, 3 ms at 4x and a fifth of a millisecond at the normal zoom;
   reading a render from its sprites takes 40 ms (a house) to 170 ms (the
   school, the hotel), paid when a building is first drawn and when the
   turn crosses one of the eight directions. Facing north the game draws
   the house or industry as it always did (its original sprite), so a house
   with a render changes its look at the first degree of a turn.
5. **The signs** (town, station, waypoint and sign names) and the floating
   texts are collected north-up and each moved to where the ground under it is
   drawn turned.

The usual path collects against the block being drawn; the turned path
collects against the north-up box of the world under the block (from the
ground to the tallest thing there can be), so nothing the turn brings into the
block is clipped away. The block is first cleared black, which also tells the
32bpp animation blitter that these pixels do not animate; the textures and
faces are written straight into it, the billboards through the blitter as
always. It needs a 32bpp blitter: with an 8bpp one the angle stays 0.

   **The marijuana plantation** is laid a tile at a time from layers rendered
   the same way (`ThreeDModelKind::Layer`, found by name): a field tile is
   the soil with the plants of the stage and, when the girls are there, the
   girl at work on it (the six of them, as `DrawMarijuanaPlantationTile()`
   puts them); from the field tile before the field road comes the road tile
   behind it, each thing on it its own: the road, flat, sorted before
   whatever stands on it, the shed and the haystack at its end, the pairs of
   girls by its edges. All the layers of a tile go into one picture, the
   nearest point winning each pixel, so a girl stands among the plants
   (`GetThreeDLayersPicture()`, `GetThreeDPlantationTile()` in
   `industry_cmd.cpp`).

What is not there yet: the girls about the school, the machine and the statue
and the hut's girls as renders of their own (the buildings turn without them
for now, in the map and in the preview), the plantation in the preview, boxes
for the houses of the other climates, the proper sorting of long things
against each other, the smaller textures of the zoomed-out view, the seams at
the edges of the map. The first likeness of a vehicle's look is made when it
is first drawn turned, a second or two in which the game waits. Facing north
nothing of this runs.

## Buildings as real 3D objects

`src/three_d_objects.h`, `.cpp`. The player (13. 10.): "the next houses will
be real 3D objects, not eight renders, to save megabytes"; the colleague made
every temperate house of the original game so (grrrrf `hra/3d/objekty/`, the
format in `hra/3d/OBJEKTY-3D.md`), 31 files for the 21 houses and their colour
variants, 3.5 MB in all. They lie in `media/baseset/decouple/objekty/mirne/`
with their index `objekty.json` (CMake copies every set under
`decouple/objekty/` into the game's `baseset/`), and the game reads them from
`baseset/decouple/objekty/<set>/` the first time one is wanted, the folders
in the order of their names. The houses of the sub-arctic and sub-tropical
climates followed (14. 10.; the player: "he has those houses done, take them
in"): `subarkticke/` (19 objects, 4.6 MB) and `subtropicke/` (15, 1.5 MB). A
house is found by its number whatever the climate played, so the houses of
another climate in a themed town of the temperate one ("Houses from" in the
town window, the settings of desert and arctic towns) are drawn as objects
too.

- **The file.** glTF binary (`.glb`): the JSON and the buffer; the first
  mesh's triangles, their corners' positions and texture coordinates, the
  node's transform applied; glTF's Y is up and its Z points along the game's
  -y (Blender writes x, z, -y); metres to units, a tile 14.83765 m as the
  renders have it and a height level 3 m. The base colour's texture is a PNG
  in the buffer, read by libpng into the blitter's colours the first time the
  object is drawn (an alpha under half is transparent), halved into levels of
  detail down to a pixel.
- **The index** says which house of the original game each object is
  (`nahrazuje_house`, the house of its north tile; or a list of them, a
  sub-arctic house below the snow line and the same one above it, the later
  ones dressed in the snowy texture `snih` names, a PNG beside the `.glb`
  over the same UVs), its size in tiles and
  which colour variants (`varianty`, the slot `TileHash2Bit()` of the tile
  picks in `_town_draw_tile_data`); a variant whose sprite is another shape
  has an object of its own. An object reaching out of its plot is said in
  the log (`-d misc=1`).
- **A set's houses.** An object may be a house of a set (NewGRF) instead:
  `nahrazuje_grf`, the set's id as the game prints it, and
  `nahrazuje_house_grf`, the house's number in the set (its Action 0 id) --
  the colleague's `mars/` (14. 10., 12 objects, 2.9 MB): the seven houses of
  (Fixed) OpenGFX Mars Houses, `524A450B`, the set the game fetches itself
  and builds its Mars towns of (`mars_houses.h`), two looks by the colour
  slot for five of them. `GetThreeDHouseObjectAny()` finds an original house
  by its number and a set's by the set and its number.
- **Where.** A finished house of the original game or of a set the objects
  name (not one being built, houses not see-through) whose north tile's
  variant has an
  object, and that has no eight renders (those keep them): in the **turned
  view** it is drawn once, whole, among the standing things by the box of its
  plot (`box_kind` 7, `DrawTurnedObject()`): its triangles turned with the
  view, far first by their middles, as the faces of the boxes, each filled
  with its texture at the level of the zoom -- the texture's full width for a
  tile at the closest zoom, halved with every zoom level out; the sprites of
  its tiles are left out. A triangle turned away from the view is left out
  (`ThreeDObject::facing`: its outer side by its corners' order, or by its
  normals where they disagree; a `doubleSided` material is drawn from both
  sides), so a back wall never comes through a front one; a triangle outside
  the block being drawn is skipped before sorting. In the **cab view** its
  triangles go through the
  perspective rasterizer and the depth buffer with the texture's level for
  about a texture pixel to a pixel of the picture.
- **A name of its own** (`"nazev"`: a string, or `{"cs": ..., "en": ...}`): the land area information shows it
  for the finished house drawn as that object, 3D on (`GetThreeDHouseTitle()`, `TileDesc::name`): house 0x1F is the
  ÚV KSČ building (the colleague's `hra/3d/PROSBA-UV-KSC.md`).
- **Facing north**: the main view is drawn by the second renderer at angle 0 too, the same as turned (the player,
  28. 10.: "3D without the choice of the old way"; `IsViewDrawnTurned()`, `IsViewportTurned()`; finding the tile
  under the mouse, the clamp to the map, the dirty rects and the round curves stay as facing north,
  `IsViewportTurnedAway()`). The setting *Houses when the view faces north* is gone. The small views (a vehicle's,
  the extra viewports) do not turn; while the main view faces north they draw such a
  house in the game's usual north renderer by the same object
  (`AddNorthHouseObject()` after its tile's draw: its sprites `box_kind` 4, the
  object `box_kind` 7 among the sorted sprites, `ViewportDrawParentSprites()`);
  fences, catenary and vehicles stay as they are facing north. A house on a slope keeps the walls of its foundation, the
  ground on them left out for the object's own plot (`LeaveOutTileSprites()`; the player, 15. 10.: nothing under
  the houses on a slope, black holes in a screenshot -- on the screen what was drawn there before). Only while the main view is at angle 0 (the other views then too),
  3D on and a 32bpp blitter. A full redraw of a dense town at zoom 1 in the rig:
  44 ms with the objects, 2 ms with the sprites (the turned view 128 ms).
- **Light and colour by number** (15. 10., the ten houses that were the
  renders of `budovy3d_1` to `4`, the colleague's
  `hra/3d/PROSBA-DOMY-Z-BUDOVY3D.md`): a file with normals is lit by the game
  as the vehicles are -- each corner 0.55 + 0.45 max(0, n . L), L = (-0.55,
  0.35, 0.75) in the world's axes, fixed to the world as the sun, so it does
  not change as the view turns; across a triangle the light goes from corner
  to corner (`TurnedVertex::l`; in the cab view one light a triangle). A file
  without normals has its light in its texture and is drawn as it is. And
  `"barvy"`: parts (`cast`: roof, plaster, fence) with a grey `maska` of the
  texture's layout, `zacatek` and a `paleta`; each house on the map has for
  each part a colour of its palette by a hash of its north tile, another bit
  of it for each part (`ThreeDObject::PaintAt()`), the colleague's "each house
  another" -- a pixel of the mask that colour at the texture's lightness (its
  own where the texture is `zacatek` light), as the vehicles' `lak.barva`. The
  texture so painted is kept by level and colours (at most 192 MB of them,
  then all are dropped and made again as drawn).
- **Figures on a building** (`"postavy"` of a house: `postava`, `poloha` in
  metres of the building's frame, `otoceni` degrees in the world, `meritko`):
  drawn among its triangles by the middle of their height, so a parapet in
  front hides their feet (`DrawTurnedObject()`). The player (15. 10.): College
  Girl smoking by the parasols on the roof of the hotel (0x07), true size.
- **Depth inside an object** (15. 10.): the triangles of an object, its figures
  and plants go through a depth buffer over the pixels it covers
  (`DrawObjectInto()`; nearness xr + yr + 2 z of the turned world, a figure as
  near as its middle), still far first -- so the flags of the ÚV KSČ building
  no longer show through its front, nor the grass of the field road through
  the road laid a few centimetres over it.
- **Our industry as 3D objects** (15. 10., the colleague's `objekty/prumysl/`,
  `hra/3d/PROSBA-PRUMYSL-3D.md`; the sets `budovy3d_5`–`8`, 176 MB of renders,
  gone): records `"druh": "prumysl"` with `prumysl` the industry
  (`IT_GYMNASIUM`, `IT_STATUE` with `varianta` `kamen`/`bronz` by the
  industry's random bit 0, `IT_WEED_MACHINE`, `IT_COFFEESHOP`; `lihovar` and
  `hospoda` are read and wait for their industries), drawn once, whole, at
  the industry's north corner (`GetThreeDIndustryObject()`). The plantation is
  laid tile by tile from the parts of the fields (`"druh": "pole"`, `dil`,
  `GetThreeDFieldTile()`): the soil `zem` under the plants of the stage
  (`mari_male`, `mari_vzrostle`; the potatoes' parts wait for the potato
  field), the girl at work on the tile from the soil's `prace` (her name
  `pole_<name>`, twice the size), and from the tile before the field road the
  road `cesta` (flat, sorted first, under the cars), the shed `bouda` and the
  haystack `seno` on its first tile and the girls by its edges (`u_cesty`,
  `okraj` `sz`/`jv`), each its own thing to sort. The potato field (`IT_POTATO_FIELD`,
  15. 10., the player: "the potato field like the marijuana one, and the same") is laid
  the same way with `brambory_male`/`brambory_vzrostle` and the soil's `prace`
  `brambory`; the distillery (`lihovar`, `IT_DISTILLERY`, 3x3, potatoes to alcohol)
  and the pub (`hospoda`, `IT_PUB`, in a town, alcohol to passengers) are drawn
  from their objects. In the small views facing north the game's own industries are drawn from
  their objects too, as the houses (`AddIndustryObjects()`): the potatoes, the distillery and the pub have no
  pictures of their own (without the objects: the soil, a warehouse in a
  concrete yard, an old house).
- **The girls of the industry** (16. 10., the colleague's
  `hra/3d/PROSBA-PRUMYSL-HOLKY-NOC.md`; the player: "the statue of Karel, where
  the studentky are when I bring them"): `postavy_s_holkami` of the statue, the
  school and the machine are drawn while `HasGirls()` (30 days after the
  studentky, the machine's after the marijuana), the hut's `postavy_stoji` and
  `postavy_sedi` as `HutGirlsAt()` says (`GetThreeDIndustryGirls()`), on the
  map, in the preview beside the industry's window and in the view from the
  eyes; on the day they go the industry is drawn again (`LetGirlsGo()`). Their
  `meritko` 2 as all girls outside a cab; in the view from the eyes at their
  true size (`CAB_FIGURE_SCALE`), a thing among the figures (`"druh":
  "predmet"`, the spit) as big as the building. The pub is **2x2** (the whole
  scene twice as big): its north tile the old house (`GFX_PUB`), the three
  others its garden (`GFX_PUB_YARD`); a pub of one tile from before keeps it,
  drawn by its sprite (`GetThreeDIndustryObjectOf()`: an object only on an
  industry of its size). The school, the distillery, the machine and the pub
  have their windows lit at night as the houses (`noc`, `svetlo_NN`). Rig:
  `test3d dovoz <x> <y> stud|mari|boom|nic`, `test3d budovy`.
- **The industries of the original game** (16. 10., the colleague's set
  `objekty/prumysl_vanilla/`, `hra/3d/PROSBA-PRUMYSL-VANILLA-3D.md`; the
  player: "we have the modular industry done"): one set of 46 parts
  (`"druh": "dil"`: halls, chimneys, tanks, heaps, yards, trees, ...) shared by
  all the industries of the temperate, sub-arctic and sub-tropical climates,
  and 122 tiles (`"druh": "prumysl_dlazdice"`) that place them, each for the
  `gfx` of the original industry tiles it stands for (128 of them; Toyland has
  none and is drawn by its sprites): `podklad` the yard under it (a part drawn
  as it is, flat and first, as the field road), `dily` the parts with
  `poloha` (metres from the tile's north corner), `otoceni` and `meritko`,
  `rostliny` its trees. A building bigger than a tile is a block: its anchor
  (`blok` [x, y] tiles) draws it whole, the other tiles (`soucast_bloku`,
  `posun` from the anchor) only their yards. A part of another set (the
  haystack and the shed of our fields, `sada`) stands as it is. Each part is
  read once (`ReadIndustryParts()`), its texture kept by its own object, and
  placed on the tiles as objects of their own that draw with that texture
  (`ThreeDObject::texture_of`, `PlaceIndustryPart()`), lit by their normals
  turned with them, their windows lit at night (`noc`, `svetlo_NN`). Drawn on
  the turned map, in the small views facing north as the houses and
  in the view from the eyes (`AddOriginalIndustryTile()`); an industry being
  built shows its yards only, an animated tile (the mine's wheel, the oil
  wells) the same as at rest; an industry tile of a set (NewGRF) keeps its
  sprites. **Wheat on the farm's fields** (`"druh": "pole"`,
  `psenice_mlada` 0.63 m, `psenice_zrala` 1.36 m): none on the ploughed field
  nor on the haystacks of the harvest, young while it grows (the field's
  type 1 to 3), ripe before the harvest (4 to 7), each plant lifted onto the
  field's slope (`GetThreeDSlopedPlants()`); in the view from the eyes up to
  10 tiles away (`CAB_WHEAT_TILES`), farther the field's ground says enough.
  Rig: `test3d puvodni` lists the original industries drawn from the parts
  and the fields with their first young and ripe tile.
- **Plants** (`"rostliny"` of a building or a part: `soubor` a record
  `"druh": "rostlina"`, its foot at the origin and 1 m high, `poloha`,
  `otoceni`, `meritko` [x, y, z] metres): drawn as figures, a picture of the
  plant at the view's angle (`GetThreeDPlantPicture()`), the angles to the
  view and to the light in steps of 10 degrees and the sizes in twentieths,
  so the hundreds of plants of a field share a few pictures; baked into the
  building they would be up to 360 000 triangles (the hut) and 125 000 a
  tile of the grown field.
- **Things that turn** (`"druh": "predmet"` of the figures' index, the pub's
  spit): `otaceni` -- `osa` the axis in the thing's frame, `stred` a point of
  it, `tiku_na_otacku` the game ticks a turn -- and `stridani` -- `uzly` the
  nodes of its frames, `snimku` how many, `otocek` the turns each is shown
  (the pig, the calf and the lamb). Drawn as a figure, its picture turned in
  24 steps a turn (`SPIN_STEPS`, the vertices and normals turned about the
  axis, `RasterObject()`), its tile drawn again at every step.

One house is kept as the first technology drew it, the boxes of its hand
table, in memory of it: 0x1c, the tall office block (the player: "I am
nostalgic, the white house of many storeys, tall"); its objects are not used
(`GetThreeDHouseObject()`).

## Vehicles as real 3D objects

`src/three_d_vehicle_object.h`, `.cpp`, the reading in `src/three_d_objects.cpp`.
The player (14. 10.): "we will never render anything again; vehicles go into
the game as objects, like the houses". The colleague's request and format:
grrrrf `hra/3d/PROSBA-VOZIDLA-3D.md`, the answer `hra/3d/ODPOVED-VOZIDLA-3D.md`.

- **With the GRF, not in the game** (the player, 15. 10.: "objects must be as
  independent as GRFs: whether I give the Sergej object or not, the game cannot
  carry it, it is to be with the GRF"; the colleague's
  `hra/3d/PROSBA-OBJEKTY-U-GRF.md`): a vehicle comes in a package beside its
  GRF anywhere in the NewGRF folders, a folder or a tar, as GRFs come --
  `<package>/<name>.grf` and `<package>/objekty/<set>/objekty.json` with the
  files it names, the figures of its crew in `objekty/postavy/` of the same
  package (found there first, then among the game's). The game looks through
  the NewGRF folders and their tars for `objekty/<set>/objekty.json`
  (`PackageIndexScanner`, files in a tar read through `FioFOpenFile()`, their
  paths `tar:` and the name in the tar), and again each time the GRFs are
  loaded anew (`ResetThreeDVehicleObjects()` from `GfxInitSpriteMem()`): the
  first look, early in the start, comes before the tars are known. A package
  may lie in `baseset/decouple/` too, beside the game's own sets, a folder or a
  tar (the player: "from the decouple folder, tar"): its GRF is then one of
  the game's own, in every game (`GRFFileScanner` reads the GRFs of tars there,
  `IsInBuiltinGRFDir()`), its objects found as those of `newgrf/`. The game's
  own `baseset/decouple/objekty/` holds no vehicles any more, only the houses
  of the game and the figures it draws itself; those sets are read from every
  place the game reads its data from, each set once, the player's own folder
  first (`Documents/OpenTTD/baseset/decouple/objekty/<set>/`), so a set there
  needs no build either. Tried with the colleague's `M62_Sergej_3D-v1.tar`
  (GRF `MAX3`, every sprite empty): as a tar and unpacked, Sergej on the map,
  in the depot, the purchase list and the turned view, its driver from the
  package. **Two versions** of a set side by side (16. 10., the colleague's
  `hra/3d/PROSBA-TATRA-V6.md`): the package beside the GRF loaded wins
  (`PackageBesideLoadedGrf()`, the GRF's folder, as the game names it in a
  folder or a tar, is the package's), else the first in the order of the paths.
  A set whose engine has **empty pictures** (one pixel, in the purchase list or
  at a vehicle's front) and no object is said once in the log at level 0, with
  the folders the game looked in (`NoteThreeDVehicleWithoutObject()`): its
  package was not found, and nothing at all would be seen. Tried with the
  colleague's `Tatra_osobni_3D-v6.tar` in `newgrf/` beside v3 (v6's objects
  drawn, states 1 to 64 found), in `baseset/decouple/`, and its GRF alone.
- **Index**: entries `"druh": "vozidlo"` of any `objekty/<set>/objekty.json`
  of a package: `soubor` (.glb), `typ` (`vlak`,
  `auto`, `lod`, `letadlo`), `grf` (the set's id: four characters as in the set,
  `"MAXb"`, or eight hex digits as the game prints it), `clanky` (the engines'
  numbers in the set, Action 0, front first; or `motor` alone), `lak` and `kola`
  (below), `kotva`
  (`stred`, the middle piece, by default; `prvni`; or a number), `textura` (a
  paint: a PNG over the same UV instead of the file's texture), `kabina`
  (x, y, z of the cab's eye, metres; read, not used yet). `meritko` is not
  read: the objects are in true metres and their size is the setting below.
  The index is read the first time an engine is looked up, a .glb the first time
  its vehicle is drawn.
- **The object**: one mesh, metres, the vehicle's frame -- x forward, y to its
  left, z up, the origin in the middle of the vehicle on the top of the rail --
  `POSITION`, `NORMAL`, `TEXCOORD_0`, one base colour texture, no baked light.
- **The sprites**: an engine of an object gets no sprites from its set at all
  (`GetCustomEngineSprite()` asks `GetThreeDVehicleObjectSprite()` first). The
  game draws the object into a sprite of the sprite cache of its own
  (`AllocateDerivedSpriteID()`, `SpriteCache::object_sprite`), made when it is
  first read (`ReadSprite()` -> `MakeThreeDVehicleObjectSprite()`): at 16x with
  the camera of the sprites (`RasterObject()`: 30 degrees above the horizon,
  48.8 px/m, direction d turned 90 - 45 d degrees, trains and road vehicles
  foreshortened 1/sqrt 2 along themselves in the four directions of straight
  track, as the sets draw), triangles through a depth buffer, the texture
  nearest, twice the size and shrunk; the coarser levels are made from it as
  from a set's zin16. Lit from the normals by one light fixed to the world, the
  colleague's: brightness 0.55 + 0.45 max(0, n . L), L = (-0.55, 0.35, 0.75) in
  the world's axes. So the map facing north, the depot, the lists, the purchase
  window and the turning preview (made from these sprites) all draw it.
- **One rigid object over the pieces**: on the map, in the depot and the lists
  the anchor piece draws the whole object about the middle of the chain (its
  sprite moved from the piece's position by the pieces' spacing,
  `ThreeDChainPositions()`, and the game's sprite anchor, `ThreeDSpriteAnchor()`);
  the other pieces draw an empty picture. In the purchase window and the news,
  where only the first piece is drawn, the first piece draws it all.
- **The turned view**: the object itself at the angle of the view
  (`AddThreeDVehicleObjectToViewport()`, before the 3D likeness), foreshortened
  smoothly between the directions as the likeness is, hung from the middle of
  the chain (on a round curve from its point on the arc), sorted by the box of
  its pieces; the pictures are kept by object, zoom and degree.
- **Size and length, the setting** *Vehicles from 3D objects at the original
  game's size* (`vehicle.object_original_size`, new game, off): the original
  game's size as the bigger sets draw (BRYLE), 1.2, or CZTR's scale, 1 -- one
  file for both (the player: "the game can make it bigger"). The length of the
  pieces on the track is the object's (the player: "do it from the object"):
  its length at that size, a step of the chain to a step of diagonal track,
  shared by the pieces in the proportions of the set, each at most a whole
  vehicle (`ThreeDVehicleObjectPieceLength()` in `Train::ConsistChanged()`,
  `GetRoadVehLength()`); Sergej, 17.69 m, is 13 eighths small and 16 big, as
  the colleague's two sets have it. Ships and aircraft keep their sets'
  lengths. The lengths depend on the object files: every player of a network
  game must have the same.
- **Size as a parameter of the set** (`"velikost": {"parametr": 0, "hodnoty":
  {"0": 1.2, "1": 1.0}}`, the colleague's Sergej v3): an object with it is drawn
  at the scale its set's parameter makes (`GRFFile::GetParam()`), the setting
  only for those without it. **All the 3D sets at once** (the player, 15. 10.:
  "instead of the frame's line our text size and a toggle"): on the bottom line
  of the frame of the active list in the NewGRF window, as its caption on the top,
  *Size of the 3D vehicles* and a toggle, original (bigger, BRYLE) or smaller
  (CZTR): a click sets every active set with `velikost` to the value of its
  biggest or smallest scale, and in the window of a new game the setting too
  (`NewGRFWindow::ToggleObjectSize()`, `WID_NS_ACTIVE_FRAME`); it shows the first
  such set, or the setting. In a game it changes only with the parameters (the
  NewGRF developer tools) and is applied as they are.
- **Road vehicles of a package too** (15. 10., the colleague's Tatras 613, 613 VB and 603, GRF `MAXt`,
  `Tatra_osobni_3D-v3.tar`): a record `"druh": "auto"` with `grf` is a vehicle as `"vozidlo"` is, `typ` `silnice`
  (a road vehicle) by default; an `auto` without `grf` is a car of the game's own driving about by itself (later).
  Nothing of them is in the game: the GRF and its objects come in its package.
- **Cars of the game's own** (the player, 15. 10.: "the Tatra will be in the game like the girls at the stops and
  Mat", "it will drive like Mat, from one ÚV KSČ to another"): a record `"druh": "auto"` without `grf` and with
  `"jizda": {"k": "dum", "nahrazuje_house": 31, "rychlost_kmh": 60, "stoji_tiku": 400}` in the game's set
  (`objekty/auta/`) is a car that drives by itself in the turned view (three_d_walkers.cpp, `_drivers_timer`): from the
  stop (`"stani": {"poloha", "otoceni"}` of a house's record) of one such house near the view to another the roads
  lead to (a search over road tiles, `RoadRoute()`), keeping to the side of the road the game drives on, stands so
  many ticks and drives on. One of each such record; only the screen, never saved. Rig: `test3d auta3d`.
- **The state is the GRF's** (the player, 15. 10.: "the colour, the lid and the beacon the GRF must do, the mod; when
  it does not, nothing, it is ignored and does not fade"; grrrrf `hra/3d/ROZHRANI-GRF-STAVY.md`): the game decides
  nothing of how a vehicle looks by itself -- no colour by its number, no fading or rust by its age. The GRF picks a
  set of its sprites for the vehicle by its own rules as always (random bits, age, breakdown, load, a counter to
  blink), and `"stavy"` of the record says what a set means: `{"sprity": [first, last], "barva": "RRGGBB", "lak":
  0..1, "rez": 0..1}`, the set whose first sprite (its line in the GRF, the sprite count line 0 and every sprite and
  pseudo sprite a line on, `GetSpriteLocalID()`; not yagl's `sprite_id`) is in the range
  (`GetVehicleSetSpriteLocalIDs()`, `StateStep()`; of each layer, below). Without `stavy`, or a set in no range: the object as it is, the
  paint new, no rust. A sprite and a picture of the turned view for each state, made when first drawn. Rig:
  `test3d stavy` prints each vehicle's sprite number and state; `test3d stav <n>` draws every such vehicle in its
  state n (0 as its GRF picks). (Later: `dily`, a node turned, the lid.)
- **Add-ons and groups of the crew by the state** (16. 10., the colleague's `hra/3d/PROSBA-V3S-3D.md`, the Praga V3S
  `V3S_3D`, `MAX4`): `"doplnky": [{"soubor", "poloha", "uzly": [...], "barva": {"maska", "zacatek"}}]` of the record
  are files laid on the vehicle, of which only the node a state names (`"doplnky": {"<soubor>": "<uzel>"}`) is drawn,
  with its children; without a state none. `"barvy_doplnku": {"<soubor>": "RRGGBB"}` colours the add-on where its
  mask is, at the texture's lightness as the paint (`zacatek`), not fading. `"posadky": {"<jmeno>": [crew...]}` are
  groups of the crew a state shows by `"posadka": "<jmeno>"`; a figure of the crew may be `meritko` times its size
  (the girls on the back twice). Each node and group is a part of the shape (`ThreeDVehicleShape::tri_part`, at most
  63; `ThreeDVehicleObject::part_names`, `PartsOf()`); the add-ons' textures are marked as theirs (`tex_addon`). The
  load and the groups are not of the vehicle's length on the map. The beacon of the Tatra 613 VB goes so too.
- **Glass** (`"sklo": {"uzel", "alfa"}`): the windows' node, drawn after the rest, each layer laying its colour over
  what is behind it by `alfa`, not hiding it; over nothing a pane of that alpha (the map's pictures keep partial alpha).
- **Wheels of the model that turn** (`"kola_v_modelu": [{"uzel", "stred", "polomer_m", "rizeni", "max_uhel"}]`):
  in the view from the eyes each turns about its middle by the way gone over its radius (`ThreeDEyeVehicle::rolled`),
  the steering ones steer by how the way bends (the angle whose tangent is the wheelbase times the curvature, at most
  `max_uhel`); on the map they stand as they are.
- **The cab of the model** (`"kabina"`): the view from the cab has its eye there, in the driver's head, the model
  drawn round it -- the windows' holes or glass, the passenger -- without the driver (the figure of the crew sitting
  under the eye, `PART_DRIVER`), the near plane a few centimetres off; the eye turns as the model faces.
- **The first texture is the file's first material**: the paint is for texture 0; a vehicle's nodes were read last
  first, and the V3S's glass came first.
- The girls drawn bigger than they are (`meritko`) stay that big in the view from the eyes too: the benches of the
  students' V3S are built for girls twice as big, on purpose (the player: never shrink them).
- **A state of layers** (16. 10., the colleague's `hra/3d/PROSBA-SPECIAL-3D.md`, the special's hand cart and
  wheelbarrow `SPECIAL_3D`, `MAX5`): a state may name its layer of the set's sprites (`"vrstva": n`, 0 if not). The
  game asks the set for each layer as it does to draw it (variable 10 bits 8 to 15 the layer, a next one while
  register 0x100 has bit 31, `GetVehicleSetSpriteLocalIDs()`) and takes for each the state of that layer whose range
  holds the first sprite of the set it chooses. The states of the layers make one (`ComposeState()`, kept, its step
  after those of `stavy`): the keys a later layer gives replace an earlier one's (`barva`, `lak`, `rez`, `otoceni`,
  `posadka`), `doplnky` and `barvy_doplnku` each add-on by itself. The wheelbarrow: layer 0 the crew and the load,
  1 the body's colour, 2 the frame's -- 12 x 12 colours without a state for each. Rig: `test3d stav <n> <n> ...`
  draws the states of the layers made one, `test3d stavy` prints each layer's sprite and state.
- **The whole object turned** (`"otoceni"` of a state, degrees about z at its origin): the cart pushed is the cart
  pulled turned round, its load and its crew with it. On the map, in the turned view, in the view from the eyes (its
  lights, the eye of its cab); its wheels turn by the way gone along its own x, the other way round when turned back.
- **The crew that walks** (a figure of the crew whose `animace` has `chuze` `krok_m`): not of the vehicle's shape but
  drawn with it on its own (`WalkingCrew()`), each figure the state shows at its frame of the way gone: a round of
  its frames each `krok_m` as big as it is drawn, forward as it faces (one facing back as the vehicle goes walks back);
  standing, it stays in its step. The way is that of the vehicle as drawn -- each step over its foreshortening along
  it (`EyeTrail::walked`, `TrailOf()`), so the feet keep to the ground on the straight and on the diagonal alike --
  kept for every vehicle drawn from its object, on the map too. Its frames are in the key of the sprites and of the
  pictures (`WalkCode()`, eight bits a figure, of the first eight); the cart's girls pulling and helping behind each
  their own step (2.14 and 1.41 m), so the shape is not made for each frame.
- **The paint** (`"lak"`, the colleague's point 6): `maska` a grey PNG of the body's texture's layout (the texture of
  the file, or `textura`), 255 where it is paint; `cil` RRGGBB the colour it fades to, as far as the state's `lak`
  says; `barva` RRGGBB a fixed colour by number (a state's `barva` instead), the paint that colour at the texture's
  lightness, its own where the texture is `zacatek` light. Done per pixel while drawing (no texture copies).
- **Rust** (`"rez"`, the colleague's `hra/3d/PROSBA-VOZIDLA-3D.md`): a second grey mask of the body's texture's
  layout, `cil` the rust's colour; with `"postupne": {"nabeh": 0.25}` a pixel's value is the moment it starts
  rusting (1 at once, 255 at the end, 0 never), and in `nabeh` it goes from bare to all rust; without, it is how much
  it rusts. How far: the state's `rez`. Over the faded paint, per pixel while drawing.
- **Wheels of their own** (the player: "one wheel will do for an eight-wheeled
  car"): a .glb is read with all the nodes of its scene, a mesh as often as
  nodes name it (Blender's linked duplicates: one wheel in the file, at every
  place, the other side mirrored by a negative scale), with every material's
  texture (or its base colour); and `"kola"` of the index adds a wheel from a
  file of its own -- `soubor` (its centre at the origin, its axle along y, its
  outer face to +y), `polohy` (the centres in the vehicle's frame), `oba_boky`
  (each also on the other side, mirrored; by default) -- so one wheel file
  serves many vehicles. They are drawn as the rest of the object, not turning.
- **Figures and the crew** (the player: "the girls as objects"): entries
  `"druh": "postava"` of any index (`objekty/postavy/`) are figures, `jmeno`
  their name, `soubor` the .glb (the origin between the feet on the ground,
  facing +x, metres). `"posadka"` of a vehicle puts figures into it --
  `postava` (the name), `poloha` (x, y, z in the vehicle's frame), `otoceni`
  (degrees about z) -- merged into its shape, so the driver is in every sprite
  and picture of the vehicle (seen through the cut windows). At the bus stops
  with the girls the turned view draws them as figures
  (`AddThreeDFigureToViewport()`, twice their size, as the stops' sprites draw
  people): the girl in the turquoise top (`people_girl_stoji`) at the far
  shelter of both, College Girl smoking (`college_girl_kouri`) from behind at
  the near one along X, Matylda (`matylda`) on the near pavement along Y; a
  figure not there falls back to `college_girl_stoji`, then to the sprite.
  Facing north the sprites stay.
- **Figures that move** (the player: "the girls move their hands, as if
  smoking a joint"; the colleague's `hra/3d/PROSBA-POSTAVY-ANIMACE.md`):
  `"animace": {"uzly": "snimek_", "snimku": 8, "tiku": 9}` -- the frames are
  nodes of the one file named the prefix and a number, each the whole figure
  in a pose, the textures once. A node so named is read only into its own
  frame (with its children); nodes without the prefix into every frame. The
  turned view draws the frame (tick / tiku + phase) mod snimku of the game's
  tick counter (it stands still while the game is paused), the phase from the
  stop's tile, so the girls of a map do not all move at once; each frame a
  shape and pictures of its own (`ThreeDVehicleObject::GetShape(frame)`), read
  as drawn. A tile with such a figure is drawn again when the frame changes
  (a timer of the game's ticks marks it dirty; only the screen). Everywhere
  else -- the crew, the sprites -- a figure is its first frame. Always round and
  round (`smycka` is not read); vehicles' frames (`kdy`) not yet.
- **Effects** (`"druh": "efekt"`, the game's own `objekty/efekty/`; the
  player: "and then fireworks 3D, use the changes of colour of one and the
  same object, let it be all colours"): the colleague's `ohnostroj.glb`, four
  frames -- the launch (golden trails, its own material) and the three phases
  of the ball -- one material, `jiskry`, white, coloured by the game: each
  phase of each firework a colour of the palette of `barvy` (the texture
  times it), by the hash the hut's sprites pick their three colours by
  (`HutFireworkPhase()`), so the turned view's fireworks change as the
  sprites do; no normals, unlit (it glows). Drawn in the turned view over
  the middle of the hut's front tile at true size, after the tiles
  (`QueueThreeDFireworks()`, `AddThreeDEffectsToViewport()`: with the hut's own
  sprites they would be left out where the hut is drawn from its renders);
  facing north the sprites stay. A material's name is kept with its texture
  (`ThreeDVehicleShape::tex_names`).
- **People walking** (`src/three_d_walkers.cpp`; the player, 15. 10.: "Mat
  will walk about the town along the edge of the road tiles, there is the
  pavement in a town, and out of the town he will not walk; only from a door,
  and he disappears elsewhere at a door he gets to"): Mat the repairman, the
  colleague's `mat.glb`, eight frames, `"chuze": {"krok_m": 1.1}`. Each town
  within 40 tiles of the middle of the turned main view gets its Mat: out of a
  door (`"dvere"` of a house's index entry: `poloha`, `smer` out, `sirka`; the
  colleague put them on every house of the four sets, 15. 10., the ten of
  `budovy3d` where they are built, the others guessed in the middle of the
  south-east front; the files carry them as empty nodes `dvere_NN` too, not
  drawn) of a house drawn as a 3D object -- out to the edge of its plot, then onto the road
  of the town before it, or, where there is none, the nearest one within three
  tiles, straight over whatever is between (the player: "Mat is a repairman,
  he may go to the doors through a fence or whatever is in the way") -- to
  the door of another house, by the town's roads -- a plain road tile within
  the town (its outermost zone) with a pavement, paved by the game or of a road
  type towns build (CZTR's town roads; the player: "that is another road, the
  town's"), each step joined both ways (BFS) -- keeping 5.5 units from the
  middle of the road, on the side of the door he came out of, crossing at the
  end when the other door is across. In at the door, he comes out somewhere
  else 150 to 600 ticks later. His frame is by the way walked (a round of the
  frames every krok_m), his pace twice the frames' own (`WALK_PACE`; at theirs
  a tile took fifteen seconds), his size the vehicles' (1 or 1.2). Only the
  screen: the client's, moved by the game's ticks while the main view is
  turned, never saved, with the interactive random numbers. Rig: `test3d
  chodci` (the walkers and the doors of the town nearest the view), `chodci
  posli` (one out now), `chodci stred` (the view on him), `chodci domy
  <house>` (that house on up to three tiles whose next tile along +x is a road
  of the town).
- Not yet: the glass, levels of detail, the cab's eye, wheels that turn and
  bogies that swing on a curve.
- Cost: Sergej, 95 thousand triangles, takes 55 to 170 ms a direction for the
  sprite at 16x (once, while the sprite is in the cache) and 15 to 20 ms a
  picture of the turned view at 4x (once a degree and direction).
- Rig: `test3d vozidla3d` lists the objects, the engines they are and the
  sprites made; `-d sprite=3` logs each sprite made, `sprite=4` each picture.

## Round curves of rails

`src/round_track.cpp`, the setting *Round curves of rails*
(`gui.round_track_curves`, off by default, graphics page). Only how it looks,
and **only in the turned view**: the player builds facing north ("angle 0 is
for building, turning is for looking"), and there, as in every other viewport,
the curves are vanilla to the pixel.

- The rails of a tile are six pieces (two straight, four on the diagonal), all
  meeting at the midpoints of the tile edges, so a curve is only ever a corner
  of 45 degrees where a straight piece meets a diagonal one. At each corner the
  centre line is a circular arc tangent to both pieces, starting a tile at most
  before the corner (half the run to the next corner when they are close). Two
  corners turning the same way up to seven pieces of diagonal apart (straight,
  diagonal, straight; the player: "when it is four long and not two, it is
  straight") are one arc of 90 degrees: the circle nearest to the track, its
  tangent 1.5 times the way from a corner to where the straights either side
  meet, as near to the corners as to the middle of the diagonal (about a
  twelfth of that way), so it starts before the first corner on the straight;
  where the straights either side are too short for it or it would leave the
  track by more than 5 units, each corner is round alone. A longer diagonal
  stays straight; only its ends are rounded. `test3d oblouky` lays quarter
  turns with diagonals of 1, 3, 5, 7 and 9 pieces. Nothing is precomputed: the curves of
  the tiles in view are worked out from the track while drawing.
- Rounded is plain rail on flat ground without signals; a slope, a station,
  signals or a change of height end a curve.
- Switches: walking the track, the way goes straight on through a switch where
  it can, so the straight way stays straight and only the way branching off is
  round. Its arc starts on the straight way, at most the one piece before the
  switch, and that piece of the branch is drawn on over it as far as the arc
  goes there; a tile so reached is drawn by the strips too. Where two pieces
  overlap, each pixel takes the nearest piece whose track is there, so both
  are drawn. A train's piece on the straight way next to the switch is on the
  branch's arc while the piece before or after it in the train is on the
  branch. Where the track can only turn, either way, a curve ends.
- Drawing: such a tile draws only its ground (`DrawTrackBits()`), and after the
  ground of the view is composed (into the turned view's picture of the ground,
  before it is turned) each pixel near the arc takes the pixel of the picture of
  the straight track of its rail type at the same place along and across the
  arc. Which pixels of that picture are track: for a set's rail type what its
  overlay covers (so a set's rails, CZTR's too, bend as they are); for the
  game's own rails, whose picture includes the grass, the pixels not of the
  grass's colours within the band of the ballast. No catenary over a tile
  whose pieces are all round; a switch keeps it. A tile's track so drawn is
  kept (`StripPatchOf()`), by the shape of its pieces about its north corner,
  its rail type's picture and the zoom, and copied after: worked out pixel by
  pixel at every drawing, a train on a round curve, redrawing the tiles about
  it every tick, took most of the time of the turned view (the player: "the
  picture jerks and goes slowly as 4 km/h"; rig, 700 ticks of Sergej through
  the curve: 28.7 s, the strips 20.6 s of it; kept, 7.0 s and 0.3 s; without
  round curves 5.1 s). Rig: `test3d casy` says where the time went.
- A train's piece on the arc is its 3D likeness at the arc's point, turned
  along its tangent (`GetRoundTrackPose()`). Building, the pathfinder and the
  trains' movement are untouched.

## The view from the first person

The player (15. 10.), after looking at OpenMW: "let's make Morrowind, the
inventory, the first person view", "don't forget anything, the lights". The
map menu has *View from the first person* (`first_person_gui.cpp`; the help
menu too, right after the night, the player: "put it beside the night and day
switch"): a walker stands where the main view looks (on the nearest ground
it can stand on) and the world is drawn from its eyes, 1.55 m over the ground, as from a cab
(`DrawEyeView()` in `viewport.cpp`, the cab view's renderer with the eye of a
vehicle or of the walker; `DrawCabView()` and `DrawFirstPersonView()` call
it). W and S or the arrows up and down walk (1.6 m/s; Shift or *Run* 6.5 m/s),
A and D step aside, the arrows left and right turn, the right mouse button
held looks about -- up and down the horizon moves, the picture sheared as in
the old shooters. The keys are read as held: the video drivers keep W, A, S
and D as they keep the arrows (`_walkkeys`), and while the window has the
focus the arrows walk instead of moving the map. The walker does not go into
houses, industries' buildings (the fields are walked), stations' buildings
(platforms and stops are walked), objects or water; along a wall it slides.
It steps 25 times a second of real time whatever the frames: a frame of the
game longer than a step (the picture itself takes 30 to 200 ms) takes all the
steps missed, each against the walls, and one picture after them, at most a
second of them (`FirstPersonWindow::Step()`; the player, 16. 10.: "walking
is slow" -- one step a frame had walked it as many times slower as a frame
was longer than a step). At night the picture is darkened as the map is and the vehicles' lights are
glows hidden behind what stands in front of them; from a cab, the pool of the
own headlights lies on the way ahead (`DrawEyeNight()`). Only the picture,
nothing saved. Rig: `test3d chodec`, `test3d chodec krok <n> <ahead> <aside>
<turn> [pitch]`, `test3d chodec drzeni <frame ms> <frames>` (W held so many
frames so long each, the metres walked).

**What stands from a model, by one rule** (16. 10., the player: "the car has
the 45 degrees of the sprites although it is an object, it drives oddly; the 45
of the sprites are for where we draw from 2D"; "the dragon too from near, and
save when they are far"; "it is the same principle"). Everything of the view
from the eyes drawn from a model -- the girls, Mat, the dragons, the vehicles
drawn from their objects, the cars of the game's own -- is a figure
(`CabFigure`): as big on the picture as `_cab_solid_pixels` (28) or bigger
(its longest side), the model itself by its triangles at the angle it really
stands at; smaller, a picture of it at about that angle (a picture of the map's
camera, 30 degrees from above: near it would show the roof), farther than 14
tiles at half the detail. Drawn by its triangles it is as detailed as it is
big: `ThreeDVehicleShape::Simpler()` joins its corners in cells of 2, 4, ... 64
cm, about two pixels apart (`CAB_DETAIL_PIXELS`), each cell only corners facing
much the same way and of the same texture -- the Volga of 110 000 triangles is
48 000 at 4 cm and 25 000 at 8 cm, and two of them near the walker cost 27 ms a
picture instead of 52 -- and its textures at the level of a texel to a pixel.
The plants of the fields keep their own rule (hundreds of them; only those at
the walker's feet by their triangles).

- **The vehicles drawn from their objects** (`GetThreeDEyeVehicle()`): not
  their sprites of eight directions but the object, facing the way it goes,
  finer than the eight directions -- an object over several pieces from its
  last piece to its first (a body on its bogies), one of a single piece as its
  way goes (`EyeFacing()`: from where it was 3 units back, turning to it by
  half a tick, never more than 60 degrees off its direction), hung from the
  middle of its chain as on the map, foreshortened along as the game spaces
  its vehicles (`SqueezeAt()`: as the map, 0.71 along x or y, whole on the
  diagonal), its state's paint, faded and rusted, painted into its texture
  (`GetThreeDEyeVehicleTexture()`, `ObjectPaintOfState` shared with
  `RasterObject()`). At night the glass of its lights that are on is drawn over
  the dark (`DrawEyeVehicleGlass()`) and the lights and pools of its model's
  lights go with it (`GetThreeDEyeObjectLights()`). So also in the view from a
  cab.
- **Mat and the cars of the game's own** (the player: "Mat was not seen at
  all, I came out beside him"): they were only on the turned map, and lived
  only while the main view was turned. Now they live about the walker of the
  view from the first person while it is out (`LifeMiddle()`) and are drawn in
  the view from the eyes (`AddThreeDWalkersToEye()`, `AddThreeDDriversToEye()`),
  Mat at his true size, the cars at the vehicles' size.
- The posed shapes of the creatures (`MakePosedShape()`) know their extent.

Rig: `test3d auta <jmeno> [pocet] [x y] zatacka [naklad] [plny] [v1:1 ...]` (a
road with a bend to drive through; the sheds are built before the roads, else a
long road leaves their ways off; refitted to a cargo's label, filled with it,
options of the configurator chosen in the shed), `test3d plnost <px>`, `test3d detail <px> [textura]` (0 the whole
shape; with `textura` the textures at their full size), `test3d vozidla`
(also the speed, the order and the state of a road vehicle).

## The night

The player (15. 10.): "where the toggle console is there will be a dark mode
too, and it will make night, as if it were night, and the cars will shine".
The help menu has **Night** right under *Toggle console*, ticked when on; it
is the setting `gui.night` (Graphics, kept in the config, never in a save).
Only the picture changes: every block of the map drawn (facing north or
turned) is darkened to the moonlight -- red, green and blue to 70, 82 and 128
256ths (`DrawNight()`, the blitter's `DarkenRect()`; the 32bpp-anim blitter
darkens the brightness of the palette's animated pixels too, or the water
would light up at its next step) -- before the signs, which stay as they are.
Then the vehicles' lights are added: two white headlights at the front of a
road vehicle, a train's engine or a ship with the pool of their light on the
way ahead, two red lights at the back of its last part, an aircraft's landing
light and its red beacon blinking. At 32bpp only; the 8bpp blitter keeps the
day.

- **How light the night is** (`gui.night_light`, Settings, Graphics, under
  Night; the player, 16. 10.: "a nice dark, but sometimes too much; not only a
  bright full moon but nearly the dawn too"): dark night (70, 82, 128 of 256,
  as it was), moonlight, full moon, bright full moon, nearly the dawn (188,
  180, 200, a little warm) -- `GetNightLight()`, on the map and in the views
  from the eyes and the cab; the lighter, the fewer stars. The night itself
  stays switched in the help menu.

- **The lights marked in the model** (16. 10., the colleague's
  `hra/3d/PROSBA-SVETLA-VOZIDEL-UZLY.md`; the player: "the cars must have
  their lights marked as the houses their windows; about the car there are
  lights a metre away"): a vehicle drawn from its object whose `.glb` has the
  node `noc` -- the glass of its lights, materials `svetlo_predni` (warm
  white) and `svetlo_zadni` (red) -- and the empty nodes `svetlo_predni_NN`,
  `svetlo_zadni_NN` in the middle of its lamps. By day `noc` is left out of
  its shape (`ReadGlbMesh()`'s `skip_node`). At night its picture has the
  glass of the lights that are on drawn over the body unshaded in its
  material's colour, through the depth, its pixels marked lit and kept bright
  by the night (`ThreeDModelPicture::lit`, `BlitTurnedPicture()`); the
  headlights when it leads, the red ones when nothing follows it, none for
  one turned round (`VehicleObjectLightsOn()`). The glows shine from the
  places in the model as the picture is drawn (`GetThreeDVehicleObjectLights()`,
  the picture's `lights`), a lamp only where its glass is seen; the pool on
  the ground before the lamps. Facing north such a vehicle is drawn at night
  from its picture too, not its sprite (`ThreeDVehicleObjectLitAtNight()`),
  in the same place. The cars of the game's own (the Tatra VB) the same way
  (`ForEachThreeDDriverLight()`); in the view from the eyes the glows from the
  model's places in the world (`GetThreeDVehicleObjectWorldLights()`). A
  vehicle without them as before. Rig: `test3d svetla`, `test3d auta3d stred`.
- **Cheaper up close** (the player, 16. 10.: "make the night lights up close
  cheaper"): a vehicle's box grew a tile and a half on every side at night,
  every part of every train, and the turned view drew all that again at each
  step. Now only the front and the last part grow, by the glow of a lamp
  (`Vehicle::UpdateBoundingBoxCoordinates()`), and the pool of the headlights
  is marked by itself where it is, where it was and where it is now, as a box
  of the world (`MarkVehicleLightPoolDirty()`). Rig, 1000x700 at the closest
  zoom, turned 30 degrees, a train of 20 wagons and two cars: by day 15.9 ms a
  frame and 145 000 pixels drawn; at night 28.7 ms and 298 000 before, 20.0 ms
  and 189 000 now; no ghosts.
- **The title screen's day and night** (the player, 16. 10.: "a long day and
  then a while of night, round and round"): behind the main menu a minute of
  day, four seconds of dusk, fifteen of night and four of dawn, again and
  again (`SelectGameWindow::AdvanceDayAndNight()`, `TITLE_DAY_MS` ...). It is
  the title's own night, `_title_night` from 0 to 1, not the player's: on the
  title screen `IsNightNow()` asks it instead of `gui.night`, and in the dusk
  and the dawn `GetNightLight()` goes from the day's light to the night's as
  set (`gui.night_light`). Drawn anew only when its step of 1/32 changes.

## The title game

The title game of 16.0-beta2 (`media/baseset/opntitle.dat`) is arctic, that of
15.3 the toyland; the player (16. 10.): "we put temperate there", "make it
yourself, with the camera". It is a new temperate game of 256 by 256, 1960,
flat land, Czech town names, no breakdowns, no disasters, no growth of towns
(so no road across the track), made by the rig: `scena`-like settings and
`test3d uvodnihra`, then `test3d tiky 1500` and `save`. The command finds the
place around the towns where an octagon of track (straights of 18 and 8, its
corners diagonals of 5 pieces) lies on clear land or trees only, with the most
houses near it and the fewest slopes -- seed 7: round the town Kamenné
Pražiště -- builds it, a station of four in the middle of each long straight,
semaphores every twelfth piece, and from a depot inside, joined for the while
by a piece of curve, the fastest steam engine with three carriages and the
fastest diesel with four coal wagons, each out on its way before the next;
then the depot goes. Buses (`autobusmesto`) in that town and in the biggest
other. The camera's signs (`T<n> P 12`): round the octagon, from the right of
it, below, the left and above, panning twelve seconds each, so that the town
and the trains are seen beside the menu, which hides the middle of the
screen. Only the game's own vehicles and graphics: no GRF is in the save.
In 20 000 ticks no vehicle came to grief. Rig: `test3d titulnoc <ms>` on the
title screen (from `autoexec.scr`) moves its camera and its day and night on.

## The previews that turn

`src/three_d_preview_gui.cpp`. There is no window: the preview is a picture
on a see-through background, drawn over the map beside the window it belongs
to (the player: a window and buttons are not needed, it starts turning on a
transparent background). The main window draws it after its viewport, a timer
turns it and marks the screen under it dirty, and it goes when its window
goes; any window over it is drawn over it, as over the map. Each preview
turns about its own axis, the middle of a vehicle or the middle of a
building's plot: a frame is found once, the rectangle its pictures at every
angle fit in about the axis, the frame is placed beside the window and stays,
and every picture is drawn at the axis within it (the player: the turning was
off its axis). Beside the **purchase window** it is the vehicle chosen there,
in the company's colours, as its **3D likeness made from its eight sprites**
(below), but in the eight directions of its sprites **the set's own
pictures** (the player: "use the pictures you have, the drawn ones only where
you have not got that angle"): it stays 1.5 s in each of them and turns to the
next at 60 degrees a second, the likeness only while it turns, foreshortened
on the track as the map draws it so that the pictures fit the likeness. The
eight views the likeness is made from are kept with it for this, at 4x
(`ThreeDVehicleModel::views`); the turned map likewise draws a vehicle's own
sprites when the view is turned by a multiple of 45 degrees, the likeness
only at the angles between. A vehicle drawn from its **3D object** has no
pictures of a set to show: it turns on evenly, never stopping, and is drawn
from the object itself at the angle, as the turned map draws it (the player:
"the Sergej object stops in the sprites' directions too, a pause added for
nothing"). On the map nothing stops in time: the turned view draws a
vehicle's own sprites only at a view turned by a multiple of 45 degrees, and
a vehicle on a round curve always at its angle. A set whose chain gives no picture for some
direction gets the old preview, the eight sprites one after another. Beside
the **window of one of the game's own industries** drawn as a 3D object
(the school, the machine, the statue, the hut) and of the plantation it is
the building, turning smoothly, drawn from its object (the plantation from the
parts of the fields, `RenderThreeDObjectsPicture()`) the way the map draws
it, starting from the direction the map view is turned to. The size: four
times the normal for what fits 360 pixels, else twice, else the normal (the
player: a long wagon at 2x, not 4x). Off (the setting), neither appears. The
icons of the vehicles facing any direction are `GetTrainIcon()`,
`GetRoadVehIcon()`, `GetShipIcon()`, `GetAircraftIcon()` (`engine_gui.h`); the
purchase list goes on using the ones facing west.

### The 3D likeness of a vehicle from its eight sprites

`src/three_d_vehicle.cpp` (`ThreeDVehicleModel`). The buildings come with
renders that carry depth; a vehicle has only its eight sprites, so its depth
is made from the eight outlines, the way it was tried outside the game on
Sergej and on figures and settled with the player (the depth from the
colleague is the luxury on top; without it everything with eight pictures
turns this way):

1. **The eight views**: the sprites of every part of the vehicle (the
   articulated parts, the two heads of a double-headed engine) for a
   direction, laid together as the map lays them (`Train::UpdateDeltaXY()`,
   `Train::CalcNextVehicleOffset()`, the road vehicles', ships' and aircraft's
   own boxes), at the finest zoom the sprites have, in the company's colours
   (the recolouring the blitter would do). The anchor is the track point
   under the middle of the chain. A sprite of 4x4 or less is the empty one a
   set gives a direction a part has no picture of (Sergej's head and tail in
   the directions along the diagonal track, where the middle part draws the
   whole engine).
2. **The camera of the sprites**: orthographic, 30 degrees above the horizon,
   48.8 pixels to a metre at 16x, the game's own projection as the sets of
   this game render it; direction d has the vehicle turned `90 - 45 d`
   degrees, and in the four directions along the track its sprite is
   foreshortened by `1/sqrt 2`, as the sets draw vehicles.
3. **The visual hull**: a grid of voxels, about a pixel each (at most about
   six million), cut away wherever a voxel falls outside the outline (alpha
   40 and more) in any view. The sets are drawn by hand and a sprite may sit
   a few pixels off the others (the player's set has its N and S pictures 16
   pixels to the right of the rest at 16x), so the first hull is cut with the
   outlines widened by 16 pixels, each view's anchor is moved to where the
   middle of that hull's outline meets the middle of the sprite's, and the
   hull is cut again and the anchors refined by a pixel or two to where the
   outline covers the sprite's best. Then an opening of a voxel takes the
   specks off.
4. **The surface** of the hull, each point with the colour the sprite gives
   it in every view that sees it (a z-buffer), weighted by how squarely the
   camera looks at it; a point no view sees takes its neighbours' colour.
5. **A picture at any angle**: the points, turned, drawn as 2x2 dots with a
   z-buffer at twice the size and shrunk with their alpha, so the outline is
   soft as a sprite's; the colour of a point is a mix of the two sprites
   nearest the angle, so in a sprite's own direction it is the sprite's
   pixels, and between two there is no jump.

Sergej (13/8 tiles, three parts, zin16): a million voxels, 70 thousand
points, under two seconds to build, a few milliseconds a picture; the hull's
outline covers the sprites' at 0.93 to 0.98. The last four models are kept
(`GetThreeDVehicleModel()`); the sprites reloaded, they go.

## The view from the cab

`src/cab_view.h`, `cab_view_gui.cpp`, the drawing in `viewport.cpp`
(`DrawCabView()`). The player (13. 10.) saw a video of someone's OpenTTD with
a cab view and wanted the same, for a train, a road vehicle, an aircraft and a
ship: the button with the magnifying glass in the vehicle's window (the player
drives from the phone, so a button) opens **"Cab view: <vehicle>"**, the world
ahead of the vehicle in perspective, drawn anew about twenty-five times a
second while the window is open, with a panel under it: a gauge and the speed
in figures, the top speed, the next stop (the station, the depot, none), what
the vehicle is doing (running, stopped, loading, broken down) and how full it
is; buttons to the vehicle's window, to put the map on it, **Brake -** and
**Throttle +**, and Stop. The brake holds the vehicle to a speed a step (a
tenth of its top speed) below where it is; the throttle raises that; past the
top speed the vehicle goes as its orders say again. The speed it is held to is
`Vehicle::cab_speed_limit`, set by the command `SetCabSpeedLimit` (so a
network game stays in step), in the units the vehicle shows, and every kind
of vehicle minds it where it works out its speed (`Train::GetCurrentMaxSpeed()`,
`RoadVehicle::GetCurrentMaxSpeed()`, `Ship::GetCurrentMaxSpeed()` and
`ShipAccelerate()`, `UpdateAircraftSpeed()`); it is saved (below).

**A ship can be steered** (the player: "I want to drive the ship left and
right", and it would not leave its dock without orders; then "turning gently
by small angles"): **< Left** and **Right >** drive it freely, a small angle a
press (below), and a ship steered so leaves its shed for it without orders,
or with an order that names that very shed, the way out nearer its heading.
The first way of it, a heading in eighths of a turn taken on every tile as
the track nearest to it (`Vehicle::cab_heading`, the command `SetCabHeading`,
`ChooseShipTrack()`), is still there for the rig (`test3d smer`).

**A road vehicle can be steered too** (the player: "press right on a car and
it turns right at the next junction"): **< Left** and **Right >** ask for the
turn it takes at the next junction, like a blinker -- the other one takes the
asked turn back to straight on. It is `Vehicle::cab_turn` (a `DirDiff`, the
same command `SetCabHeading`): `RoadFindPathToDest()` takes the road
of that turn on the next tile with a choice (a bend is no junction: the turn
waits for one), and straight on from there; where the asked way is not to be
had it goes straight on and keeps the turn for the next junction, and where
straight on is not to be had either, round the corner on the side the
vehicles keep to. A steered road vehicle leaves its depot even when its order
is that very depot. The panel says "by hand: next right / next left /
straight".

**Driving freely** (`cab_drive.h`; the player: "can a car leave the road?
we are 3D, drive in every direction ... trees it drives through, at houses it
stops ... the ship too, turning gently by small angles"). **Off road** puts a
road vehicle off the road: from there **< Left** and **Right >** turn its
heading by small angles (an eleventh-and-a-quarter degrees a press), and it
drives straight along it across the land at half its speed (and none of the
slowing for a corner, there being no corners) -- over grass,
fields, rough land and through trees, across roads and level crossings --
and stands where it may not go: a house or any building, water, rails but at
a crossing, a steep slope, a bridge's end, a tunnel, a depot, the map's edge;
it turns there and goes on. A ship goes freely with its first press of left
or right, on open water and past buoys, and stands at a bank, a lock, a dock.
Road vehicles but trams, not in a depot, a tunnel or a bay; a lorry's
trailers come after it (the player: "let the trailers off the road too"):
every piece is driven freely, each sitting the length of the piece before it
behind that piece on the line to it and pointing at it (`RoadVehPullChain()`,
whole numbers again), so the trailer swings round behind the lorry as a
towed one does, and nothing of the ground stops a trailer. Back on the road
the trailers are laid behind the lorry along the road it came onto
(`RoadVehLayChainBehind()`, the same as behind a road stop a lorry is put
at). The heading is `Vehicle::cab_yaw` in 128ths of a turn and the
position `cab_px`, `cab_py` in 256ths of a unit, moved a unit along the
heading for every step the speed gives, in whole numbers from a table of
cosines (`CabCos()`), so every computer of a network game moves it alike; the
command is `CabDrive` (`CmdCabDrive()`). Its orders wait meanwhile (no
loading, no servicing). **As ordered** sends it back: it drives on until it
comes onto a road by a road's end -- a ship onto water with a course across
it -- and goes on from there into the lane, or along the course, its orders
choose (`RoadVehRejoinRoad()`, `ShipRejoinCourse()`). The panel says "by
hand: free" and "back to the orders". Closing the window asks it back too;
a road vehicle out in the fields then stops, not to wander off without anyone
at the wheel.

**Saved.** The cab's steering moves the vehicle, so it is the game's state:
the speed held, the heading, the turn, the free drive (savegame version
`CabDriving`), and a game loaded or joined over the network goes on alike. A
road vehicle off the road loads where it stands; the road type under it is
its own there (`RoadVehicle::GetRoadTypeUnder()`, for the NewGRF variables and
the road's speed limit).

**Back to the orders.** **As ordered** takes the steering off: the ship's or
the road vehicle's cached path is cleared and it finds its way to its order
anew. Closing the cab window does the same: whoever steers from the cab lets
go when leaving it. Trains and aircraft follow their rails and routes: for
them the three buttons are greyed out. The button beside the pin puts the
window over the whole screen and back (the player drives from the phone).

The picture is made by the same means as the turned map view, with a
perspective rasterizer and a depth buffer in place of the turned projection:

- **The eye** is at the front of the vehicle, a few units ahead of its
  position and above it (a train's and a ship's 9 units of height, a road
  vehicle's 7, an aircraft's 14), looking along its direction; it turns
  towards a new direction over a few frames, so a curve is a swing, not a
  jump. Under a low bridge -- its deck only a level over the road or the
  rails -- the eye bows to pass under the deck, and rises again behind it. True 3D is
  taken from the game's projection: a tile is 16 units square, a unit of
  height (z) is 0.408 of a unit of x and y (a height level of 8 stands 3.27
  units tall), a pixel of a sprite at the normal zoom is 0.354 units across
  and 0.408 up. The horizontal field of view is 90 degrees.
- **The ground.** The tiles ahead within 20 tiles and the field of view are
  asked what they draw, as the turned view asks them; their ground sprites
  (the rails, the roads, the platforms that are ground) go north-up into one
  texture at the normal zoom, and every tile is laid as four triangles meeting
  in its middle, each corner at its height, with the texture pixel of the
  world point -- in perspective, 1/depth and the texture pixel over depth
  changing linearly across the picture (`CabRasterTriangle()`), each pixel
  the texture's nearest one: sharp pixels as the game's sprites have them
  (the player: smooth textures would look like the other cab view, not
  ours). The black under a building is grown over as in the turned view. Where a road or rail is levelled on a slope, its ground lies
  on the foundation as the foundation's children: drawn into the texture
  too, at the foundation's top, with the foundation's sides as walls of its
  sprite.
- **Bridges** are built from the sprites the game draws for them
  (`DrawBridgeMiddle()` tells the cab view which they are,
  `MarkCabBridgeSprites()`): over every tile the deck at the bridge's height,
  dressed in the floor sprite (the track or the road on it), the far railing
  standing on the deck's far edge in the same sprite, the near railing in the
  sprite of the near side, the deck's edges and its underside in the shadow,
  and the pillars as columns where the sprite's pixels are
  (`CabDrawBridgeMiddle()`); a ramp is a deck rising from the ground to the
  bridge (level on a foundation), its sides down to the ground and its end
  under the bridge (`CabDrawBridgeRamp()`). "Dressed" is the boxes' way: a
  point of the surface takes the pixel where the north-up view draws it. A
  vehicle on a bridge looks from the deck; one under it sees the underside
  and the girders.
- **Tunnels.** A tunnel's end is level, with the track or the road straight
  on it, instead of the slope with the portal painted on it
  (`CabTunnelGround()`); on its edge where the hill starts stands a portal of
  stones round an opening as tall as the tunnel, with walls of stones beside
  the way in up to the slope that was there (`CabDrawPortal()`). Seen from
  outside, the tunnel shows only through the opening: its tube is drawn into
  a picture of its own and put where the opening is seen, as the opening's
  plane (`CabDrawTunnelSeenIn()`), so a hill a level high -- lower than the
  vault -- never shows in it, nor the vault out of the hill. With the eye in
  the tunnel the tube is drawn round it and the hill over it left out: walls
  and vault in pieces, lit from the ends and dark inside, lamps along the
  walls, the rails or the line of the road on the floor, the daylight of the
  other end, and the vehicles in the same tunnel (`CabDrawTube()`).
- **Everything standing** is drawn far to near with the depth test: what
  stands on a house, industry, object or station tile with a footprint is a
  **box** of its sprite (`CabDrawBox()`: the sprite's pixels over the top and
  the two walls it shows, mirrored over the two it does not, as the turned
  view's boxes), a platform a low box; the rest -- a tree, a signal, a lamp,
  the other vehicles -- a **billboard** of its sprite facing the eye, sized as
  the sprite is on the map, its foot at the middle of its bounding box. The
  other vehicles show the picture of their direction as seen from the eye (as
  the turned map shows them); the vehicle's own consist is left out. A line
  of a sprite (a fence seen along its length, a wire) and a fence piece are
  left out too: they would stand as tall posts.
- **The catenary and the fences** along the rails, of fields, farms,
  industries and stations are not drawn in the 3D views at all, the cab view
  and the turned map (the player: "turn them off in these 3D views"): they
  are lines of the north-up view and stand about as posts and ribbons. The
  cab view collects with the catenary made invisible, the rail fences off and
  `_draw_fences` (clear_cmd.cpp) off; the turned view the same, and a thin
  long low box of any tile (a fence of a farm or a station) is skipped.
- **The sky** is a gradient down to the horizon in the middle of the
  picture, the far ground a plain green under it; what is drawn fades into
  the haze colour from 6 tiles out to the edge of reach (not in a tunnel).
  Hidden elsewhere than in a tunnel (in a depot) the picture is dark with
  the light of the door.
- A picture of 450x300 takes about 4 ms (427 tiles, 400 billboards); the
  sprites' textures are kept between frames (`_cab_textures`, up to 4096).

What is not there yet: the catenary as wires of their own, the own vehicle's
nose or cab, finer ground near the eye (the normal zoom's pixels, big up
close), the buildings with eight renders and the
new 3D objects (billboards and boxes for now). A bridge's thin lines (the
cables of a suspension bridge) stand as rows of the sprite's pixels.

## The rig

`test3d` in the console, under the null video driver with `blit=` (so the
screen is a buffer drawn for real): `test3d uhel <degrees>` turns the main
view, `test3d stred <x> <y>` puts a tile under the middle, `test3d zoom
<level>` sets the zoom, `test3d vozidlo [n]` puts the n-th visible vehicle
under the middle (`test3d vozidla` lists them all, with the tile under each
and its type -- 1 plain rail, 5 a station -- its pieces, whether it is
stopped or coupled turned round), `test3d zatacka <x> <y> [name]` builds
the curve scene -- a shed, four tiles of straight track, a quarter turn
and five more, levelled first, on the first flat clear square of the map
when x and y are 0 -- and sends an articulated engine along it (the first
there is, or the one whose name has the text; the curve under the middle of
the view), `test3d kolej <x> <y> <track 0..5>` lays one piece of rail (X,
Y, upper, lower, left, right) to build other curves for the round curves of
rails, `test3d oblouky` levels clear land and lays quarter turns with 1 to 9
pieces of diagonal side by side, `test3d motory [text]` lists the rail engines with their pieces
and whether the company may build them, `test3d kabina [n]` opens the cab
view of the n-th visible vehicle (`test3d kabina cas` says how long its last
picture took and what was in it; with `-d misc=5` the tall things, the boxes
and the walls drawn are listed, with `misc=6` the picture and the sprites'
textures are written as `cab_picture.ppm` and `cab_tex_<sprite>.ppm`),
`test3d trat [bridge type]` builds the cab view's railway -- a shed, a
bridge over a dip (of the type asked, else the first that can be built), a
tunnel through a hill a level high, a road under the bridge where it is a
level low with a bus on it -- and sends an engine along it, `test3d silnice`
a grid of roads with junctions and a bus (with `-d misc=5` a steered road
vehicle says the turn it takes at each junction; with `misc=6` the cab
view's depth is written as `cab_depth.pgm` too, with `misc=7` its ground
texture as `cab_ground.ppm`),
`test3d jed <type> <unit>` starts a vehicle that stands in its shed (type 0
train, 1 road, 2 ship, 3 aircraft; with `testletadlo <x> <y>` after a
company exists, an aircraft in the air; with `testlod`, a ship in a shed),
`test3d smer <type> <unit> <direction 0..7 | 255>` steers a vehicle from its
cab as the window's buttons do (a road vehicle takes the turn instead: 0
straight on, 2 right, 6 left), `test3d objekty` lists the buildings that are 3D objects as read (their
house, variants, triangles, height), `test3d mesto [n]` puts the n-th town
under the middle of the view, `test3d autobusmesto [n]` lets a bus loose in
it from a depot built beside its road (for the cab view among the houses),
`test3d jizda <type> <unit> <op> [times]`
drives it freely (op 0 off the road, 1 a step left, 2 a step right, 3 back to
the orders, 4 stop or start it; with `-d misc=5` it says where it is back on
the road or its course; `test3d vozidla` shows a free vehicle's heading) (with `-d misc=5` the ship says why it stays
in its shed or that it leaves), `test3d snimek <name>` writes
the screen to `screenshot/<name>.png`, `test3d mys <x> <y>` says which tile
the game finds under a pixel, `test3d cas [n]` times n redraws of the whole
screen and says where the time went, `test3d ladeni <x> <y>` writes out how
that tile is drawn (its sprites, walls, corners) and its texture as
`textura.ppm`. For the game's own industries drawn as 3D objects: `test3d objekty`
lists every object read, `test3d budovy` says where such industries stand on the
map, `test3d postav <x> <y> <object>` puts one there by its object's name
(gymnazium, socha_kamen, socha_bronz, automat, chatka; it must be in a town), `test3d plantaz <x> <y> <care> [holky]`
sets a plantation's stage. For the previews:
`test3d okno nakup <auto|vlak|lod|letadlo>` opens the purchase window (with a
company made for it, since the null driver's game starts as a spectator),
`test3d okno prumysl <x> <y>` the window of the industry on that tile,
`test3d motor <name>` chooses the engine whose name has the text in the
purchase window, `test3d nahledy` describes the previews (where, the angle,
the vehicle's likeness: its fit to each sprite), `test3d nahled uhel
<degrees>` turns them to an angle (the frames of a turn; with `-d misc=5` the
likeness writes its eight views with the hull's outline over them as
`three_d_view_<engine>_<d>.ppm`, with `misc=6` every picture drawn as
`three_d_render_<engine>_<angle>.ppm`), `test3d okna` lists every window, and `test3d snimek <name> okna` writes the
whole screen with the windows (the plain `snimek` is the viewport alone; the
world generation still counts as in progress at the first tick, which would
leave every other window undrawn, so the rig calls it over). `test3d snimek
<name> obrazovka` writes the screen as it stands, only the blocks marked
dirty since drawn, which is what the player sees: with `testpauza` and
`testzatik <ticks> test3d snimek ...` it shows whether a vehicle that moved
was repainted where the turned view draws it (an `obrazovka` snapshot and a
plain one at the same tick must be the same picture). Screenshots asked for under the null driver are made on the
spot (`MakeScreenshot()`), since that driver never drains the main-thread
queue.

A 1600x1200 view of a town at the normal zoom, turned: 44 ms a redraw of the
whole screen (the first one longer, loading sprites), of which the block's
texture 10, laying the tiles 28, the billboards and boxes 6; 1000x700 about
16 ms, facing north 2 ms. Only dirty blocks are redrawn as always, so this is
the cost of turning and scrolling, not of vehicles moving.

## The graphics of the 3D era

openttd.grf stays as it is (the player's rule). The 16x pictures go into sets
of their own under `media/baseset/decouple/grafika/` (`zastavka.grf` first,
the girls at the bus stop; `zastavka/README.md` there), static in every game,
laid over openttd.grf's sprites with Action A: a set of the game's own
graphics may replace any sprite loaded so far, openttd.grf's included
(`SpriteReplace()` in `src/newgrf/newgrf_acta.cpp`, `GRFConfig::builtin_graphics`).

The renders of the buildings that turned were the sets `budovy3d_<set>_V<version>.grf`
there (all gone since 15. 10., the buildings are 3D objects; the block of sprites stays for the game's own sets), each under 100 MB, made by `grafika/openttd_3d_grf.py` from the
colleague's renders (grrrrf); a set made again gets the next version in its
name and the old one goes, so the player takes only the sets that changed
(the Windows build leaves them out; the player keeps them in the installed
game's `baseset/decouple/grafika/`). For every model and direction three sprites of 16x only,
the colour, the depth (a 32bpp picture: red the high byte, green the low
byte, alpha 255 where measured) and the normals, 24 a model. They go into a
block of the game's own, `SPR_DECOUPLE_3D_BASE` (2048 sprites before
`SPR_NEWGRFS_BASE`), through Action A with sprite numbers from 0x8000 on,
which a set of the game's own graphics may use for that block. The same
script writes the table `src/table/three_d_models.h` (which house or
industry each model is, its plot, and for every direction the camera that
took the render). The pictures themselves are not kept in the repository,
only the sets and their `.yagl`. Sets 1 to 4, the ten houses, are gone since
15. 10.: those houses are 3D objects (`objekty/mirne/`, below); the script
keeps their places in its list (`AS_OBJECTS`), so the other models' sprites
keep their numbers and sets 5 to 8 stand as they are.

## Trains of Microsoft Train Simulator (simtrains)

Each player downloads their own trains of Microsoft Train Simulator (MSTS)
and keeps them where such trains are kept. The game looks for them in:

- the `Trains/Trainset` folder of MSTS: where MSTS was installed (the
  registry of Windows), the folders Open Rails was given (its `Folders` in the
  registry), and where MSTS usually is (`C:\Program Files (x86)\Microsoft
  Games\Train Simulator`, `C:\Train Simulator`, `C:\MSTS`, `D:\MSTS` ...);
- the folders named by `simtrains_path` in `[misc]` of `openttd.cfg`
  (separated by `;`; the folder of MSTS, its `Trains` or its `Trainset`);
- `newgrf/simtrains/` of the game's folders, for a player without MSTS.

Each vehicle there (an `.eng` with its model, `WagonShape`) becomes a new
vehicle of the game:

- Before the NewGRFs are looked for (`PrepareSimtrainsGrfs()`), the game
  writes a NewGRF of one vehicle, `newgrf/simtrains/<vehicle>.grf` of the
  player's own folder, from the figures of the `.eng`: `Name`, `Type` (diesel,
  electric, steam), `MaxPower`, `MaxVelocity`, `Mass`, `MaxForce` (the
  tractive effort, at most the original engines' 0.3). Introduced in 1920 and
  never retired, every climate, the prices of the original diesel of the same
  power. Its id is `ST` and a number of the file's name, the same on every
  computer; it is written again only when it changes, and goes when its
  vehicle is no longer there. The player switches it on among the NewGRFs as
  "Simtrains: <name>".
- Wagons too (`.wag`): a carriage ("Type ( Carriage )") carries passengers,
  as many as its `PassengerCapacity`; a goods wagon any cargo but passengers,
  30 of it (MSTS says nothing of it). Priced as the original game's wagons.
  Left out: tenders, and a `.wag` whose model is that of an `.eng` of the same
  folder (the engine hauled dead, "ms").
- The vehicle is drawn from its model (`ReadSimtrainsShape()`): the `.s`,
  binary (`JINX0s1b`) or text (`JINX0s1t`, the older models: the same written
  out as names and brackets, a part named before its bracket as
  `matrix RAM ( ... )`), the nearest of its distances, every part through its matrix and those
  above it, turned into the frame of the vehicle objects (x forward, y left,
  z up); the textures `.ace`, the first of their sizes no bigger than 1024:
  uncompressed (rows of red, green, blue, a mask and alpha) or packed as DXT1,
  DXT3 or DXT5 (a table of the sizes, each its length and its blocks of four by
  four). `PassengerCabinHeadPos` is the eye of the cab view.
- Its length: the third figure of `Size` of its `Wagon` (`ReadSize()`). A
  vehicle of the game is at most eight steps of the chain, a step of diagonal
  track (a tile 14.84 m, as the vehicle objects): about 10.5 m. A longer one
  would be drawn over the next (the player, 16. 10.: "the wagon and the engine
  are pressed into each other, no gap"), so its GRF makes it articulated: the
  parts are the numbers after its own (`SIMTRAINS_ENGINE_ID` + 1 ...), in no
  climate, with nothing but their length (no power, weight or load), given by
  callback 16 (the callback mask 0x10, a varaction2 of variable 0C going to
  one of variable 10). The model is one object over all the parts, hung from
  the middle one, as long as the model makes it (the ŽSSK 750, 16.24 m: two
  parts, 6 + 7; the ČD B 598, 24.2 m: three, 6 + 7 + 6).
- Not read yet: the freight animation, lights and sounds.
- The rig: `test3d ace <file.ace> <out.ppm>` writes a texture out as the game
  reads it.

The GRF the game writes holds only the figures of the `.eng`; the model and
its textures are read from the player's own files.

### The cab of an engine of MSTS

An `.eng` names its cab, `CabView ( ... .cvf )`, from the engine's folder
`CABVIEW` (as MSTS reads it; a cab shared by several engines is in a folder
above, `../../common.cab/cab451/451.cvf`). The `.cvf` (`ReadSimtrainsCab()`)
has pictures of the driver's desk, each with its eye (`Position`, the eye of
the cab view then) and where it is seen (`Direction`: down, to the right):
the 451's ahead, 60 degrees to the left and 75 to the right. By night the
same pictures from a folder `night` (or `noc` beside `den`). Its controls
(`CabViewControls`: dials, levers, lamps, figures) are read for the next
steps. An engine whose cab is not there (the ŽSSK 750 comes without
`common.cab/cab750`) has its driver's eye by the side window its `HeadOut`
gives, drawn in inside the body to the seat, looking ahead -- not the
passenger's place of its `Inside` (`PassengerCabinHeadPos`), which looks out
to the side.

In the cab view with the camera Inside (`simtrains_cab.cpp`) the pictures are
drawn over the view: each a flat board before the eye where MSTS sees it, as
big as MSTS's screen sees (45 degrees up and down, the cab view then as wide);
they turn with the engine, not with the head (the right button). A board
further from ahead is widened to meet the one nearer, so the desk goes round
without a gap; above, below and past the last boards the colour of the edge.
The clear windows show the world, drawn by the processor or by the graphics
card under them. The window keeps which pixel of which picture each of its
pixels shows, made anew only when the head turns or the window changes.

The cab view from the train's window and the walker who boards the train
with E (`first_person_gui.cpp`) are one seat (`SimtrainsCabSeat`): the same
eye, the desk over the view, the controls and their switch at the bottom.

The rig: `test3d obloukjizda 1 0 300 53542BB7` buys the 451 (an electric
engine gets its track under the wires), `test3d kabina 0`, `test3d kabina
klik cely`, `test3d kabina hlava <dx> <dy>`; boarded with E: `test3d
chodec`, `test3d chodec u 0`, `test3d chodec e`.

### Driving a train of MSTS from its desk

The player drives it, the game does not (`Train::cab_driven`): the first
touch of a control in its cab view takes it over, the window closed (or the
button Drive / T, its route) lets it go, and the game drives it again by its
orders. Driven, it runs past every red (`force_proceed`, renewed each tick)
at the speed it is driven -- not slowed before it as a train the game drives
is (`TrainCheckIfLineEnds()`) --, no platform slows it and no order's speed
holds it; stopped by the player on
a platform of the station its order is for, it arrives there and loads.
The curves, the bridges and the depots still hold it to their speeds.

The controls (`CmdTrainCabControl()`, saved with the train): the throttle,
the electric brake, the train's brake, the engines' own brake, the emergency
brake and the reverser. `Train::CabAcceleration()` moves the train by them
as forces: the pull as the game counts it (the power at the speed, no more
than the tractive effort, by the throttle), less the way's resistance, less
the brakes -- full service 0.9 m/s2 of the train, the emergency brake
1.3 m/s2 and the power off, the engines' own brake on their weight, the
electric brake three quarters of the motors' pull, fading out below 8 km/h.
No delay of the air brake (the player: "it is a few metres").

At the desk (`SimtrainsCabDriver`, a switch at the bottom of the cab view,
`gui.cab_msts_keys`): the arrows move one combined lever -- up the notches of
the `.eng`'s throttle, down from nought the electric brake (three steps, if
the engine has one) and then the train's brake in five steps, held down at
the end the emergency brake; or the keys of MSTS: D/A the throttle, '/; the
brake, ]/[ the engines' brake, ./, the electric brake, W/S the reverser,
Backspace the emergency brake, Space the horn. The panel under the view says
where the controls stand.

The rig: `test3d kabina pult <rizen|vykon|edb|brzda|primocinna|rychlobrzda|smer>
<0-255>`, `test3d kabina zavri`; `test3d kabina cas` shows the desk. The 451
alone: full service from 65 km/h stands within the tile, the emergency brake
from 99 km/h within two; 0 to 99 km/h in three tiles. Past the red of
`testnedobrzdil blok` (the train behind taken over, full power): through at
64 km/h, into the train standing on the platform beyond.

The rig: `test3d obloukjizda <n> <wagons> <ticks> <grfid> [<wagon grfid>]`
buys the engine of that GRF (`53542680`, the ŽSSK 750 031) and the wagons of
the other (`53544EE5`, the ČD B 598).

## Cars of BeamNG.drive mods (beamng)

The player (16. 10.): "shall we drive mods of BeamNG?", sending a mod, the GAZ
Volga 24 (made in Automation). As the trains of MSTS, each player downloads
their own mods; they are read where the player has them, straight from their
`.zip` (`beamng.cpp`). Looked for in:

- `newgrf/beamng/` of the game's folders (`.zip` files, or unpacked mods with
  their `vehicles/` folder), for a player without BeamNG;
- the folders of `beamng_path` in `[misc]` of `openttd.cfg` (separated by `;`:
  a folder of mods, or the user folder of BeamNG);
- BeamNG's user folders: each version under `%LOCALAPPDATA%\BeamNG.drive`
  and the older `Documents\BeamNG.drive`, their `mods`, `mods\repo` and the
  mods unpacked in `mods\unpacked`.

A car is a folder `vehicles/<name>/` of a mod with a `.jbeam` whose part has
`"slotType": "main"`, a model `.dae` and an engine; a folder of configurations
only (for a car of BeamNG) is left out, so are props and trailers. Read:

- The `.jbeam` files, JSON written loosely (comments, commas left out or left
  over, words without quotes). The main part, then for each slot ("slots" or
  "slots2") the part the `.pc` names (info.json's `default_pc`, else the only
  one), else its default.
- The figures, as BeamNG drives the car: the power is the most of the
  engine's torque curve (`torque` rows of rpm and Nm); the weight the nodes'
  (`nodeWeight` before a row or in it, 25 kg otherwise) and the wheels' nodes
  (`pressureWheels`, `hubWheels`); the top speed the lesser of what the top
  gear and the final drive allow at the most revolutions (`gearRatios`, a
  differential's `gearRatio`, the wheels' `radius`) and what the power gives
  against the air (its frontal area from its nodes, Cd 0.4) and the road; the
  tractive effort the first gear's force at the wheels against the weight.
  The Volga: 103 kW, 142 km/h, 1.34 t, 0.71.
- What it carries: four passengers; a bus ("Body Style") thirty; a lorry
  ("truck", or over 3.5 t) goods of any kind but passengers.
- `info.json`: `Name` with `Brand`, `Author`, the paints (`colors` with
  `default_color`, or `paints` with `defaultPaintName1`), the default first.
- The driver's eye for the cab view: `camerasInternal`, the row "driver",
  else "dash", else one with "driver" in it.

Before the NewGRFs are looked for (`PrepareBeamngGrfs()`) the game writes a
GRF of one road vehicle of these figures, `newgrf/beamng/<name>.grf` of the
player's own folder (id `BN` and a number of the mod's and the folder's
names): introduced in 1920, never retired, every climate, priced by the
original bus (a car half). The player switches it on as "BeamNG: <name>".

Drawn from its model (`ReadBeamngShape()`): the meshes the flexbodies of its
parts name, each through its node's matrix in the `.dae` (its unit and up),
then the flexbody's `scale`, `rot` (degrees about x, then y, then z) and
`pos`; then turned into the frame of the vehicle objects (BeamNG's x is the
car's left, its y backwards: x forward = -y, y left = x), the middle of its
nodes along it at 0 and the bottoms of its wheels on the ground. Materials of
`materials.cs` (Torque's `singleton Material`) and `*.materials.json`: the
colour and texture of the first layer (`.dds` BC1, BC2, BC3, BC4, plain;
`.png`), the paint (`instanceDiffuse`, the material drawn white and painted),
and the mask that cuts the body (`opacityMap` on the second coordinates: a
triangle goes where the mask is dark at its middle -- the openings of the
lamps and the grille). Each car one of its mod's paints by its random bits
(`paint_random`, the player: "the colour of the mod"), the first in the
purchase list. Its lights as its mod has them: the glass of the materials of
`glowMap` with "lowbeam"/"highbeam" (ahead) or "brake" (at the back), and the
places of its `props` of lights ("lowbeam", "brake") -- then night as for the
other vehicle objects. The model made lighter on reading (`Simplify()`): the
corners of a material in a cell of 2 cm that face about the same way made one;
the Volga 207 000 triangles to 110 000, the closest zoom of the map has a
pixel of 4 cm. Six Volgas on the screen, turned: the same 50 to 64 ms a
redraw as without them.

Rig: `test3d kresli <engine> <degrees> <px a metre> [state] [lights] [name] [cm]`
draws a vehicle's object alone and big to `kresli.ppm` (state `vrstvy`: the
states of the layers `test3d stav` set; its crew that walks at the frames of so
many centimetres gone); `test3d auta Volga 6 20 20`.
