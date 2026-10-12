#pragma once

#include <functional>

// "PC Settings" row in the game's own Options screen (FrontEndState_Options,
// options.apt). Selecting it opens the host settings overlay, so the recomp
// settings are reachable from the in-game menus as well as from Escape.
namespace skate3::options_menu {

// Patches the row into the root Options layout. Call once after the XEX image
// is loaded and before the guest launches. No-op (logged) when the image does
// not match the expected title update layout or the cvar is off.
void Install();

// Invoked from the guest thread when the row is selected; the callback must
// marshal to the UI thread itself. Set during app setup, before the guest
// boots.
void SetOpenSettingsCallback(std::function<void()> callback);

}  // namespace skate3::options_menu
