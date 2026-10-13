// Guest heap diagnostics for the game's EA MemoryFramework.
//
// Every game allocation names a category (77 of them: "Default", "CACEditor",
// "WorldStream", ...). Categories live in a 108-byte table reached through the
// framework global [0x830CFEEC] -> manager (+4) -> category array (+0xC1B4).
// Each category embeds an ICoreAllocator-style proxy at +12 (vtable
// 0x823178DC: [2] Alloc(size, name, flags) = sub_8298E4E8, [1] Alloc(size,
// name, flags, align, offset) = sub_8298E510, [3] Free(ptr) = sub_8298E530)
// and points at up to three fixed-size heaps: +0 small-block, +4 main CPU
// heap, +8 GFX heap. Both Alloc slots funnel into sub_8298E020(manager,
// category, size, flags, align, offset), which tries the small-block heap and
// then the main heap and returns NULL when both are out of room.
//
// The game does not check every NULL: CACDBPartContainer (Create-a-Character
// part data, category CACEditor -> StreamerHeap) writes its records through
// it and crashes on a near-null store. So this file:
//  - always logs a failed allocation with its category, name tag, size and
//    the state of the heaps behind that category;
//  - every skate3_heap_diag_interval seconds, logs used/peak/largest-free
//    for every heap, which separates exhaustion (used ~ size) from
//    fragmentation (plenty free, but no block big enough);
//  - with skate3_heap_diag_track_tags, keeps live bytes per (category, name
//    tag) and logs the biggest ones with their growth, to attribute a leak.
//
// Heap holders (+0 heap object, +4 name, +20 base, +24 size) wrap one of
// three heap classes, told apart by the object's vtable:
//  - 0x823177E4 general heap: +4 allocs, +8 frees, +12 size, +16 used,
//    +20 peak, +24 PPMalloc allocator (sub_82B17350 = largest free block);
//  - 0x8231784C GFX heap: +4 allocs, +8 frees, +12 used, +16 peak, +20 size;
//  - 0x82317884 small-block heap: no counters decoded, size only.
// Addresses are for the TU3 image.

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_INT32(skate3_heap_diag_interval, 300, "Skate 3",
                     "Log the game's own heap usage (used / peak / largest free "
                     "block per heap) every this many seconds. 0 = only when an "
                     "allocation fails.")
    .range(0, 86400)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_heap_diag_track_tags, false, "Skate 3",
                    "Track live bytes per allocation name tag in the game's "
                    "heaps and include the largest tags (with growth since the "
                    "previous report) in the heap report. Costs a little CPU "
                    "per allocation; counts only allocations made while on.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#if SKATE3_HAS_TITLE_UPDATE
namespace {

constexpr uint32_t kMemoryFrameworkPtr = 0x830CFEEC;
constexpr uint32_t kManagerCategoriesOffset = 0xC1B4;
constexpr uint32_t kCategoryStride = 108;
constexpr uint32_t kCategoryProxyVtableOffset = 12;
constexpr uint32_t kCategoryProxyVtable = 0x823178DC;
constexpr uint32_t kCategoryNameOffset = 40;
constexpr uint32_t kCategoryHeapOffsets[] = {0, 4, 8};
constexpr uint32_t kMaxCategories = 128;

constexpr uint32_t kHolderNameOffset = 4;
constexpr uint32_t kHolderBaseOffset = 20;
constexpr uint32_t kHolderSizeOffset = 24;

constexpr uint32_t kGeneralHeapVtable = 0x823177E4;
constexpr uint32_t kGfxHeapVtable = 0x8231784C;

constexpr uint32_t kImageStart = 0x82000000;
constexpr uint32_t kImageEnd = 0x831B0000;

// Allocations that may fail by design: the heap returns NULL instead of
// allocating when the request exceeds its largest free block.
constexpr uint32_t kAllocFlagFailSoft = 0x200;

constexpr int kFailuresLoggedInFull = 8;
constexpr size_t kTopTags = 20;

inline uint32_t LoadGuestU32(uint8_t* base, uint32_t addr) {
  uint32_t raw;
  std::memcpy(&raw, base + addr, 4);
  return __builtin_bswap32(raw);
}

inline bool PlausibleGuestAddr(uint32_t addr) {
  return addr >= 0x10000 && addr < 0xE0000000;
}

std::string ReadGuestString(uint8_t* base, uint32_t addr) {
  if (!PlausibleGuestAddr(addr)) {
    return addr ? "<bad>" : "<none>";
  }
  std::string text;
  for (uint32_t i = 0; i < 64; ++i) {
    const char c = static_cast<char>(base[addr + i]);
    if (c == '\0') {
      break;
    }
    text.push_back(c >= 0x20 && c < 0x7F ? c : '?');
  }
  return text;
}

std::string FormatBytes(uint64_t bytes) {
  char text[32];
  if (bytes >= 1024 * 1024) {
    std::snprintf(text, sizeof(text), "%.2f MB", bytes / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
  } else {
    std::snprintf(text, sizeof(text), "%u B", static_cast<uint32_t>(bytes));
  }
  return text;
}

struct Heap {
  uint32_t holder = 0;
  uint32_t object = 0;
  uint32_t vtable = 0;
  uint32_t base_addr = 0;
  uint32_t size = 0;
  std::string name;
};

struct Category {
  std::string name;
  uint32_t holders[3] = {};
};

struct Layout {
  std::vector<Category> categories;
  std::vector<Heap> heaps;

  const Heap* HeapAt(uint32_t holder) const {
    for (const Heap& heap : heaps) {
      if (heap.holder == holder) {
        return &heap;
      }
    }
    return nullptr;
  }
  const Heap* HeapContaining(uint32_t ptr) const {
    for (const Heap& heap : heaps) {
      if (ptr >= heap.base_addr && ptr - heap.base_addr < heap.size) {
        return &heap;
      }
    }
    return nullptr;
  }
};

// Re-read on every report: categories are cheap to walk and some are filled
// in after the first allocation.
Layout ReadLayout(uint8_t* base) {
  Layout layout;
  const uint32_t framework = LoadGuestU32(base, kMemoryFrameworkPtr);
  if (!PlausibleGuestAddr(framework)) {
    return layout;
  }
  const uint32_t manager = LoadGuestU32(base, framework + 4);
  if (!PlausibleGuestAddr(manager)) {
    return layout;
  }
  const uint32_t categories = LoadGuestU32(base, manager + kManagerCategoriesOffset);
  if (!PlausibleGuestAddr(categories)) {
    return layout;
  }
  for (uint32_t i = 0; i < kMaxCategories; ++i) {
    const uint32_t entry = categories + i * kCategoryStride;
    if (LoadGuestU32(base, entry + kCategoryProxyVtableOffset) != kCategoryProxyVtable) {
      break;
    }
    Category category;
    category.name = ReadGuestString(base, LoadGuestU32(base, entry + kCategoryNameOffset));
    for (size_t k = 0; k < std::size(kCategoryHeapOffsets); ++k) {
      const uint32_t holder = LoadGuestU32(base, entry + kCategoryHeapOffsets[k]);
      category.holders[k] = holder;
      if (!PlausibleGuestAddr(holder) || layout.HeapAt(holder)) {
        continue;
      }
      Heap heap;
      heap.holder = holder;
      heap.object = LoadGuestU32(base, holder);
      heap.vtable = PlausibleGuestAddr(heap.object) ? LoadGuestU32(base, heap.object) : 0;
      heap.base_addr = LoadGuestU32(base, holder + kHolderBaseOffset);
      heap.size = LoadGuestU32(base, holder + kHolderSizeOffset);
      heap.name = ReadGuestString(base, LoadGuestU32(base, holder + kHolderNameOffset));
      layout.heaps.push_back(std::move(heap));
    }
    layout.categories.push_back(std::move(category));
  }
  return layout;
}

// Calls back into the guest allocator, so the caller's registers are saved
// and restored around it (the hooks below run between the game's call and
// its use of r3).
uint32_t LargestFreeBlock(PPCContext& ctx, uint8_t* base, const Heap& heap) {
  if (heap.vtable != kGeneralHeapVtable) {
    return 0;
  }
  const PPCContext saved = ctx;
  ctx.r3.u64 = heap.object + 24;
  sub_82B17350(ctx, base);
  const uint32_t largest = ctx.r3.u32;
  ctx = saved;
  return largest;
}

void LogHeap(PPCContext& ctx, uint8_t* base, const Heap& heap, bool as_error) {
  std::string line;
  if (heap.vtable == kGeneralHeapVtable || heap.vtable == kGfxHeapVtable) {
    const bool general = heap.vtable == kGeneralHeapVtable;
    const uint32_t allocs = LoadGuestU32(base, heap.object + 4);
    const uint32_t frees = LoadGuestU32(base, heap.object + 8);
    const uint32_t used = LoadGuestU32(base, heap.object + (general ? 16 : 12));
    const uint32_t peak = LoadGuestU32(base, heap.object + (general ? 20 : 16));
    const uint32_t size = heap.size ? heap.size : 1;
    line = fmt::format("{:<17} used {:>9} / {:>9} ({:5.1f}%) peak {:>9} live {}", heap.name,
                       FormatBytes(used), FormatBytes(heap.size), 100.0 * used / size,
                       FormatBytes(peak), allocs - frees);
    if (general) {
      line += fmt::format(" largest-free {}", FormatBytes(LargestFreeBlock(ctx, base, heap)));
    }
  } else {
    line =
        fmt::format("{:<17} size {} (small-block, no counters)", heap.name, FormatBytes(heap.size));
  }
  if (as_error) {
    REXLOG_ERROR("guest-heap:   {}", line);
  } else {
    REXLOG_INFO("guest-heap:   {}", line);
  }
}

// --- name-tag tracking -------------------------------------------------------

struct LiveAlloc {
  uint32_t size;
  uint16_t tag;
  uint16_t category;
};

// A tag can span heaps (a category's small requests land in its small-block
// heap, large ones in its main heap), so totals are kept per heap as well.
struct TagKey {
  uint16_t tag;
  uint16_t category;
  uint16_t heap;
  bool operator==(const TagKey&) const = default;
};

constexpr uint16_t kNoHeap = 0xFFFF;

struct TagKeyHash {
  size_t operator()(const TagKey& key) const {
    return (static_cast<size_t>(key.heap) << 32) | (static_cast<size_t>(key.category) << 16) |
           key.tag;
  }
};

// g_tracking is written under g_track_mutex and read lock-free on the
// allocation path; everything else below is guarded by g_track_mutex.
std::mutex g_track_mutex;
std::atomic<bool> g_tracking{false};
std::unordered_map<uint32_t, LiveAlloc> g_live;
std::vector<std::string> g_tag_names{"<none>"};
std::unordered_map<uint32_t, uint16_t> g_tag_by_addr;
std::unordered_map<std::string, uint16_t> g_tag_by_text;
std::unordered_map<TagKey, uint64_t, TagKeyHash> g_previous_bytes;

// Name tag of the Alloc call in flight on this thread (sub_8298E020 itself
// does not receive it).
thread_local uint32_t t_alloc_tag = 0;

// Static names are keyed by address; anything outside the image is copied,
// since it may live in memory that is freed or reused later.
uint16_t InternTagLocked(uint8_t* base, uint32_t addr) {
  if (!PlausibleGuestAddr(addr)) {
    return 0;
  }
  const bool in_image = addr >= kImageStart && addr < kImageEnd;
  if (in_image) {
    if (auto it = g_tag_by_addr.find(addr); it != g_tag_by_addr.end()) {
      return it->second;
    }
  }
  std::string text = ReadGuestString(base, addr);
  uint16_t id;
  if (auto it = g_tag_by_text.find(text); it != g_tag_by_text.end()) {
    id = it->second;
  } else if (g_tag_names.size() < 0xFFFF) {
    id = static_cast<uint16_t>(g_tag_names.size());
    g_tag_names.push_back(text);
    g_tag_by_text.emplace(std::move(text), id);
  } else {
    return 0;
  }
  if (in_image) {
    g_tag_by_addr.emplace(addr, id);
  }
  return id;
}

bool TrackingEnabled() {
  const bool wanted = REXCVAR_GET(skate3_heap_diag_track_tags);
  if (wanted == g_tracking.load(std::memory_order_relaxed)) {
    return wanted;
  }
  std::lock_guard lock(g_track_mutex);
  if (wanted != g_tracking.load(std::memory_order_relaxed)) {
    g_tracking.store(wanted, std::memory_order_relaxed);
    g_live.clear();
    g_previous_bytes.clear();
    REXLOG_INFO("guest-heap: name-tag tracking {}", wanted ? "on" : "off");
  }
  return wanted;
}

void TrackAlloc(uint8_t* base, uint32_t ptr, uint32_t size, uint32_t category, uint32_t tag) {
  std::lock_guard lock(g_track_mutex);
  if (!g_tracking.load(std::memory_order_relaxed)) {
    return;
  }
  g_live[ptr] = {size, InternTagLocked(base, tag), static_cast<uint16_t>(category)};
}

void TrackFree(uint32_t ptr) {
  std::lock_guard lock(g_track_mutex);
  g_live.erase(ptr);
}

// Logs the biggest (heap, category, tag) groups, optionally only those living
// in one heap. Growth is measured against the previous full report.
void LogTopTags(const Layout& layout, const Heap* only_heap, bool as_error) {
  struct Entry {
    TagKey key;
    uint64_t bytes = 0;
    uint32_t count = 0;
    uint32_t lowest = 0xFFFFFFFF;
    uint32_t highest = 0;
  };
  std::unordered_map<TagKey, Entry, TagKeyHash> totals;
  std::vector<std::string> tag_names;
  std::unordered_map<TagKey, uint64_t, TagKeyHash> previous_bytes;
  {
    std::lock_guard lock(g_track_mutex);
    if (!g_tracking.load(std::memory_order_relaxed)) {
      return;
    }
    for (const auto& [ptr, alloc] : g_live) {
      const Heap* heap = layout.HeapContaining(ptr);
      if (only_heap && heap != only_heap) {
        continue;
      }
      const TagKey key{alloc.tag, alloc.category,
                       heap ? static_cast<uint16_t>(heap - layout.heaps.data()) : kNoHeap};
      Entry& entry = totals[key];
      entry.key = key;
      entry.bytes += alloc.size;
      entry.count += 1;
      entry.lowest = std::min(entry.lowest, ptr);
      entry.highest = std::max(entry.highest, ptr + alloc.size);
    }
    tag_names = g_tag_names;
    if (!only_heap) {
      previous_bytes.swap(g_previous_bytes);
      for (const auto& [key, entry] : totals) {
        g_previous_bytes[key] = entry.bytes;
      }
    }
  }
  std::vector<Entry> sorted;
  sorted.reserve(totals.size());
  for (auto& [key, entry] : totals) {
    sorted.push_back(entry);
  }
  const size_t shown = std::min(kTopTags, sorted.size());
  std::partial_sort(sorted.begin(), sorted.begin() + shown, sorted.end(),
                    [](const Entry& a, const Entry& b) { return a.bytes > b.bytes; });
  REXLOG_INFO("guest-heap: top tags by live bytes{}{}:", only_heap ? " in " : "",
              only_heap ? only_heap->name : "");
  for (size_t i = 0; i < shown; ++i) {
    const Entry& entry = sorted[i];
    const std::string& heap =
        entry.key.heap < layout.heaps.size() ? layout.heaps[entry.key.heap].name : "?";
    const std::string& category = entry.key.category < layout.categories.size()
                                      ? layout.categories[entry.key.category].name
                                      : "?";
    std::string growth;
    if (!only_heap) {
      const auto it = previous_bytes.find(entry.key);
      const int64_t delta = static_cast<int64_t>(entry.bytes) -
                            static_cast<int64_t>(it != previous_bytes.end() ? it->second : 0);
      growth = fmt::format(" ({}{})", delta < 0 ? "-" : "+",
                           FormatBytes(static_cast<uint64_t>(delta < 0 ? -delta : delta)));
    }
    const std::string line =
        fmt::format("{:<16} {:<14} {:<40} {:>9} in {:>6} blocks{} at {:08X}-{:08X}", heap, category,
                    tag_names[entry.key.tag], FormatBytes(entry.bytes), entry.count, growth,
                    entry.lowest, entry.highest);
    if (as_error) {
      REXLOG_ERROR("guest-heap:   {}", line);
    } else {
      REXLOG_INFO("guest-heap:   {}", line);
    }
  }
}

// --- reports -----------------------------------------------------------------

std::mutex g_report_mutex;
const auto g_start_time = std::chrono::steady_clock::now();
std::atomic<int64_t> g_next_report_ms{0};
std::atomic<int> g_failures{0};

int64_t UptimeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               g_start_time)
      .count();
}

void LogFullReport(PPCContext& ctx, uint8_t* base, const char* reason) {
  std::lock_guard lock(g_report_mutex);
  const Layout layout = ReadLayout(base);
  if (layout.heaps.empty()) {
    return;
  }
  REXLOG_INFO("guest-heap: {} at {:.1f} min - {} heaps, {} categories, {} failed allocs", reason,
              UptimeMs() / 60000.0, layout.heaps.size(), layout.categories.size(),
              g_failures.load(std::memory_order_relaxed));
  for (const Heap& heap : layout.heaps) {
    LogHeap(ctx, base, heap, false);
  }
  LogTopTags(layout, nullptr, false);
}

void MaybeReport(PPCContext& ctx, uint8_t* base) {
  // The game allocates thousands of times a second; look at the clock only
  // on every 64th allocation per thread.
  thread_local uint32_t calls = 0;
  if ((++calls & 63) != 0) {
    return;
  }
  const int32_t interval_s = REXCVAR_GET(skate3_heap_diag_interval);
  if (interval_s <= 0) {
    return;
  }
  const int64_t now = UptimeMs();
  int64_t due = g_next_report_ms.load(std::memory_order_relaxed);
  if (now < due) {
    return;
  }
  const int64_t next = now + int64_t{interval_s} * 1000;
  if (!g_next_report_ms.compare_exchange_strong(due, next, std::memory_order_relaxed)) {
    return;
  }
  // The first allocation comes before the framework has heaps; start the
  // clock then and report one interval later.
  if (due == 0) {
    return;
  }
  LogFullReport(ctx, base, "heap report");
}

void OnAllocFailure(PPCContext& ctx, uint8_t* base, uint32_t category_index, uint32_t size,
                    uint32_t flags, uint32_t align, uint32_t tag) {
  if (flags & kAllocFlagFailSoft) {
    return;
  }
  const int failure = g_failures.fetch_add(1, std::memory_order_relaxed) + 1;
  if (failure > kFailuresLoggedInFull) {
    if (failure % 100 == 0) {
      REXLOG_ERROR("guest-heap: {} failed allocations so far", failure);
    }
    return;
  }
  std::lock_guard lock(g_report_mutex);
  const Layout layout = ReadLayout(base);
  const Category* category =
      category_index < layout.categories.size() ? &layout.categories[category_index] : nullptr;
  REXLOG_ERROR(
      "guest-heap: ALLOCATION FAILED #{} at {:.1f} min: category {} ({}) tag '{}' size {} ({}) "
      "flags 0x{:X} align {}",
      failure, UptimeMs() / 60000.0, category_index, category ? category->name : "?",
      ReadGuestString(base, tag), size, FormatBytes(size), flags, align);
  if (!category) {
    return;
  }
  for (uint32_t holder : category->holders) {
    if (const Heap* heap = layout.HeapAt(holder)) {
      LogHeap(ctx, base, *heap, true);
    }
  }
  // The main CPU heap (+4) is the one a large request lands in.
  if (const Heap* main_heap = layout.HeapAt(category->holders[1])) {
    LogTopTags(layout, main_heap, true);
  }
}

}  // namespace

// Category proxy Alloc(size, name, flags): record the name tag for the
// sub_8298E020 hook below.
extern "C" REX_FUNC(sub_8298E4E8) {
  const uint32_t outer_tag = t_alloc_tag;
  t_alloc_tag = ctx.r5.u32;
  __imp__sub_8298E4E8(ctx, base);
  t_alloc_tag = outer_tag;
}

// Category proxy Alloc(size, name, flags, align, offset).
extern "C" REX_FUNC(sub_8298E510) {
  const uint32_t outer_tag = t_alloc_tag;
  t_alloc_tag = ctx.r5.u32;
  __imp__sub_8298E510(ctx, base);
  t_alloc_tag = outer_tag;
}

// Manager allocate(manager, category, size, flags, align, offset): the single
// point every category allocation goes through.
extern "C" REX_FUNC(sub_8298E020) {
  const uint32_t category = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  const uint32_t flags = ctx.r6.u32;
  const uint32_t align = ctx.r7.u32;
  const uint32_t tag = t_alloc_tag;
  __imp__sub_8298E020(ctx, base);
  const uint32_t result = ctx.r3.u32;
  if (result == 0) {
    if (size != 0) {
      OnAllocFailure(ctx, base, category, size, flags, align, tag);
    }
  } else if (TrackingEnabled()) {
    TrackAlloc(base, result, size, category, tag);
  }
  MaybeReport(ctx, base);
  ctx.r3.u64 = result;
}

// Category proxy Free(ptr). Untracked before the free so a racing allocation
// that reuses the address cannot be erased by mistake.
extern "C" REX_FUNC(sub_8298E530) {
  if (ctx.r4.u32 != 0 && TrackingEnabled()) {
    TrackFree(ctx.r4.u32);
  }
  __imp__sub_8298E530(ctx, base);
}

// Frees the five blocks of a multi-part allocation (array at r4).
extern "C" REX_FUNC(sub_8298E608) {
  if (TrackingEnabled() && PlausibleGuestAddr(ctx.r4.u32)) {
    for (uint32_t i = 0; i < 5; ++i) {
      if (const uint32_t ptr = LoadGuestU32(base, ctx.r4.u32 + i * 4)) {
        TrackFree(ptr);
      }
    }
  }
  __imp__sub_8298E608(ctx, base);
}
#endif  // SKATE3_HAS_TITLE_UPDATE
