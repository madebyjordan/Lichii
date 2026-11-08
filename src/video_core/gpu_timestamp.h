// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Tegra {

/// present_ns and divisor (Ampere only): time since the last present (present_ns, the same clock)
/// runs `divisor` times slower, so the GPU work a game times within a frame looks that much
/// shorter. Timestamps stay monotonic and catch up with the host clock at every present.
template <typename Timing>
[[nodiscard]] u64 ReadGpuTimestamp(const Timing& timing, bool ampere, Settings::GpuClock clock,
                                   u64 present_ns = 0, u64 divisor = 1) {
    if (ampere) {
        const u64 now = static_cast<u64>(timing.GetGlobalTimeNs().count());
        if (divisor <= 1 || now <= present_ns) {
            return now;
        }
        return present_ns + (now - present_ns) / divisor;
    }
    const u64 ticks = timing.GetGPUTicks();
    switch (clock) {
    case Settings::GpuClock::Boost:
        return ticks / 256;
    case Settings::GpuClock::Overclock:
        return ticks / 512;
    default:
        return ticks;
    }
}

} // namespace Tegra
