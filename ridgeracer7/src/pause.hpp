// Pausing the game under the settings menu (menu.hpp).
//
// The game's threads run freely (the scheduler isn't this runtime's yet), so
// they can't all be parked at a switch point as in WindWakerHD. Instead what
// drives the game's progress is held: its frames, its vertical blanks and
// its sound. Each of those is a thread of the runtime's own, and each is held
// at a point where it holds no lock and is in none of the game's code:
//
// - the flip: the RSX's thread, once a flip's picture is handed to the window
//   (main.cpp's present sink). The game's main loop waits on the RSX (for a
//   flip to be shown, for its command buffer to be read), so it stops within
//   a frame, and the paused picture is the last one it made;
// - the vertical blank: its thread (gcm.cpp), before it calls the game's
//   handler, and on from the next blank when it goes on, not catching up;
// - the sound: the mixer's thread (main.cpp's audio sink), so the game's
//   sound ports aren't read on and its sound thread, which writes a block each
//   time one has been read, waits.
//
// What is not held: the game's own clock (the time base and the system's
// time), and threads waiting on none of these (a loader thread, the SPUs),
// which stop by themselves once what they feed stops.
//
// Runtime code: no std::format.
#pragma once

namespace rr7 {

enum class HoldPoint : unsigned { flip, vertical_blank, sound, count };

// From the window's thread: ask for the pause, or end it (every held thread goes on).
void request_pause(bool on);
bool pause_requested();
// Whether that thread is held now.
bool held_at(HoldPoint where);
// The game's frames have stopped: the RSX's thread is held at a flip.
inline bool frames_held() { return held_at(HoldPoint::flip); }

// From the runtime's thread at that point: returns at once unless a pause is
// asked for, or else waits until it's over. True if it waited.
bool hold_while_paused(HoldPoint where);

}  // namespace rr7
