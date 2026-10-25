#pragma once

namespace online {
// Set once by main before runtime/UI threads start. A fresh process releases the
// override; personal settings are never loaded into this disposable match.
inline bool match_settings_locked = false;
}
