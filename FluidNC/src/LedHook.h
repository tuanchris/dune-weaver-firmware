// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  Which LED state hook applies, and whether the user's own effect choice has
  suspended it.

  The hooks are $LED/RunEffect (while Run/Jog/Home) and $LED/IdleEffect (while
  Idle/Hold): each names an effect that overrides $LED/Effect for as long as the
  machine is in that state, or "none" to leave the strip manual.

  The rule this class exists to enforce: an explicit effect change outranks the
  hook until the machine next changes state category.  Without it,
  $LED/IdleEffect=off makes every "turn the LEDs on" at an idle table a silent
  no-op -- the write lands in NVS, the hook keeps rendering "off", and the only
  brightness that looks correct is 0.

  Std-only on purpose (no RMT, no Setting, no machine state) so the precedence
  rules are unit-testable in [env:tests]; Leds owns one instance.
*/

class LedHook {
public:
    // Machine-state categories the hooks key off.
    enum Cat {
        CatNone = -1,  // nothing reported yet (boot)
        CatRun  = 0,   // Run / Jog / Home  -> $LED/RunEffect
        CatIdle = 1,   // Idle / Hold       -> $LED/IdleEffect
    };

    // A status report placed the machine in this category.  Entering a new one
    // re-arms the hook: the manual choice was a statement about the old state.
    void reportCat(Cat cat) {
        if (cat != _cat) {
            _cat    = cat;
            _manual = kNever;
        }
    }

    // The user chose an effect by hand -- suspend the hook for this category.
    void manualChoice() { _manual = _cat; }

    // The effect id the hook is forcing, or -1 for "no override".  Both hook
    // settings are passed in every call (rather than cached) so clearing one
    // with =none takes effect on the next frame.  A negative id means "none":
    // EFFECT_NONE (255) does not fit the int8_t an EnumSetting stores, so it
    // reads back as -1.
    int effect(int run_effect, int idle_effect) const {
        if (_cat == CatNone || _manual == _cat) {
            return -1;
        }
        int hook = _cat == CatRun ? run_effect : idle_effect;
        return hook < 0 ? -1 : hook;
    }

    Cat cat() const { return _cat; }

private:
    static constexpr int kNever = -2;  // never equal to a real category

    Cat _cat    = CatNone;
    int _manual = kNever;
};
