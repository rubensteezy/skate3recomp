#include "skate3_options_menu.h"

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
// borrowed for the root "PC Settings" row. Our pages are screen ids past the
// game's tables (kFirstPageScreen + page index) that every table reader below
// answers for from host rows. All text goes through the localizer hook, which
// resolves our host-owned string IDs.
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
constexpr uint32_t kFirstPageScreen = kScreenCount;

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

// FE sounds the Options screen uses for sub-screen entry, Back and selectors.
constexpr uint64_t kSubmenuSound = 0x373D38D7EA167A71ull;
constexpr uint64_t kBackSound = 0x4D927CD64CC8B818ull;
constexpr uint64_t kSelectorLeftSound = 0xAD8BF9A0B4997B49ull;
constexpr uint64_t kSelectorRightSound = 0xD8EE38A61C45EA28ull;

constexpr std::string_view kHubId = "ID_RECOMP_PC_SETTINGS";
constexpr std::string_view kHubTitle = "PC Settings";

enum class RowKind {
  kSelector,  // left/right cycle labels
  kPage,      // A opens another page
  kOverlay,   // A opens the host settings overlay
};

struct Row {
  std::string title;
  std::string desc;
  RowKind kind = RowKind::kSelector;
  std::vector<std::string> labels;
  std::function<int()> get;        // current label index
  std::function<void(int)> set;    // apply a label index
  std::function<bool()> enabled;   // null = always enabled
  bool rebuild_on_change = false;  // other rows' enabled state depends on it
  size_t target_page = 0;          // kPage
  // Guest string IDs, resolved by the localizer hook.
  uint32_t title_id = 0;
  uint32_t desc_id = 0;
  std::vector<uint32_t> label_ids;
};

struct Page {
  std::string title;
  std::vector<Row> rows;
  uint32_t title_id = 0;
};

std::atomic<bool> g_installed{false};
std::vector<Page> g_pages;  // [0] = hub
std::unordered_map<uint32_t, uint32_t> g_locale;  // ID hash -> guest text
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

// Rows for cvars this build lacks are skipped; they return null.
Row* AddRow(Page& page, std::string title, std::string desc) {
  Row row;
  row.title = std::move(title);
  row.desc = std::move(desc);
  page.rows.push_back(std::move(row));
  return &page.rows.back();
}

Row* AddToggle(Page& page, std::string title, std::string desc, std::vector<std::string> cvars) {
  std::erase_if(cvars, [](const std::string& name) { return !HasCvar(name); });
  if (cvars.empty()) {
    return nullptr;
  }
  Row* row = AddRow(page, std::move(title), std::move(desc));
  row->labels = {"Off", "On"};
  row->get = [cvars] { return rex::cvar::Query<bool>(cvars.front()) ? 1 : 0; };
  row->set = [cvars](int value) {
    for (const auto& name : cvars) {
      rex::cvar::SetFlagByName(name, value ? "true" : "false");
    }
  };
  return row;
}

Row* AddNumberChoice(Page& page, std::string title, std::string desc,
                     std::vector<std::string> labels, std::vector<double> values,
                     std::vector<std::string> cvars) {
  std::erase_if(cvars, [](const std::string& name) { return !HasCvar(name); });
  if (cvars.empty()) {
    return nullptr;
  }
  Row* row = AddRow(page, std::move(title), std::move(desc));
  row->labels = std::move(labels);
  row->get = [values, cvars] {
    return NearestIndex(values, rex::cvar::Query<double>(cvars.front()));
  };
  row->set = [values, cvars](int index) {
    const std::string text = std::to_string(values[index]);
    for (const auto& name : cvars) {
      rex::cvar::SetFlagByName(name, text);
    }
  };
  return row;
}

Row* AddStringChoice(Page& page, std::string title, std::string desc,
                     std::vector<std::string> labels, std::vector<std::string> values,
                     std::string cvar) {
  if (!HasCvar(cvar)) {
    return nullptr;
  }
  Row* row = AddRow(page, std::move(title), std::move(desc));
  row->labels = std::move(labels);
  row->get = [values, cvar] {
    const std::string current = rex::cvar::Query<std::string>(cvar);
    const auto it = std::find(values.begin(), values.end(), current);
    return it != values.end() ? static_cast<int>(it - values.begin()) : 0;
  };
  row->set = [values, cvar](int index) { rex::cvar::SetFlagByName(cvar, values[index]); };
  return row;
}

bool NativeRendererActive() {
  return !HasCvar("skate3_native_render_scene") ||
         rex::cvar::Query<bool>("skate3_native_render_scene");
}

// Native-renderer-only rows grey out while the emulated renderer is active.
void NativeOnly(Row* row) {
  if (row) {
    row->enabled = NativeRendererActive;
  }
}

Page BuildGraphicsPage() {
  Page page{"Graphics"};
  // The renderer switch only exists on builds that can run the native path.
  if (HasCvar("skate3_native_render") && rex::cvar::Query<bool>("skate3_native_render")) {
    if (Row* row = AddToggle(page, "Renderer",
                             "Native renders the game directly on your GPU (much faster); "
                             "Emulated replays the console GPU exactly.",
                             {"skate3_native_render_scene"})) {
      row->labels = {"Emulated", "Native"};
      row->rebuild_on_change = true;
    }
  }
  NativeOnly(AddToggle(page, "Ambient Occlusion",
                       "Soft contact shading where surfaces meet: under ledges, rails and "
                       "the skater.",
                       {"skate3_native_render_scene_ssao"}));
  NativeOnly(AddToggle(page, "Bloom", "Glow around bright light sources, strongest at night.",
                       {"skate3_native_render_scene_bloom"}));
  NativeOnly(AddToggle(page, "Volumetric Lighting",
                       "Sun shafts and directional haze that follow the time of day.",
                       {"skate3_native_render_scene_shafts", "skate3_native_render_scene_haze"}));
  NativeOnly(AddToggle(page, "Enhanced Shadows",
                       "Shadows from buildings and level geometry, not just characters.",
                       {"skate3_native_render_scene_shadow_static_casters"}));
  NativeOnly(AddToggle(page, "PCSS Shadows",
                       "Contact-hardening soft shadows: sharp near the caster, softer with "
                       "distance.",
                       {"skate3_native_render_scene_shadow_pcss"}));
  return page;
}

Page BuildDisplayPage() {
  Page page{"Display"};
  {
    std::vector<std::string> labels;
    std::vector<double> values;
    for (int degrees = 40; degrees <= 120; degrees += 5) {
      labels.push_back(degrees == 60 ? "60 (Default)" : std::to_string(degrees));
      values.push_back(degrees);
    }
    AddNumberChoice(page, "Field of View", "Gameplay camera field of view in degrees.",
                    std::move(labels), std::move(values), {"skate3_field_of_view"});
  }
  AddNumberChoice(page, "Draw Distance",
                  "How far world detail and characters stay at full detail.",
                  {"Original", "2x", "3x", "5x"}, {1.0, 2.0, 3.0, 5.0},
                  {"skate3_draw_distance_scale", "skate3_lod_distance_scale"});
  AddNumberChoice(page, "World Streaming",
                  "Pre-loads neighbouring areas within this radius so they are ready "
                  "before you reach them.",
                  {"Original", "Near (100 m)", "Extended (200 m)", "Far (300 m)"},
                  {0.0, 100.0, 200.0, 300.0}, {"skate3_draw_distance_stream_probe"});
  AddToggle(page, "FPS Counter", "Shows the frame rate in the corner of the screen.",
            {"show_fps_counter"});
  AddToggle(page, "Renderer Indicator",
            "Corner readout of which renderer drew the last frame (Native or Emulated).",
            {"skate3_native_render_mode_indicator"});
  return page;
}

Page BuildControlsPage() {
  Page page{"Controls"};
  {
    const std::vector<double> values = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5,
                                        2.0,  2.5, 3.0,  4.0, 5.0};
    std::vector<std::string> labels;
    for (double value : values) {
      char text[16];
      std::snprintf(text, sizeof(text), "%.2f", value);
      labels.push_back(value == 1.0 ? std::string(text) + " (Default)" : std::string(text));
    }
    AddNumberChoice(page, "Mouse Sensitivity",
                    "Mouse-look speed while Mouse & Keyboard Mode is active.", std::move(labels),
                    values, {"mnk_sensitivity"});
  }
  AddStringChoice(page, "Settings Menu Shortcut",
                  "Controller button combination that opens the full PC settings.",
                  {"RB + Start", "LB + RB + Start", "Back + Start", "L3 + R3"},
                  {"rb+start", "lb+rb+start", "back+start", "l3+r3"}, "menu_chord");
  return page;
}

void BuildPages() {
  g_pages.push_back(Page{std::string(kHubTitle)});
  const std::pair<Page, const char*> categories[] = {
      {BuildGraphicsPage(), "Renderer, lighting and shadow quality."},
      {BuildDisplayPage(), "Field of view, draw distance and on-screen readouts."},
      {BuildControlsPage(), "Mouse sensitivity and controller shortcuts."},
  };
  for (const auto& [page, desc] : categories) {
    if (page.rows.empty()) {
      continue;
    }
    g_pages.push_back(page);
    Row* row = AddRow(g_pages.front(), page.title, desc);
    row->kind = RowKind::kPage;
    row->target_page = g_pages.size() - 1;
  }
  Row* all = AddRow(g_pages.front(), "All PC Settings",
                    "Every recomp setting: graphics API, resolution, frame rate, profiles "
                    "and more.");
  all->kind = RowKind::kOverlay;
}

// ---- Guest strings ------------------------------------------------------------

// Writes every string ID and its display text into one guest block and fills
// the localizer map. Returns false when the allocation fails.
bool PublishStrings(rex::memory::Memory* memory, uint8_t* base) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (size_t p = 0; p < g_pages.size(); ++p) {
    const Page& page = g_pages[p];
    const std::string prefix = "ID_RECOMP_PC_P" + std::to_string(p);
    entries.emplace_back(p == 0 ? std::string(kHubId) : prefix, page.title);
    for (size_t r = 0; r < page.rows.size(); ++r) {
      const Row& row = page.rows[r];
      const std::string row_prefix = prefix + "_R" + std::to_string(r);
      entries.emplace_back(row_prefix, row.title);
      entries.emplace_back(row_prefix + "_DESC", row.desc);
      for (size_t v = 0; v < row.labels.size(); ++v) {
        entries.emplace_back(row_prefix + "_V" + std::to_string(v), row.labels[v]);
      }
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
  for (auto& page : g_pages) {
    page.title_id = ids[next++];
    for (auto& row : page.rows) {
      row.title_id = ids[next++];
      row.desc_id = ids[next++];
      for (size_t v = 0; v < row.labels.size(); ++v) {
        row.label_ids.push_back(ids[next++]);
      }
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

// The page shown by an Options state, or null on the game's own screens.
Page* PageOf(uint8_t* base, uint32_t state) {
  if (state < 0x10000) {
    return nullptr;
  }
  const uint32_t screen = REX_LOAD_U32(state + kStateScreenOffset);
  if (screen < kFirstPageScreen || screen - kFirstPageScreen >= g_pages.size()) {
    return nullptr;
  }
  return &g_pages[screen - kFirstPageScreen];
}

Row* RowAt(Page& page, uint32_t row) {
  return row < page.rows.size() ? &page.rows[row] : nullptr;
}

bool RowEnabled(const Row& row) { return !row.enabled || row.enabled(); }

bool IsPcSettingsRootRow(uint8_t* base, uint32_t state, uint32_t row) {
  const uint32_t screen = REX_LOAD_U32(state + kStateScreenOffset);
  if (screen >= kScreenCount || row >= kSlotsPerScreen) {
    return false;
  }
  return static_cast<int32_t>(REX_LOAD_U32(LayoutSlot(screen, row))) == kPcSettingsOptionId;
}

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

void ShowPage(PPCContext& ctx, uint8_t* base, uint32_t state, size_t page, uint64_t sound) {
  PlaySound(ctx, base, sound);
  REX_STORE_U32(state + kStateScreenOffset, kFirstPageScreen + static_cast<uint32_t>(page));
  RefreshScreen(ctx, base, state, kRefreshScreen);
}

void StepRow(PPCContext& ctx, uint8_t* base, uint32_t state, Row& row, int delta) {
  PlaySound(ctx, base, delta < 0 ? kSelectorLeftSound : kSelectorRightSound);
  const int count = static_cast<int>(row.labels.size());
  row.set(((row.get() + delta) % count + count) % count);
  if (g_save_settings) {
    g_save_settings();
  }
  RefreshScreen(ctx, base, state, row.rebuild_on_change ? kRefreshScreen : kRefreshValues);
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
                "PC Settings pages not added");
    return;
  }
  BuildPages();
  if (!PublishStrings(memory, base)) {
    g_pages.clear();
    REXLOG_WARN("Options menu: guest string allocation failed; PC Settings pages not added");
    return;
  }
  g_type_selector = REX_LOAD_U32(OptionTypeSlot(kSelectorOptionId));
  g_type_option = REX_LOAD_U32(OptionTypeSlot(kPcSettingsOptionId));

  REX_STORE_U32(OptionTitleSlot(kPcSettingsOptionId), g_pages.front().title_id);
  for (uint32_t screen : kRootScreens) {
    REX_STORE_U32(LayoutSlot(screen, std::size(kRootLayout)),
                  static_cast<uint32_t>(kPcSettingsOptionId));
  }
  g_installed.store(true, std::memory_order_release);
  REXLOG_INFO("Options menu: PC Settings added ({} pages)", g_pages.size());
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
// hub; on our pages A opens pages and the overlay, and (as on the game's own
// selectors) does nothing on selector rows.
extern "C" REX_FUNC(sub_8260D290) {
  if (IsInstalled()) {
    const uint32_t state = ctx.r3.u32;
    const uint32_t row_index = ctx.r5.u32;
    if (Page* page = PageOf(base, state)) {
      Row* row = RowAt(*page, row_index);
      if (row && RowEnabled(*row)) {
        if (row->kind == RowKind::kPage) {
          ShowPage(ctx, base, state, row->target_page, kSubmenuSound);
        } else if (row->kind == RowKind::kOverlay) {
          PlaySound(ctx, base, kSubmenuSound);
          if (g_open_settings) {
            g_open_settings();
          }
        }
      }
      return;
    }
    if (state >= 0x10000 && IsPcSettingsRootRow(base, state, row_index)) {
      ShowPage(ctx, base, state, 0, kSubmenuSound);
      return;
    }
  }
  __imp__sub_8260D290(ctx, base);
}

// Input handler (r5 = event). Left/right step selectors. Back from a category
// returns to the hub; Back from the hub keeps the game's handling (the root
// Options screen). Nothing else is consumed, so the game never indexes its
// tables with a page's screen id.
extern "C" REX_FUNC(sub_8260C258) {
  if (IsInstalled()) {
    const uint32_t state = ctx.r3.u32;
    const uint32_t event = ctx.r5.u32;
    if (Page* page = PageOf(base, state)) {
      if (event == kInputBack) {
        if (page == &g_pages.front()) {
          __imp__sub_8260C258(ctx, base);
          return;
        }
        ShowPage(ctx, base, state, 0, kBackSound);
        ctx.r3.u64 = 1;
        return;
      }
      Row* row = RowAt(*page, REX_LOAD_U32(state + kStateRowOffset));
      if ((event == kInputLeft || event == kInputRight) && row &&
          row->kind == RowKind::kSelector && RowEnabled(*row)) {
        StepRow(ctx, base, state, *row, event == kInputLeft ? -1 : 1);
        ctx.r3.u64 = 1;
      } else {
        ctx.r3.u64 = 0;
      }
      return;
    }
  }
  __imp__sub_8260C258(ctx, base);
}

// Row title string ID (r4 = row).
extern "C" REX_FUNC(sub_8260D718) {
  if (IsInstalled()) {
    if (Page* page = PageOf(base, ctx.r3.u32)) {
      const Row* row = RowAt(*page, ctx.r4.u32);
      ctx.r3.u64 = row ? row->title_id : kStringEmpty;
      return;
    }
  }
  __imp__sub_8260D718(ctx, base);
}

// Row integer value (r4 = row): the selected label index.
extern "C" REX_FUNC(sub_8260D7E8) {
  if (IsInstalled()) {
    if (Page* page = PageOf(base, ctx.r3.u32)) {
      const Row* row = RowAt(*page, ctx.r4.u32);
      ctx.r3.s64 = row && row->kind == RowKind::kSelector ? row->get() : 0;
      return;
    }
  }
  __imp__sub_8260D7E8(ctx, base);
}

// Row value string ID (r4 = row).
extern "C" REX_FUNC(sub_8260D968) {
  if (IsInstalled()) {
    if (Page* page = PageOf(base, ctx.r3.u32)) {
      const Row* row = RowAt(*page, ctx.r4.u32);
      ctx.r3.u64 = row && row->kind == RowKind::kSelector ? row->label_ids[row->get()]
                                                           : kStringEmpty;
      return;
    }
  }
  __imp__sub_8260D968(ctx, base);
}

// Row enabled (r5 = row).
extern "C" REX_FUNC(sub_8260D648) {
  if (IsInstalled()) {
    if (Page* page = PageOf(base, ctx.r3.u32)) {
      const Row* row = RowAt(*page, ctx.r5.u32);
      ctx.r3.u64 = row && RowEnabled(*row) ? 1 : 0;
      return;
    }
  }
  __imp__sub_8260D648(ctx, base);
}

// ---- GameSettings_* APT natives that read the tables directly ------------------

// GameSettings_GetNumOptions().
extern "C" REX_FUNC(sub_825A6F70) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    if (Page* page = PageOf(base, OptionsState(ctx, base))) {
      AptReturnInt(ctx, base, static_cast<int32_t>(page->rows.size()));
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
    if (Page* page = PageOf(base, OptionsState(ctx, base))) {
      const Row* row = RowAt(*page, static_cast<uint32_t>(AptArgInt(ctx, base, 0)));
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
    if (Page* page = PageOf(base, OptionsState(ctx, base))) {
      AptReturnString(ctx, base, page->title_id);
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
    if (Page* page = PageOf(base, OptionsState(ctx, base))) {
      const Row* row = RowAt(*page, static_cast<uint32_t>(AptArgInt(ctx, base, 0)));
      AptReturnString(ctx, base, row ? row->desc_id : kStringEmpty);
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6E88(ctx, base);
}

// GameSettings_GetButtonHelpTextA(): "Select" on rows A acts on (pages and the
// overlay), nothing on selectors, as the game does for its "option" rows.
extern "C" REX_FUNC(sub_825A6FE0) {
  if (IsInstalled()) {
    ArgRegisters args(ctx);
    const uint32_t state = OptionsState(ctx, base);
    if (Page* page = PageOf(base, state)) {
      const Row* row = RowAt(*page, REX_LOAD_U32(state + kStateRowOffset));
      AptReturnString(ctx, base,
                      row && row->kind != RowKind::kSelector ? kStringCommonSelect
                                                             : kStringEmpty);
      return;
    }
    args.Restore();
  }
  __imp__sub_825A6FE0(ctx, base);
}
#endif
