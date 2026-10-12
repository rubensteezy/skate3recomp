#include "skate3_options_menu.h"

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(skate3_options_menu_entry, true, "Skate 3",
                    "Add a PC Settings page to the game's Options menu with the live recomp "
                    "settings (takes effect on restart)");

namespace skate3::options_menu {

namespace {
std::function<void()> g_open_settings;
std::function<void()> g_save_settings;
}  // namespace

#if SKATE3_HAS_TITLE_UPDATE
namespace {

// The game's Options screen (FrontEndState_Options, options.apt) is
// table-driven (Title Update 3 image):
// - kOptionTable: 36 x {char* title string ID, char* widget type}, indexed by
//   option id; the type is "option" (submenu/action), "slider" or "selector".
// - kScreenLayouts: int32[13][16] option ids per screen, 0-terminated. Screens
//   0 and 1 are the root page (Difficulty, Audio, Video, Controls, Online,
//   Skate Feed) with ten free slots each.
// - The state keeps the current screen at +68 and the highlighted row at +72.
//   The movie pulls everything through GameSettings_* APT natives and the
//   state's per-row getters; Back (input handler, event 3) always returns to
//   the root screen.
// Option id 23 is an "option"-typed entry no layout uses (title "0"); it is
// borrowed for the root "PC Settings" row. Selecting it switches the state to
// kPcSettingsScreen, a screen id past the game's tables that every table
// reader below answers for instead of the tables. All text goes through the
// localizer hook, which resolves our host-owned string IDs.
constexpr uint32_t kOptionTable = 0x83026EC8;
constexpr uint32_t kScreenLayouts = 0x83026FE8;
constexpr uint32_t kScreenCount = 13;
constexpr uint32_t kSlotsPerScreen = 16;
constexpr uint32_t kRootScreens[] = {0, 1};
constexpr int32_t kRootLayout[] = {2, 3, 4, 5, 6, 35};
constexpr int32_t kPcSettingsOptionId = 23;
constexpr uint32_t kSpareOptionTitle = 0x8220337C;  // "0"
constexpr int32_t kAudioOptionId = 3;               // layout sanity check
constexpr int32_t kSelectorOptionId = 11;           // camera angle: "selector"
constexpr uint32_t kPcSettingsScreen = kScreenCount;

constexpr uint32_t kStateScreenOffset = 68;
constexpr uint32_t kStateRowOffset = 72;

// Literal ("#") and stock string IDs the GameSettings natives push.
constexpr uint32_t kStringEmpty = 0x822039C4;         // "#"
constexpr uint32_t kStringCommonSelect = 0x822039B0;  // "ID_COMMON_SELECT"

// Input handler events (FrontEndState_Options vtable +40).
constexpr uint32_t kInputBack = 3;
constexpr uint32_t kInputLeft = 14;
constexpr uint32_t kInputRight = 15;

// Refresh modes passed to the movie wrapper: full screen rebuild vs values.
constexpr uint32_t kRefreshScreen = 1;
constexpr uint32_t kRefreshValues = 50;

// FE sounds the Options screen uses for sub-screen entry and selector steps.
constexpr uint64_t kSubmenuSound = 0x373D38D7EA167A71ull;
constexpr uint64_t kSelectorLeftSound = 0xAD8BF9A0B4997B49ull;
constexpr uint64_t kSelectorRightSound = 0xD8EE38A61C45EA28ull;

constexpr std::string_view kPageId = "ID_RECOMP_PC_SETTINGS";
constexpr std::string_view kPageText = "PC Settings";

enum class RowKind { kSelector, kAction };

struct Row {
  std::string title;
  std::string desc;
  RowKind kind = RowKind::kSelector;
  std::vector<std::string> labels;
  std::function<int()> get;       // current label index
  std::function<void(int)> set;   // apply a label index
  // Guest string IDs, resolved by the localizer hook.
  uint32_t title_id = 0;
  uint32_t desc_id = 0;
  std::vector<uint32_t> label_ids;
};

std::atomic<bool> g_installed{false};
std::vector<Row> g_rows;
std::unordered_map<uint32_t, uint32_t> g_locale;  // ID hash -> guest text
uint32_t g_page_id = 0;
uint32_t g_type_selector = 0;
uint32_t g_type_option = 0;

// Same hash GetLocaleText (sub_825AA818) feeds the localizer.
uint32_t LocaleHash(std::string_view id) {
  uint32_t hash = 0xFFFFFFFFu;
  for (char c : id) {
    hash = hash * 33 + static_cast<uint8_t>(c);
  }
  return hash;
}

bool HasCvar(std::string_view name) { return rex::cvar::GetFlagInfo(name) != nullptr; }

// ---- Rows -------------------------------------------------------------------

int NearestIndex(const std::vector<double>& values, double value) {
  int best = 0;
  for (int i = 1; i < static_cast<int>(values.size()); ++i) {
    if (std::abs(values[i] - value) < std::abs(values[best] - value)) {
      best = i;
    }
  }
  return best;
}

void AddToggle(std::string title, std::string desc, std::vector<std::string> cvars) {
  std::erase_if(cvars, [](const std::string& name) { return !HasCvar(name); });
  if (cvars.empty()) {
    return;
  }
  Row row;
  row.title = std::move(title);
  row.desc = std::move(desc);
  row.labels = {"Off", "On"};
  row.get = [cvars] { return rex::cvar::Query<bool>(cvars.front()) ? 1 : 0; };
  row.set = [cvars](int value) {
    for (const auto& name : cvars) {
      rex::cvar::SetFlagByName(name, value ? "true" : "false");
    }
  };
  g_rows.push_back(std::move(row));
}

void AddChoice(std::string title, std::string desc, std::vector<std::string> labels,
               std::vector<double> values, std::vector<std::string> cvars) {
  std::erase_if(cvars, [](const std::string& name) { return !HasCvar(name); });
  if (cvars.empty()) {
    return;
  }
  Row row;
  row.title = std::move(title);
  row.desc = std::move(desc);
  row.labels = std::move(labels);
  row.get = [values, cvars] {
    return NearestIndex(values, rex::cvar::Query<double>(cvars.front()));
  };
  row.set = [values, cvars](int index) {
    const std::string text = std::to_string(values[index]);
    for (const auto& name : cvars) {
      rex::cvar::SetFlagByName(name, text);
    }
  };
  g_rows.push_back(std::move(row));
}

void BuildRows() {
  {
    std::vector<std::string> labels;
    std::vector<double> values;
    for (int degrees = 40; degrees <= 120; degrees += 5) {
      labels.push_back(degrees == 60 ? "60 (Default)" : std::to_string(degrees));
      values.push_back(degrees);
    }
    AddChoice("Field of View", "Gameplay camera field of view in degrees.", std::move(labels),
              std::move(values), {"skate3_field_of_view"});
  }
  AddChoice("Draw Distance",
            "Scales how far world detail and characters stay at full detail.",
            {"Original", "2x", "3x", "5x"}, {1.0, 2.0, 3.0, 5.0},
            {"skate3_draw_distance_scale", "skate3_lod_distance_scale"});
  AddChoice("World Streaming",
            "Pre-loads neighbouring world cells within this radius so distant "
            "areas are ready before you reach them.",
            {"Original", "Near (100 m)", "Extended (200 m)", "Far (300 m)"},
            {0.0, 100.0, 200.0, 300.0}, {"skate3_draw_distance_stream_probe"});
  AddToggle("Ambient Occlusion",
            "Soft contact shading where surfaces meet. Native renderer only.",
            {"skate3_native_render_scene_ssao"});
  AddToggle("Bloom", "Glow around bright light sources. Native renderer only.",
            {"skate3_native_render_scene_bloom"});
  AddToggle("Volumetric Lighting",
            "Sun shafts and directional haze that follow the time of day. Native "
            "renderer only.",
            {"skate3_native_render_scene_shafts", "skate3_native_render_scene_haze"});
  AddToggle("Enhanced Shadows",
            "Higher quality shadows from buildings and level geometry. Native "
            "renderer only.",
            {"skate3_native_render_scene_shadow_static_casters"});
  AddToggle("PCSS Shadows",
            "Contact-hardening soft shadows: sharp near the caster, softer with "
            "distance. Native renderer only.",
            {"skate3_native_render_scene_shadow_pcss"});
  AddToggle("FPS Counter", "Shows the frame rate in the corner of the screen.",
            {"show_fps_counter"});

  Row all;
  all.title = "All PC Settings";
  all.desc =
      "Opens the full recomp settings: graphics API, resolution, frame rate, "
      "controls, profiles and more.";
  all.kind = RowKind::kAction;
  g_rows.push_back(std::move(all));
}

// ---- Guest strings ------------------------------------------------------------

// Writes every string ID and its display text into one guest block and fills
// the localizer map. Returns false when the allocation fails.
bool PublishStrings(rex::memory::Memory* memory, uint8_t* base) {
  std::vector<std::pair<std::string, std::string>> entries;
  entries.emplace_back(std::string(kPageId), std::string(kPageText));
  for (size_t r = 0; r < g_rows.size(); ++r) {
    const std::string prefix = "ID_RECOMP_PC_SETTINGS_" + std::to_string(r);
    entries.emplace_back(prefix, g_rows[r].title);
    entries.emplace_back(prefix + "_DESC", g_rows[r].desc);
    for (size_t v = 0; v < g_rows[r].labels.size(); ++v) {
      entries.emplace_back(prefix + "_V" + std::to_string(v), g_rows[r].labels[v]);
    }
  }
  uint32_t size = 0;
  for (const auto& [id, text] : entries) {
    size += static_cast<uint32_t>(id.size() + text.size() + 2);
  }
  const uint32_t block = memory->SystemHeapAlloc(size);
  if (!block) {
    return false;
  }
  uint32_t cursor = block;
  auto write = [&](const std::string& text) {
    const uint32_t address = cursor;
    std::memcpy(base + cursor, text.data(), text.size());
    base[cursor + text.size()] = 0;
    cursor += static_cast<uint32_t>(text.size() + 1);
    return address;
  };
  std::vector<uint32_t> ids;
  for (const auto& [id, text] : entries) {
    ids.push_back(write(id));
    g_locale[LocaleHash(id)] = write(text);
  }
  size_t next = 0;
  g_page_id = ids[next++];
  for (auto& row : g_rows) {
    row.title_id = ids[next++];
    row.desc_id = ids[next++];
    for (size_t v = 0; v < row.labels.size(); ++v) {
      row.label_ids.push_back(ids[next++]);
    }
  }
  return true;
}

// ---- Tables -------------------------------------------------------------------

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
      !GuestStringEquals(base, REX_LOAD_U32(OptionTypeSlot(kSelectorOptionId)), "selector") ||
      !GuestStringEquals(base, REX_LOAD_U32(OptionTitleSlot(kAudioOptionId)),
                         "ID_GAMESETTINGS_AUDIO_SETTINGS") ||
      !GuestStringEquals(base, kStringEmpty, "#") ||
      !GuestStringEquals(base, kStringCommonSelect, "ID_COMMON_SELECT")) {
    return false;
  }
  for (uint32_t screen : kRootScreens) {
    for (uint32_t slot = 0; slot < kSlotsPerScreen; ++slot) {
      const int32_t expected = slot < std::size(kRootLayout) ? kRootLayout[slot] : 0;
      if (static_cast<int32_t>(REX_LOAD_U32(LayoutSlot(screen, slot))) != expected) {
        return false;
      }
    }
  }
  return true;
}

// ---- Guest helpers ------------------------------------------------------------

bool IsInstalled() { return g_installed.load(std::memory_order_acquire); }

uint32_t StateScreen(uint8_t* base, uint32_t state) {
  return state >= 0x10000 ? REX_LOAD_U32(state + kStateScreenOffset) : 0xFFFFFFFFu;
}

bool OnPcSettingsScreen(uint8_t* base, uint32_t state) {
  return StateScreen(base, state) == kPcSettingsScreen;
}

bool IsPcSettingsRootRow(uint8_t* base, uint32_t state, uint32_t row) {
  const uint32_t screen = StateScreen(base, state);
  if (screen >= kScreenCount || row >= kSlotsPerScreen) {
    return false;
  }
  return static_cast<int32_t>(REX_LOAD_U32(LayoutSlot(screen, row))) == kPcSettingsOptionId;
}

Row* RowAt(uint32_t row) { return row < g_rows.size() ? &g_rows[row] : nullptr; }

// The live FrontEndState_Options instance, as the GameSettings natives find it.
uint32_t OptionsState(PPCContext& ctx, uint8_t* base) {
  sub_824AD240(ctx, base);
  const uint32_t manager = ctx.r3.u32;
  if (manager < 0x10000) {
    return 0;
  }
  const uint32_t owner = REX_LOAD_U32(manager + 8);
  return owner >= 0x10000 ? REX_LOAD_U32(owner + 180) : 0;
}

int32_t AptArgInt(PPCContext& ctx, uint8_t* base, uint32_t index) {
  ctx.r3.u64 = index;
  sub_82E62E00(ctx, base);
  sub_82E5F2A8(ctx, base);
  return ctx.r3.s32;
}

void AptReturnInt(PPCContext& ctx, uint8_t* base, int32_t value) {
  ctx.r3.s64 = value;
  sub_82E86550(ctx, base);
}

void AptReturnString(PPCContext& ctx, uint8_t* base, uint32_t text) {
  ctx.r3.u64 = text;
  sub_82E864F8(ctx, base);
}

void PlaySound(PPCContext& ctx, uint8_t* base, uint64_t sound) {
  ctx.r3.u64 = sound;
  ctx.r4.u64 = 0;
  sub_825DFAF0(ctx, base);
}

// What the game does after changing screen or a value: clear the pending
// flag and have the movie wrapper (state+56, vtable +20) rebuild.
void RefreshScreen(PPCContext& ctx, uint8_t* base, uint32_t state, uint32_t mode) {
  REX_STORE_U8(state + 49, 0);
  const uint32_t movie = REX_LOAD_U32(state + 56);
  const uint32_t function = REX_LOAD_U32(REX_LOAD_U32(movie) + 20);
  ctx.r3.u64 = movie;
  ctx.r4.u64 = REX_LOAD_U32(state + 8);
  ctx.r5.u64 = mode;
  REX_CALL_INDIRECT_FUNC(function);
}

void StepRow(Row& row, int delta) {
  const int count = static_cast<int>(row.labels.size());
  row.set(((row.get() + delta) % count + count) % count);
  if (g_save_settings) {
    g_save_settings();
  }
}

// Native hooks run with no incoming register arguments, but the state lookup
// calls into the guest; keep the argument registers as the caller left them
// for the game's own implementation.
struct ArgRegisters {
  explicit ArgRegisters(PPCContext& ctx)
      : ctx_(ctx), r3_(ctx.r3.u64), r4_(ctx.r4.u64), r5_(ctx.r5.u64), r6_(ctx.r6.u64) {}
  void Restore() {
    ctx_.r3.u64 = r3_;
    ctx_.r4.u64 = r4_;
    ctx_.r5.u64 = r5_;
    ctx_.r6.u64 = r6_;
  }
  PPCContext& ctx_;
  uint64_t r3_, r4_, r5_, r6_;
};

}  // namespace
#endif  // SKATE3_HAS_TITLE_UPDATE

void SetOpenSettingsCallback(std::function<void()> callback) {
  g_open_settings = std::move(callback);
}

void SetSaveSettingsCallback(std::function<void()> callback) {
  g_save_settings = std::move(callback);
}

void Install() {
#if SKATE3_HAS_TITLE_UPDATE
  if (IsInstalled() || !REXCVAR_GET(skate3_options_menu_entry)) {
    return;
  }
  auto* memory = rex::system::kernel_memory();
  if (!memory) {
    return;
  }
  uint8_t* base = memory->virtual_membase();
  if (!ImageMatches(base)) {
    REXLOG_WARN("Options menu: unexpected Options tables in the loaded image; "
                "PC Settings page not added");
    return;
  }
  BuildRows();
  if (!PublishStrings(memory, base)) {
    g_rows.clear();
    REXLOG_WARN("Options menu: guest string allocation failed; PC Settings page not added");
    return;
  }
  g_type_selector = REX_LOAD_U32(OptionTypeSlot(kSelectorOptionId));
  g_type_option = REX_LOAD_U32(OptionTypeSlot(kPcSettingsOptionId));

  REX_STORE_U32(OptionTitleSlot(kPcSettingsOptionId), g_page_id);
  for (uint32_t screen : kRootScreens) {
    REX_STORE_U32(LayoutSlot(screen, std::size(kRootLayout)),
                  static_cast<uint32_t>(kPcSettingsOptionId));
  }
  g_installed.store(true, std::memory_order_release);
  REXLOG_INFO("Options menu: PC Settings page added ({} rows)", g_rows.size());
#endif
}

}  // namespace skate3::options_menu

#if SKATE3_HAS_TITLE_UPDATE
using namespace skate3::options_menu;

// Localizer string lookup by hash (r3 = hash, r4 = out table index), behind
// GetLocaleText and every other localized-text path. Returns a char*.
extern "C" REX_FUNC(sub_8290C1C0) {
  if (IsInstalled()) {
    if (auto it = g_locale.find(ctx.r3.u32); it != g_locale.end()) {
      ctx.r3.u64 = it->second;
      return;
    }
  }
  __imp__sub_8290C1C0(ctx, base);
}

// ---- FrontEndState_Options methods (r3 = state) -------------------------------

// OnMenuSelect(menu, row = r5, item name). The root PC Settings row opens the
// page; on the page, actions run and selectors step forward.
extern "C" REX_FUNC(sub_8260D290) {
  if (IsInstalled()) {
    const uint32_t state = ctx.r3.u32;
    const uint32_t row_index = ctx.r5.u32;
    if (OnPcSettingsScreen(base, state)) {
      if (Row* row = RowAt(row_index)) {
        if (row->kind == RowKind::kAction) {
          PlaySound(ctx, base, kSubmenuSound);
          if (g_open_settings) {
            g_open_settings();
          }
        } else {
          PlaySound(ctx, base, kSelectorRightSound);
          StepRow(*row, 1);
          RefreshScreen(ctx, base, state, kRefreshValues);
        }
      }
      return;
    }
    if (IsPcSettingsRootRow(base, state, row_index)) {
      PlaySound(ctx, base, kSubmenuSound);
      REX_STORE_U32(state + kStateScreenOffset, kPcSettingsScreen);
      RefreshScreen(ctx, base, state, kRefreshScreen);
      return;
    }
  }
  __imp__sub_8260D290(ctx, base);
}

// Input handler (r5 = event). Left/right step the page's selectors; Back keeps
// the game's handling (always returns to the root screen); the page consumes
// nothing else, so the game never indexes its tables with the page's id.
extern "C" REX_FUNC(sub_8260C258) {
  if (IsInstalled() && OnPcSettingsScreen(base, ctx.r3.u32) && ctx.r5.u32 != kInputBack) {
    const uint32_t state = ctx.r3.u32;
    const uint32_t event = ctx.r5.u32;
    Row* row = RowAt(REX_LOAD_U32(state + kStateRowOffset));
    if ((event == kInputLeft || event == kInputRight) && row &&
        row->kind == RowKind::kSelector) {
      const bool left = event == kInputLeft;
      PlaySound(ctx, base, left ? kSelectorLeftSound : kSelectorRightSound);
      StepRow(*row, left ? -1 : 1);
      RefreshScreen(ctx, base, state, kRefreshValues);
      ctx.r3.u64 = 1;
    } else {
      ctx.r3.u64 = 0;
    }
    return;
  }
  __imp__sub_8260C258(ctx, base);
}

// Row title string ID (r4 = row).
extern "C" REX_FUNC(sub_8260D718) {
  if (IsInstalled() && OnPcSettingsScreen(base, ctx.r3.u32)) {
    const Row* row = RowAt(ctx.r4.u32);
    ctx.r3.u64 = row ? row->title_id : kStringEmpty;
    return;
  }
  __imp__sub_8260D718(ctx, base);
}

// Row integer value (r4 = row): the selected label index on the page.
extern "C" REX_FUNC(sub_8260D7E8) {
  if (IsInstalled() && OnPcSettingsScreen(base, ctx.r3.u32)) {
    const Row* row = RowAt(ctx.r4.u32);
    ctx.r3.s64 = row && row->kind == RowKind::kSelector ? row->get() : 0;
    return;
  }
  __imp__sub_8260D7E8(ctx, base);
}

// Row value string ID (r4 = row).
extern "C" REX_FUNC(sub_8260D968) {
  if (IsInstalled() && OnPcSettingsScreen(base, ctx.r3.u32)) {
    const Row* row = RowAt(ctx.r4.u32);
    ctx.r3.u64 = row && row->kind == RowKind::kSelector ? row->label_ids[row->get()]
                                                         : kStringEmpty;
    return;
  }
  __imp__sub_8260D968(ctx, base);
}

// Row enabled (r5 = row): every page row is enabled.
extern "C" REX_FUNC(sub_8260D648) {
  if (IsInstalled() && OnPcSettingsScreen(base, ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_8260D648(ctx, base);
}

// ---- GameSettings_* APT natives that read the tables directly ------------------

// GameSettings_GetNumOptions().
extern "C" REX_FUNC(sub_825A6F70) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    if (OnPcSettingsScreen(base, OptionsState(ctx, base))) {
      AptReturnInt(ctx, base, static_cast<int32_t>(g_rows.size()));
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6F70(ctx, base);
}

// GameSettings_GetOptionType(row).
extern "C" REX_FUNC(sub_825A6CE0) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    if (OnPcSettingsScreen(base, OptionsState(ctx, base))) {
      const Row* row = RowAt(static_cast<uint32_t>(AptArgInt(ctx, base, 0)));
      AptReturnString(ctx, base,
                      row && row->kind == RowKind::kSelector ? g_type_selector : g_type_option);
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6CE0(ctx, base);
}

// GameSettings_GetScreenTitle().
extern "C" REX_FUNC(sub_825A6DA8) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    if (OnPcSettingsScreen(base, OptionsState(ctx, base))) {
      AptReturnString(ctx, base, g_page_id);
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6DA8(ctx, base);
}

// GameSettings_GetValueDescription(row).
extern "C" REX_FUNC(sub_825A6E88) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    if (OnPcSettingsScreen(base, OptionsState(ctx, base))) {
      const Row* row = RowAt(static_cast<uint32_t>(AptArgInt(ctx, base, 0)));
      AptReturnString(ctx, base, row ? row->desc_id : kStringEmpty);
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6E88(ctx, base);
}

// GameSettings_GetButtonHelpTextA(): "Select" on action rows only, as the game
// does for its "option" rows.
extern "C" REX_FUNC(sub_825A6FE0) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    const uint32_t state = OptionsState(ctx, base);
    if (OnPcSettingsScreen(base, state)) {
      const Row* row = RowAt(REX_LOAD_U32(state + kStateRowOffset));
      AptReturnString(ctx, base,
                      row && row->kind == RowKind::kAction ? kStringCommonSelect
                                                           : kStringEmpty);
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6FE0(ctx, base);
}
#endif
