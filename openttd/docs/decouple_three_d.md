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
  view, four pixels to a degree (the right button scrolls as it always has).
  The keys **Y** and **N** turn by 15 degrees to the left and to the right,
  with **Ctrl** by 45, the next of the eight directions vehicles have
  pictures for; Ctrl+, and Ctrl+. do the same. Y and N are the only two
  letters no window of the game binds (`rotate_view_left`,
  `rotate_view_right`, `rotate_view_left_45`, `rotate_view_right_45` in the
  hotkeys file, under the global ones).
- The view turns about the ground point under the middle of the screen, which
  stays where it is. Scrolling, zooming, clicking on tiles, the town and
  station names, the floating texts all work turned. The angle is nobody's but
  this client's: not in the savegame, not in the config; every game starts
  facing north. Only the main view turns; the extra viewports and the ones in
  vehicle windows keep facing north.
- Vehicles show the picture of the direction they face **as seen from the
  turn**: a train heading north on a view turned by 90 degrees shows its
  westward picture and runs along what is now a horizontal track. Between two
  of the eight directions the nearer one is shown.

## How it is drawn

The world is turned, not the camera. A world point is turned about the pivot
(the ground point under the middle of the screen when the turn began) by the
angle, and then drawn by the game's own projection, `RemapCoords()`. So the
main viewport goes on being a plain viewport of a world that happens to be
turned: the scroll position, the zoom and the dirty blocks mean what they
always did, and only the two directions world -> screen (`ProjectTurned()`)
and screen -> world (`InverseRemapCoordsTurned()`, which walks down the line
of sight to the ground like `InverseRemapCoords2()`) know the angle. Turning
moves the pivot to the point under the middle and shifts the scroll position
so that the point stays put.

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
   roofs, vehicles, houses without a table -- is a billboard: the sprite as it
   is, hung from its anchor point turned (`ParentSpriteToDraw::ax, ay, az`,
   the world point `AddSortableSpriteToDraw()` was given), drawn far to near
   by the turned depth of its bounding box. A vehicle's sprite is the one of
   its turned direction. The game's own sprite sorter is not used here.
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
   cover) is grown over from the ground beside it.
4. **The signs** (town, station, waypoint and sign names) and the floating
   texts are collected north-up and each moved to where the ground under it is
   drawn turned.

The usual path collects against the block being drawn; the turned path
collects against the north-up box of the world under the block (from the
ground to the tallest thing there can be), so nothing the turn brings into the
block is clipped away. The block is first cleared black, which also tells the
32bpp animation blitter that these pixels do not animate; the textures and
faces are written straight into it, the billboards through the blitter as
always. It needs a 32bpp blitter: with an 8bpp one the angle stays 0.

What is not there yet: the previews that turn (purchase window, beside the
industry window), the eight-direction pictures of our zin16 buildings, boxes
for the houses of the other climates and for industries (billboards for now),
the proper sorting of long things against each other, the smaller textures of
the zoomed-out view, the seams at the edges of the map. Facing north nothing
of this runs.

## The rig

`test3d` in the console, under the null video driver with `blit=` (so the
screen is a buffer drawn for real): `test3d uhel <degrees>` turns the main
view, `test3d stred <x> <y>` puts a tile under the middle, `test3d zoom
<level>` sets the zoom, `test3d vozidlo [n]` puts the n-th visible vehicle
under the middle (`test3d vozidla` lists them), `test3d snimek <name>` writes
the screen to `screenshot/<name>.png`, `test3d mys <x> <y>` says which tile
the game finds under a pixel, `test3d cas [n]` times n redraws of the whole
screen and says where the time went, `test3d ladeni <x> <y>` writes out how
that tile is drawn (its sprites, walls, corners) and its texture as
`textura.ppm`. Screenshots asked for under the null driver are made on the
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
the girls at the bus stop; `grafika/README.md` there), static in every game,
laid over openttd.grf's sprites with Action A: a set of the game's own
graphics may replace any sprite loaded so far, openttd.grf's included
(`SpriteReplace()` in `src/newgrf/newgrf_acta.cpp`, `GRFConfig::builtin_graphics`).
