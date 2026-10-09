# The configurator: details a set lets the player choose on a vehicle

*OpenTTD Decouple by Karel Mácha. The player's word: "konfigurátor". Game side
in `src/vehicle_config.h`, `vehicle_config.cpp`, `vehicle_config_gui.cpp`, the
colours in `src/true_colour.h`, `true_colour.cpp`; test sets
`tests/rig/grf/konfig.yagl` and `tests/rig/grf/barvy.yagl`, written in yagl.*

A set may offer, on a vehicle, up to eight **details** the player chooses in
the game, each with up to 32 named **options**: who pulls the hand cart (College
Girl, Female Girl, an alien), whether the cart is pushed or pulled, the paint of
the cab, of the body, of the radiator. The game knows nothing of what a
detail means. It reads the names from the set, lets the player pick one option
per detail in the configurator window, keeps the choice on the vehicle and
reads it back to the set in a variable. What the set draws or does with the
choice is the set's own business.

The cargo is not a detail: the refit changes it, and the configurator leaves it
as refitted. The details are an addition to the refit, not a replacement.

## In the game

- The purchase list says under a vehicle: *Configurator: Crew, Cart* -- the
  names of its details.
- A new vehicle has the **first option** of every detail.
- The refit window has a **Configurator** button (lit when the set offers
  something, and not for a refit order). It opens the configurator: a row per
  detail with a dropdown of its options, and under them the cargo. Like a
  refit, a choice is made on a vehicle **stopped in a depot**
  (`Commands::ConfigureVehicle`); each choice goes to the vehicle at once and
  the vehicle is drawn anew. No picture of the vehicle in the window. The
  configurator closes with the refit window it was opened from.
- A cloned vehicle takes the original's choices; a vehicle renewed with the
  same model keeps them; a replacement by another model starts with the first
  options.
- The choices are saved with the vehicle (`Vehicle::config_options`: four
  from savegame version 372, eight from 373).

## In the set

Three things, a fourth for colours, all with existing GRF mechanisms. No new action, property or
syntax: yagl writes it as it is (the sets of this game are made with the
colleague's yagl and nothing else).

### 1. Ask for the feature

Action 14, a feature test by name, the way `decouple_128_cargo` is asked:

```
optional_info // Action14
{
    FTST:
    {
        NAME: default, "decouple_vehicle_config";
        MINV: [ 0x01 0x00 ];
        SETP: [ 0x09 ];            // any free bit of global variable 0x9D
    }
}
```

The game answers with version 1 and sets the bit. Only a set that asked is
ever asked for its details: without this, callbacks 1C0 and 1C1 are never
called and variables 5C and 5D are not answered (they read as for any unknown
variable). A set may test the bit with an Action 7 and skip
its own error (Action B) where the bit is set, as the colleague's sets do.

### 2. Name the details and their options: callback 0x1C0

The game calls callback **0x1C0** (`CBID_VEHICLE_DECOUPLE_CONFIG_TEXT`) on the
vehicle's engine (no vehicle, as in the purchase list) with **variable 0x10**:

| bits 8..15 | bits 0..7 | asks for |
|---|---|---|
| detail *a* (0..7) | **0xFF** | the name of detail *a* |
| detail *a* | option *o* (0..31) | the name of option *o* of detail *a* |

The result is a **text**: result `n` (`0x8000 | n` in the Action 2) is text
`0xD000 + n`; result `0x40F` takes the text id from register 0x100 with a text
stack after it. Result **`0x400`** (`0x8400`) means *no such detail* or *no
such option*, and ends the list: details are read from 0 up to the first one
without a name, the options of each from 0 up to the first without a name. A
detail without options ends the details too (it would move the ones after it
up a place, and they would no longer be the byte the set reads).

Hook it on the callback switch of the vehicle (variable 0x0C), as other
callbacks:

```
switch<RoadVehicles, 0x03, PrimaryDWord> // texts of the configurator
{
    expression: { value1 = variable[0x10] & 0x0000FFFF; };
    ranges:
    {
        0x000000FF: 0x8000;     // D000 "Crew"
        0x00000000: 0x8001;     // D001 "College Girl"
        0x00000001: 0x8002;     // D002 "Female Girl"
        0x00000002: 0x8003;     // D003 "Alien"
        0x000001FF: 0x8004;     // D004 "Cart"
        0x00000100: 0x8005;     // D005 "pulled"
        0x00000101: 0x8006;     // D006 "pushed"
    };
    default: 0x8400;            // nothing more
}
switch<RoadVehicles, 0x04, PrimaryDWord> // callbacks
{
    expression: { value1 = variable[0x0C] & 0x0000FFFF; };
    ranges: { 0x000001C0: 0x0003; };
    default: 0x0002;            // the graphics
}
```

Names are in the player's language if the set has them (Action 4 with a
language), as any other text.

### 3. Read the choice: variables 0x5C and 0x5D

In any Action 2 of the vehicle -- graphics, other callbacks -- variable
**0x5C** (dword) holds the player's choices for details 0 to 3: **byte *a* is
the option chosen for detail *a*** (0 = the first option). Variable **0x5D**
holds details 4 to 7 the same way: its byte 0 is detail 4, byte 3 detail 7. In
the purchase list both are 0. They are read from the vehicle's front for every
part of the vehicle, so an articulated vehicle's parts all see the same
choices.

```
switch<RoadVehicles, 0x02, PrimaryDWord> // the picture by the crew and the cart
{
    expression: { value1 = variable[0x5C] & 0x0000FFFF; };
    ranges:
    {
        0x00000000: 0x0010;     // College Girl pulls
        0x00000001: 0x0011;     // Female Girl pulls
        0x00000100: 0x0012;     // College Girl pushes
        ...
    };
    default: 0x0010;
}
```

The set may combine the choice with anything else it reads: the load
(variable 0x47, 0xBA...), the direction, the sprite-stack layer (variable 0x10
of the graphics chain). For the hand cart: a pulled cart with a load draws the
puller in front and a helper pushing behind; empty, the puller alone; a
pushed cart one pusher, full or empty.

### 4. Colours: callback 0x1C1

A detail may be a **colour**: the player picks the paint of the cab, the body,
the radiator, and the vehicle is drawn in it, the exact colour the set gives,
not the nearest of the 256 of the palette. The colours are a palette the set
writes down as numbers, each option of the detail one colour; every vehicle
may have other ones (the company's two colours stay for whatever the set
leaves to them).

**The picture.** The parts to paint are drawn grey in the 32bpp sprite --
light where the paint catches light, dark in the shade -- and marked by mask
(`c32bpp | mask`, a mask sheet beside the picture, as `orig_extra.grf` has).
Each colour detail paints **eight mask indices in a row**, the first of them
given by the set; which of the eight a pixel has makes no difference, its
lightness is the lightest of its red, green and blue (black stays black). The detail's eight are
the set's choice; two that work everywhere: **0xC6-0xCD** (the company
colour, so a vehicle drawn where nobody paints it is in the company colour)
and **0x50-0x57** (the second company colour, for a vehicle with two). Others,
for the body and the radiator: any eight below the palette's animated colours
(the first at most 0xDB), not 0, e.g. 0x60, 0x68, 0x70. A pixel of an 8bpp
picture, which has no red, green and blue, is lit by its place in the eight
instead: lightness 32 for the first, 64, 96, 128 (the colour itself) for the
fourth, up to 255 for the eighth.

**The callback.** The game calls callback **0x1C1**
(`CBID_VEHICLE_DECOUPLE_CONFIG_COLOUR`) on the engine (no vehicle, as for the
texts) with variable 0x10 as for 1C0:

| bits 8..15 | bits 0..7 | asks for | answer |
|---|---|---|---|
| detail *a* | **0xFF** | is detail *a* a colour, and where | the **first of its eight mask indices** (`0x8000 \| index`); register 0x100 its lightening (below), 0 for the usual. Any other answer, or none: the detail is no colour. |
| detail *a* | option *o* | the colour of option *o* | **`0x40F`** (`0x840F`) with the colour in register **0x100** as `0x00RRGGBB`; register **0x101** a lightening of this colour's own, 0 for the detail's. Any other answer: this option is no colour, the pixels stay as drawn (in the company colour where the mask is the company colour's). |

The game asks with the option the player chose, the first option in the
purchase list; so the **first option is the vehicle's default colour** -- for
our Tatras the orange they have now. The set stores the numbers with the
`TempStore` operation of a switch (register in `value2`, the number in
`value1`), a constant being `variable[0x1A] & <number>`:

```
switch<RoadVehicles, 0x10, PrimaryDWord> // cab: Tatra orange, in register 100
{
    expression:
    {
        value1 = variable[0x1A] & 0x00E8641E;      // the colour, 0x00RRGGBB
        value2 = variable[0x1A] & 0x00000100;      // register 0x100
        value1 = TempStore(value1, value2);
    };
    ranges: { 0x00000000: 0x840F; };
    default: 0x840F;                               // result 0x40F: the colour is in register 100
}
switch<RoadVehicles, 0x18, PrimaryDWord> // body: mask 0x60-0x67, lightening start 100, stop 200, most 255
{
    expression:
    {
        value1 = variable[0x1A] & 0x00FFC864;      // 100 | 200 << 8 | 255 << 16
        value2 = variable[0x1A] & 0x00000100;
        value1 = TempStore(value1, value2);
    };
    ranges: { 0x00000000: 0x8060; };
    default: 0x8060;                               // first mask index 0x60
}
switch<RoadVehicles, 0x19, PrimaryDWord> // the colours (callback 1C1)
{
    expression: { value1 = variable[0x10] & 0x0000FFFF; };
    ranges:
    {
        0x000000FF: 0x80C6;     // cab: paints 0xC6-0xCD, usual lightening
        0x00000000: 0x0010;     // cab option 0: Tatra orange
        0x00000001: 0x0011;     // cab option 1: white
        0x000001FF: 0x0018;     // body: paints 0x60-0x67, its own lightening
        0x00000100: 0x0013;     // body option 0 ...
    };
    default: 0x8400;            // no colour
}
```

and the callback switch takes `0x000001C1` to it beside `0x000001C0`. The
whole of it, with a radiator and a black that lightens otherwise, is the rig's
set `tests/rig/grf/barvy.yagl`.

**The shading: the player's three numbers.** Each colour lightens by three
numbers, 0 to 255 each, given in one register as `start | stop << 8 | most <<
16`:

1. **start** -- the lightness of the grey where the pixel is exactly the
   colour, and where lightening starts. Darker grey darkens the colour toward
   black evenly: at half the start, half the colour.
2. **stop** -- the lightness where lightening stops: lighter grey than this is
   as light as here.
3. **most** -- how near white the colour would get at the lightest grey, 255,
   were there no stop: 0 not at all, 255 white. Between the start and the stop
   it goes evenly toward it.

So a pixel of lightness *L* above the start is the colour moved toward white
by `(min(L, stop) - start) / (255 - start) * most / 255` of the way. Nothing
given (0) is **start 128, stop 255, most 128**: exact at 128, half way to white
at 255. Render the grey so that a plain flat panel is at the start.

**How the game draws it.** The picture is made again with the painted pixels in
their colours when it is read, once for each set of colours some vehicle is in,
and drawn like any other sprite, by every blitter; a painted pixel has no mask
index left, so neither the company colour nor a recolour sprite changes it. A
vehicle's parts are all painted with the choices on its front, each part asked
of its own engine. In an 8bpp game each painted pixel becomes the nearest
colour of the palette.

## Limits and rules

- At most 8 details, 32 options each. More are not read.
- A detail's options are a list in the set's order; the player's choice is
  the index. A release of the set that reorders or removes options changes
  what saved vehicles show; add new options at the end.
- The names are asked whenever the game needs them (purchase list, refit
  window, configurator); keep the callback cheap and without side effects.
- The choice is kept on the vehicle's front; the set's graphics for every part
  read the same value.
- A colour detail paints eight mask indices in a row from the one the set
  gives, never the animated colours of the palette (0xE3 and up).
- The colours are asked once for each engine and choice and kept until the
  sets are read again; keep callback 1C1 without side effects too.
