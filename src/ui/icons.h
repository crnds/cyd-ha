#pragma once
#include <stdint.h>

// ICON SET — one visual language, drawn from primitives rather than stored as
// bitmaps. Three reasons that is the right trade here and not just thrift:
//
//   1. They recolour for free. Night mode swaps the whole palette at runtime, so
//      a baked RGB565 sprite would render in day colours over a red-only UI —
//      the exact bug themeMap() exists to paper over for the one bitmap we do
//      have (the splash logo).
//   2. No flash cost and no generator. logo_ha.h needs a browser in the loop to
//      regenerate (see CLAUDE.md); an icon that is nine drawFastVLine calls
//      needs nothing.
//   3. Stroke weight and optical size stay consistent by construction, because
//      every glyph below is built on the same grid with the same 1px stroke.
//
// THE GRID: every icon is centred on (cx, cy) and fits a 15x15 box — it draws
// within cx-7..cx+7 and cy-7..cy+7 and never outside it, so a caller can reserve
// one size for all of them (CARD_ICO_R in config.h). Weight is 1px throughout
// except where a shape is solid by nature (the droplet, the chevrons).
//
// EVERY ICON HERE IS USED, and each earns its place by carrying state rather
// than decorating a label:
//   icoBulb/icoSnow/icoDrop/icoPower  a device card's live status, at a glance
//   icoSun/icoMoon/icoClock/icoRotate the four settings, which are otherwise
//                                     four identical rows of text and a toggle
//   icoChevron                        the setpoint stepper and the scroll gutter
// Nothing was added because a row "looked bare", and an icon that stops earning
// its place is deleted rather than kept: icoWifi and icoBadge were the header's
// connectivity readout until that moved to a text banner in the body
// (drawNoConn(), screen.cpp), and they went with it.

// A bulb. `filled` is the ON state and is the single most useful pixel on the
// Devices page: the caller passes the bulb's real colour temperature blended by
// its real brightness, so the icon column becomes a scannable strip of what the
// room is actually doing. Outline = off, and the caller passes C_DISABLED (or
// C_ERROR when unreachable) for it.
void icoBulb(int16_t cx, int16_t cy, uint16_t c, bool filled);

// Climate modes: cooling, drying, and off. Same three states the mode chips
// select, so the icon always agrees with the highlighted chip.
void icoSnow(int16_t cx, int16_t cy, uint16_t c);
void icoDrop(int16_t cx, int16_t cy, uint16_t c);
void icoPower(int16_t cx, int16_t cy, uint16_t c);

// Settings. icoMoon needs the colour BEHIND it: a crescent is a disc minus a
// disc, and with no alpha in RGB565 the bite has to be painted in the card's own
// fill.
void icoSun(int16_t cx, int16_t cy, uint16_t c);
void icoMoon(int16_t cx, int16_t cy, uint16_t c, uint16_t bg);
void icoClock(int16_t cx, int16_t cy, uint16_t c);
void icoRotate(int16_t cx, int16_t cy, uint16_t c);

// A solid triangle, for the stepper and the scroll gutter. Solid rather than a
// stroked chevron because direction has to survive being read across a dark
// bedroom, which is the same reason the setpoint stepper does not use "+"/"-":
// two glyphs differing by one crossbar are exactly what was already tried and
// found wanting. `hw` is the half-length of the base (perpendicular to the
// point) and `hh` is the apex's offset from centre along the point — passing
// the same two numbers into a rotated direction rotates the drawn triangle
// with them, rather than redrawing a differently-proportioned one.
enum ChevDir : uint8_t { CHEV_UP, CHEV_DOWN, CHEV_LEFT, CHEV_RIGHT };
void icoChevron(int16_t cx, int16_t cy, ChevDir dir, uint16_t c, int16_t hw,
                int16_t hh);
