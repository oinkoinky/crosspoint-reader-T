#pragma once

// The unit link calls these to copy a touch lock between two units in a case. The original X4 has no touch screen,
// so there is nothing to lock: every call does nothing and the lock is always off.
namespace touchlock {
inline bool locked() { return false; }
inline void set(bool) {}
inline void tick() {}
inline void restore() {}
}  // namespace touchlock
