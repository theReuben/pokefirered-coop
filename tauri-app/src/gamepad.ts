// Gamepad -> GBA button mapping.
//
// Fixed default mapping, no remap UI.  Values are the GBA KEYINPUT bit
// positions, the same ones GameScreen.tsx uses for the keyboard, so both
// sources produce masks in one shared vocabulary.
//
// Unlike DOM key events, navigator.getGamepads() does not require the game
// div to hold focus — the controller keeps working after the player clicks
// the room-code button.

export const GBA_A      = 1 << 0;
export const GBA_B      = 1 << 1;
export const GBA_SELECT = 1 << 2;
export const GBA_START  = 1 << 3;
export const GBA_RIGHT  = 1 << 4;
export const GBA_LEFT   = 1 << 5;
export const GBA_UP     = 1 << 6;
export const GBA_DOWN   = 1 << 7;
// NB: in the GBA KEYINPUT register R is bit 8 and L is bit 9 — that order is
// easy to get backwards, and did ship backwards here.
export const GBA_R      = 1 << 8;
export const GBA_L      = 1 << 9;

// Standard Gamepad button indices (https://w3c.github.io/gamepad/#remapping).
// Both shoulder and trigger map to L/R so either feels natural.
const BUTTON_MAP: Record<number, number> = {
  0:  GBA_A,       // south (A / cross)
  1:  GBA_B,       // east  (B / circle)
  4:  GBA_L,       // left shoulder
  6:  GBA_L,       // left trigger
  5:  GBA_R,       // right shoulder
  7:  GBA_R,       // right trigger
  8:  GBA_SELECT,  // back / view / share
  9:  GBA_START,   // start / menu / options
  12: GBA_UP,
  13: GBA_DOWN,
  14: GBA_LEFT,
  15: GBA_RIGHT,
};

// Analogue triggers report as buttons with a fractional `value`; treat anything
// past halfway as pressed.  Same threshold for the stick so a resting stick
// (which idles anywhere up to ~0.2 on worn hardware) never emits a direction.
const PRESS_THRESHOLD = 0.5;
const AXIS_DEADZONE   = 0.5;

// Returns the union of every GBA button currently held on any connected pad.
// Zero when no gamepad is attached, so callers need no special case.
export function pollGamepad(): number {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  let mask = 0;

  for (const pad of pads) {
    if (!pad || !pad.connected) continue;

    for (const [indexStr, bit] of Object.entries(BUTTON_MAP)) {
      const button = pad.buttons[Number(indexStr)];
      if (!button) continue;
      if (button.pressed || button.value >= PRESS_THRESHOLD) mask |= bit;
    }

    // Left stick folded into the d-pad bits.  Opposite directions can never be
    // held together on one axis, so this can't produce left+right.
    const x = pad.axes[0] ?? 0;
    const y = pad.axes[1] ?? 0;
    if (x <= -AXIS_DEADZONE) mask |= GBA_LEFT;
    else if (x >= AXIS_DEADZONE) mask |= GBA_RIGHT;
    if (y <= -AXIS_DEADZONE) mask |= GBA_UP;
    else if (y >= AXIS_DEADZONE) mask |= GBA_DOWN;
  }

  return mask;
}
