#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace maclauncher::arm32 {
// The original Region owns these values and the permission bytes. Dynarmic's
// page-table entries borrow this prefix only while Run holds the mapping guard.
struct PageTableMapping {
  uint32_t base, size, accessible;
  uint8_t *bytes;
  const uint8_t *permissionBytes = nullptr;
};
static_assert(std::is_standard_layout_v<PageTableMapping>);
constexpr uintptr_t boundedPageTag = 1;
constexpr unsigned compactCellBits = 4;
} // namespace maclauncher::arm32
