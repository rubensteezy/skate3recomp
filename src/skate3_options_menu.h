#pragma once

#include <functional>

// Live recomp settings in the game's own Options screen (FrontEndState_Options,
// options.apt): Graphics, Display and PC Controls rows on the root Options
// screen open pages of native selector rows, and All PC Settings opens the
// full host settings overlay.
namespace skate3::options_menu {

// Adds the rows to the root Options layout. Call once after the XEX image is
// loaded and before the guest launches. No-op (logged) when the image does not
// match the expected title update layout or the cvar is off.
void Install();

// Both callbacks are invoked from the guest thread and must marshal to the UI
// thread themselves. Set during app setup, before the guest boots.
// Open: the "All PC Settings" row was selected.
void SetOpenSettingsCallback(std::function<void()> callback);
// Save: a page row changed its cvars; persist the user settings file.
void SetSaveSettingsCallback(std::function<void()> callback);

}  // namespace skate3::options_menu
