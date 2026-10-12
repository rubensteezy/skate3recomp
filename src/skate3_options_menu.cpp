#include "skate3_options_menu.h"

#include "generated/skate3_init.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(skate3_options_menu_entry, true, "Skate 3",
                    "Add a PC Settings row to the game's Options menu that opens the "
                    "recomp settings screen (takes effect on restart)");

namespace skate3::options_menu {

namespace {
std::function<void()> g_open_settings;
}  // namespace

#if SKATE3_HAS_TITLE_UPDATE
namespace {

// The game's Options screen is table-driven (Title Update 3 image):
// - kOptionTable: 36 x {char* title string ID, char* widget type}, indexed by
//   option id; the type is "option" (submenu/action), "slider" or "selector".
// - kScreenLayouts: int32[13][16] option ids per screen, 0-terminated. Screens
//   0 and 1 are the root page (Difficulty, Audio, Video, Controls, Online,
//   Skate Feed) with ten free slots each.
// - The state keeps the current screen at +68; its OnMenuSelect (vtable +64,
//   sub_8260D290) switches on layout[screen][row] and ignores unknown ids.
// Option id 23 is an "option"-typed entry no layout uses (title "0"), so it is
// borrowed for the new row; its title is pointed at a host-owned string ID
// that the localizer hook below resolves.
constexpr uint32_t kOptionTable = 0x83026EC8;
constexpr uint32_t kScreenLayouts = 0x83026FE8;
constexpr uint32_t kScreenCount = 13;
constexpr uint32_t kSlotsPerScreen = 16;
constexpr uint32_t kRootScreens[] = {0, 1};
constexpr int32_t kRootLayout[] = {2, 3, 4, 5, 6, 35};
constexpr int32_t kPcSettingsOptionId = 23;
constexpr uint32_t kSpareOptionTitle = 0x8220337C;  // "0"
constexpr uint32_t kAudioOptionId = 3;              // layout sanity check
constexpr uint32_t kStateScreenOffset = 68;

constexpr std::string_view kLabelId = "ID_RECOMP_PC_SETTINGS";
constexpr std::string_view kLabelText = "PC Settings";

// FE nav sound the Options screen plays when it opens a sub-screen.
constexpr uint64_t kSubmenuSound = 0x373D38D7EA167A71ull;

std::atomic<bool> g_installed{false};
uint32_t g_label_hash = 0;
uint32_t g_label_text = 0;  // guest address of kLabelText

// Same hash GetLocaleText (sub_825AA818) feeds the localizer.
uint32_t LocaleHash(std::string_view id) {
  uint32_t hash = 0xFFFFFFFFu;
  for (char c : id) {
    hash = hash * 33 + static_cast<uint8_t>(c);
  }
  return hash;
}

bool GuestStringEquals(uint8_t* base, uint32_t address, std::string_view expected) {
  if (address < 0x82000000u || address >= 0x84000000u) {
    return false;
  }
  const char* text = reinterpret_cast<const char*>(base + address);
  return std::strncmp(text, expected.data(), expected.size()) == 0 &&
         text[expected.size()] == '\0';
}

uint32_t LayoutSlot(uint32_t screen, uint32_t slot) {
  return kScreenLayouts + (screen * kSlotsPerScreen + slot) * 4;
}

uint32_t OptionTitleSlot(int32_t id) { return kOptionTable + id * 8; }
uint32_t OptionTypeSlot(int32_t id) { return kOptionTable + id * 8 + 4; }

bool ImageMatches(uint8_t* base) {
  if (REX_LOAD_U32(OptionTitleSlot(kPcSettingsOptionId)) != kSpareOptionTitle ||
      !GuestStringEquals(base, REX_LOAD_U32(OptionTypeSlot(kPcSettingsOptionId)), "option") ||
      !GuestStringEquals(base, REX_LOAD_U32(OptionTitleSlot(kAudioOptionId)),
                         "ID_GAMESETTINGS_AUDIO_SETTINGS")) {
    return false;
  }
  for (uint32_t screen : kRootScreens) {
    for (uint32_t slot = 0; slot < kSlotsPerScreen; ++slot) {
      const int32_t expected =
          slot < std::size(kRootLayout) ? kRootLayout[slot] : 0;
      if (static_cast<int32_t>(REX_LOAD_U32(LayoutSlot(screen, slot))) != expected) {
        return false;
      }
    }
  }
  return true;
}

bool IsPcSettingsRow(uint8_t* base, uint32_t state, uint32_t row) {
  if (state < 0x10000 || row >= kSlotsPerScreen) {
    return false;
  }
  const uint32_t screen = REX_LOAD_U32(state + kStateScreenOffset);
  if (screen >= kScreenCount) {
    return false;
  }
  return static_cast<int32_t>(REX_LOAD_U32(LayoutSlot(screen, row))) == kPcSettingsOptionId;
}

}  // namespace
#endif  // SKATE3_HAS_TITLE_UPDATE

void SetOpenSettingsCallback(std::function<void()> callback) {
  g_open_settings = std::move(callback);
}

void Install() {
#if SKATE3_HAS_TITLE_UPDATE
  if (g_installed.load(std::memory_order_relaxed) || !REXCVAR_GET(skate3_options_menu_entry)) {
    return;
  }
  auto* memory = rex::system::kernel_memory();
  if (!memory) {
    return;
  }
  uint8_t* base = memory->virtual_membase();
  if (!ImageMatches(base)) {
    REXLOG_WARN("Options menu: unexpected Options tables in the loaded image; "
                "PC Settings row not added");
    return;
  }

  // One guest block for the row's string ID and its display text.
  const uint32_t id_size = static_cast<uint32_t>(kLabelId.size()) + 1;
  const uint32_t text_size = static_cast<uint32_t>(kLabelText.size()) + 1;
  const uint32_t block = memory->SystemHeapAlloc(id_size + text_size);
  if (!block) {
    REXLOG_WARN("Options menu: guest string allocation failed; PC Settings row not added");
    return;
  }
  std::memcpy(base + block, kLabelId.data(), kLabelId.size());
  base[block + kLabelId.size()] = 0;
  g_label_text = block + id_size;
  std::memcpy(base + g_label_text, kLabelText.data(), kLabelText.size());
  base[g_label_text + kLabelText.size()] = 0;
  g_label_hash = LocaleHash(kLabelId);

  REX_STORE_U32(OptionTitleSlot(kPcSettingsOptionId), block);
  for (uint32_t screen : kRootScreens) {
    REX_STORE_U32(LayoutSlot(screen, std::size(kRootLayout)),
                  static_cast<uint32_t>(kPcSettingsOptionId));
  }
  g_installed.store(true, std::memory_order_release);
  REXLOG_INFO("Options menu: PC Settings row added to the root Options screen");
#endif
}

}  // namespace skate3::options_menu

#if SKATE3_HAS_TITLE_UPDATE
using namespace skate3::options_menu;

// Localizer string lookup by hash (r3 = hash, r4 = out table index), behind
// GetLocaleText and every other localized-text path. Returns a char*.
extern "C" REX_FUNC(sub_8290C1C0) {
  if (g_installed.load(std::memory_order_acquire) && ctx.r3.u32 == g_label_hash) {
    ctx.r3.u64 = g_label_text;
    return;
  }
  __imp__sub_8290C1C0(ctx, base);
}

// FrontEndState_Options::OnMenuSelect(menu, row = r5, item name): the row
// plays the regular submenu sound and opens the host settings overlay; every
// other row keeps the game's handling.
extern "C" REX_FUNC(sub_8260D290) {
  if (g_installed.load(std::memory_order_acquire) &&
      IsPcSettingsRow(base, ctx.r3.u32, ctx.r5.u32)) {
    REXLOG_INFO("Options menu: PC Settings selected");
    ctx.r3.u64 = kSubmenuSound;
    ctx.r4.u64 = 0;
    sub_825DFAF0(ctx, base);
    if (g_open_settings) {
      g_open_settings();
    }
    return;
  }
  __imp__sub_8260D290(ctx, base);
}
#endif
