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
  setting says so. The keys
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
  pieces -- has one likeness made from all its pieces, and each piece is
  drawn as its own cut of it, at its own place and in its own direction,
  so the vehicle bends on a curve as its sprites do; the two heads of a
  double-headed engine stand at either end of the train; each wagon is its
  own. A vehicle whose likeness cannot be made (no sprites in the set, a
  crashed one, an 8bpp blitter) shows the sprite of the nearer of the eight
  directions as seen from the turn, as before.
- Stations turn too, any set's: the platforms as flat slabs laid over the
  turned tiles, the buildings and roofs as boxes, each from its own sprite.
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
   the chain spaces them, and knows where each piece lies along it
   (`part_x`, `part_cut`: a piece ends half its length behind its
   position); each piece is drawn as its own cut of the model about its
   own position, in its own direction, so the vehicle bends on a curve as
   its sprites do, and the rear head of a double-headed engine stands at
   the end of the train. A vehicle without a likeness is a billboard of the
   sprite of its turned direction, as before.
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
   (`src/three_d_model.cpp`, the table `table/three_d_models.h`: ten
   houses of the temperate climate, the school, the statue of stone and of
   bronze, the machine and the hut) turn for real. Every pixel of a render
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
`baseset/decouple/objekty/<set>/` the first time one is wanted.

- **The file.** glTF binary (`.glb`): the JSON and the buffer; the first
  mesh's triangles, their corners' positions and texture coordinates, the
  node's transform applied; glTF's Y is up and its Z points along the game's
  -y (Blender writes x, z, -y); metres to units, a tile 14.83765 m as the
  renders have it and a height level 3 m. The base colour's texture is a PNG
  in the buffer, read by libpng into the blitter's colours the first time the
  object is drawn (an alpha under half is transparent), halved into levels of
  detail down to a pixel.
- **The index** says which house of the original game each object is
  (`nahrazuje_house`, the house of its north tile), its size in tiles and
  which colour variants (`varianty`, the slot `TileHash2Bit()` of the tile
  picks in `_town_draw_tile_data`); a variant whose sprite is another shape
  has an object of its own. An object reaching out of its plot is said in
  the log (`-d misc=1`).
- **Where.** A finished house of the original game (not a NewGRF's, not one
  being built, houses not see-through) whose north tile's variant has an
  object, and that has no eight renders (those keep them): in the **turned
  view** it is drawn once, whole, among the standing things by the box of its
  plot (`box_kind` 7, `DrawTurnedObject()`): its triangles turned with the
  view, far first by their middles, as the faces of the boxes, each filled
  with its texture at the level of the zoom -- the texture's full width for a
  tile at the closest zoom, halved with every zoom level out; the sprites of
  its tiles are left out. In the **cab view** its triangles go through the
  perspective rasterizer and the depth buffer with the texture's level for
  about a texture pixel to a pixel of the picture. The view facing north keeps
  the sprites, pixel for pixel.

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
in the company's colours, turning smoothly as its **3D likeness made from its
eight sprites** (below); a set whose chain gives no picture for some
direction gets the old preview, the eight sprites one after another. Beside
the **window of one of the game's own industries** that has eight renders
(the school, the machine, the statue, the hut) and of the plantation it is
the building, turning smoothly, drawn from its renders the way the map draws
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
drives straight along it across the land at half its speed -- over grass,
fields, rough land and through trees, across roads and level crossings --
and stands where it may not go: a house or any building, water, rails but at
a crossing, a steep slope, a bridge's end, a tunnel, a depot, the map's edge;
it turns there and goes on. A ship goes freely with its first press of left
or right, on open water and past buoys, and stands at a bank, a lock, a dock.
Single road vehicles only (no trams, none with trailers), not in a depot, a
tunnel or a bay. The heading is `Vehicle::cab_yaw` in 128ths of a turn and the
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
the view), `test3d motory [text]` lists the rail engines with their pieces
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
`textura.ppm`. For the buildings with eight renders: `test3d modely` lists
them and whether their sprites are there, `test3d model <name>` reads the
first render and makes the picture at the angle and zoom of the view, saying
how long each took, `test3d budovy` says where such buildings stand on the
map, `test3d postav <x> <y> <name>` puts one there (a house, or one of the
game's own industries, which must be in a town). For the previews:
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

The renders of the buildings that turn are the sets `budovy3d_<set>_V<version>.grf`
there, each under 100 MB, made by `grafika/openttd_3d_grf.py` from the
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
only the sets and their `.yagl`.
