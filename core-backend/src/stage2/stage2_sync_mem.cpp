#include "stage2_sync.hpp"

namespace stage2 {

json StageSync::memory_breakdown() const {
  return builder_.memory_breakdown();
}

} // namespace stage2
