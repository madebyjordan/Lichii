// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

#include "common/common_types.h"

namespace Vulkan {

// Selects the oldest eligible pipelines, visiting each candidate once in any order.
class PipelineEvictionPolicy {
public:
    PipelineEvictionPolicy(std::span<u64> last_uses, size_t count) {
        count = std::min(count, last_uses.size());
        if (count == 0) {
            return;
        }
        std::ranges::nth_element(last_uses, last_uses.begin() + (count - 1));
        cutoff = last_uses[count - 1];
        const size_t older = std::ranges::count_if(last_uses, [this](u64 last_use) {
            return last_use < cutoff;
        });
        equal_remaining = count - older;
    }

    bool Take(u64 last_use) {
        if (last_use < cutoff) {
            return true;
        }
        if (last_use == cutoff && equal_remaining != 0) {
            --equal_remaining;
            return true;
        }
        return false;
    }

private:
    u64 cutoff{};
    size_t equal_remaining{};
};

} // namespace Vulkan
