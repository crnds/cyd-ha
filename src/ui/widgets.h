#pragma once
#include <stdint.h>

// COMPONENT LIBRARY. Every interactive thing on the panel is one of these, and
// each takes its appearance from a single `vis` byte rather than from the
// underlying value — which is also what makes the renderer's dirty-region
// compares cheap and exactly right: two brightness values that map to the same
// highlight cost nothing to "change" between.
//
// The state set is closed and every component honours all of it:
//
//   BV_INACTIVE  default — available, not selected
//   BV_ACTIVE    selected / current state
//   BV_ACTIVE_OFF  selected, and what it selected is "off" — see ctlColour
//   BV_PRESSED   held (a PRESS_FLASH_MS tactility flash, not a state)
//   BV_DISABLED  unavailable: there is no state to show, so none is implied
//   BV_ERR       last command on this surface failed
//   BV_ACTIVE_BRI  selected, and it's a bulb's 1%/30%/100% chip — see ctlColour
//
// BV_INACTIVE is 0 so a memset of a snapshot means "inactive" — which is why
// every snapshot also carries a `valid` flag, or a cleared one would read as
// "already drawn".
//
// New states go on the END. The values are stored raw in every page's snapshot
// and compared against next frame's, so renumbering them is fine within a build
// but the ordinal is what a dirty-region compare sees.
enum BtnVis : uint8_t {
  BV_INACTIVE = 0, BV_ACTIVE, BV_PRESSED, BV_DISABLED, BV_ERR, BV_ACTIVE_OFF,
  BV_ACTIVE_BRI
};

// The state -> colour table, in ONE place. Ask for it rather than branching on
// `vis` locally: a component that rolled its own is how a pressed chip and a
// pressed chevron end up looking like different interactions.
//
// Two decisions worth keeping. An INACTIVE control has edge == fill, i.e. no
// visible border: the old UI outlined all 23 of them, and removing those
// outlines is the single largest reduction in visual noise here — an unselected
// control is legible from its fill against the card, and does not need a box.
// An ACTIVE control is dark text on the accent, never white: white-on-cyan
// measures ~1.9:1 contrast, dark-on-cyan ~9:1.
//
// ACTIVE_OFF is the same treatment in C_NEUTRAL grey, for a control whose
// selected state is the absence of one — the OFF chip. It is a separate vis and
// not a colour the caller passes in, so the rule stays in this table: every
// "selected" control in the UI is a solid fill with dark text, and only WHICH
// fill depends on what was selected.
//
// ACTIVE_BRI is the same treatment again in C_BRI yellow, scoped to exactly
// one control: a bulb card's 1%/30%/100% chip, on request — everything else
// that selects (AC mode chips, tabs, Settings chips, night-mode picker, a
// swatch's accent halo) stays on ACCENT. A second saturated colour loose in
// the whole UI would undo the "ACCENT is the only one" rule; confined to one
// control on one card kind, it doesn't.
struct CtlColour { uint16_t fill, edge, fg; };
CtlColour ctlColour(uint8_t vis);

// A container. Everything on a page sits in one of these, which is what replaced
// the hairline-separated rows: a surface groups its contents without drawing a
// line, and gives text something to be legible against.
void wCard(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t fill,
           uint16_t edge);

// The workhorse control: a labelled chip. `alt` is a shorter rendering tried
// before the font is dropped (see textFit).
void wChip(int16_t x, int16_t y, int16_t w, int16_t h, const char* label,
           const char* alt, uint8_t vis);

// One end of a stepper. No BV_ACTIVE case on purpose — a step is momentary, so
// it is never "the current state".
void wStepBtn(int16_t x, int16_t y, int16_t w, int16_t h, bool up, uint8_t vis);

// A colour-temperature swatch: the control IS its value, so it carries no
// label. Selection reads as a jump from a tinted disc to a saturated one plus a
// halo — a luminance change, not a hue change, so it survives night mode.
void wSwatch(int16_t cx, int16_t cy, int16_t r, uint16_t colour, uint8_t vis);

// A settings toggle: a square knob at one end of a square track. Gets no press
// flash — the flip IS the feedback, and it is immediate because nothing here
// touches the network.
void wToggle(int16_t x, int16_t y, int16_t w, int16_t h, bool on);

// A large numeric readout, optionally with a degree ring. NOT a button and not a
// tap target — see the AC_BTN_TEMP note in config.h for why that dead cell is
// load-bearing. `bg` is the surface it sits on, since the glyphs paint no
// background of their own.
void wValue(int16_t x, int16_t y, int16_t w, int16_t h, const char* val,
            bool degree, uint16_t fg, uint16_t bg);

// A navigation tab: a label, and under the selected one a 2px underline flush
// with the TOP of the rect you pass. Seating the bar on the header rule is
// therefore the caller's choice of rect, not a constant baked in here — the
// header sits at the bottom of the screen, so the rule (and the bar that seats
// on it) is at the top of the band, not the bottom.
//
// It is an underline and not a pill because a filled pill made the header read
// as a fourth row of buttons — the same solid-accent fill that means "selected"
// on a chip, spent on something that navigates rather than acts. Selection is
// carried by two signals, the text tier AND the bar, so it survives the collapse
// to red at night the same way the connectivity glyph does.
//
// The bar spans the WORD, not the cell: at 81px per cell against a 46-65px
// label, a full-width bar reads as a box around the label, which is the button
// again. Unlike a pill it can also be repainted per-cell — a straight rect has
// no rounded ends to bleed into the neighbouring cell's fill.
void wTab(int16_t x, int16_t y, int16_t w, int16_t h, const char* label,
          uint8_t vis);

// One scene pip. An off bulb is a ring, not a dim disc: a disc dark enough to
// read as "off" is also dark enough to be invisible across a dark room.
void wPip(int16_t cx, int16_t cy, int16_t r, uint16_t c, bool filled);
