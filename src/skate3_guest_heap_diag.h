#pragma once

#include <cstdint>
#include <string_view>

// Free bytes (size - used) in the main heap behind a game allocation
// category, e.g. "WorldStream" -> StreamerHeap. Returns false before the
// game's memory framework is up, for an unknown category, or for a heap
// class without counters.
bool Skate3GuestHeapCategoryFreeBytes(uint8_t* base, std::string_view category,
                                      uint32_t* free_bytes);
