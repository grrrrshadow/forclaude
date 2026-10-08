# The configurator: details a set lets the player choose on a vehicle

*OpenTTD Decouple by Karel Mácha. The player's word: "konfigurátor". Game side
in `src/vehicle_config.h`, `vehicle_config.cpp`, `vehicle_config_gui.cpp`;
test set in `tests/rig/grf/konfig_yagl.py`, written in yagl.*

A set may offer, on a vehicle, up to four **details** the player chooses in
the game, each with up to 32 named **options**: who pulls the hand cart (College
Girl, Female Girl, an alien), whether the cart is pushed or pulled, later a
stripe on a tanker or a colour on a trailer. The game knows nothing of what a
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
  the vehicle is drawn anew. No picture of the vehicle in the window.
- A cloned vehicle takes the original's choices; a vehicle renewed with the
  same model keeps them; a replacement by another model starts with the first
  options.
- The choices are saved with the vehicle (savegame version 372,
  `Vehicle::config_options`).

## In the set

Three things, all with existing GRF mechanisms. No new action, property or
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
ever asked for its details: without this, callback 1C0 is never called and
variable 5C is not answered. A set may test the bit with an Action 7 and skip
its own error (Action B) where the bit is set, as the colleague's sets do.

### 2. Name the details and their options: callback 0x1C0

The game calls callback **0x1C0** (`CBID_VEHICLE_DECOUPLE_CONFIG_TEXT`) on the
vehicle's engine (no vehicle, as in the purchase list) with **variable 0x10**:

| bits 8..15 | bits 0..7 | asks for |
|---|---|---|
| detail *a* (0..3) | **0xFF** | the name of detail *a* |
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

### 3. Read the choice: variable 0x5C

In any Action 2 of the vehicle -- graphics, other callbacks -- variable
**0x5C** (dword) holds the player's choices: **byte *a* is the option chosen
for detail *a*** (0 = the first option). In the purchase list it is 0. It is
read from the vehicle's front for every part of the vehicle, so an articulated
vehicle's parts all see the same choices.

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

## Limits and rules

- At most 4 details, 32 options each. More are not read.
- A detail's options are a list in the set's order; the player's choice is
  the index. A release of the set that reorders or removes options changes
  what saved vehicles show; add new options at the end.
- The names are asked whenever the game needs them (purchase list, refit
  window, configurator); keep the callback cheap and without side effects.
- The choice is kept on the vehicle's front; the set's graphics for every part
  read the same value.
