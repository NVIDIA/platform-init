// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA
//
// emmc-layout-machine.h — VR-NVL-BMC AST2700-A2 partition + mount layout.

#pragma once

#include "emmc-layout-types.h"

#include <array>

namespace emmc::layout
{

// Kernel-side name of the SDHCI controller this BSP's eMMC sits behind.
// euda.cpp writes this to /sys/bus/platform/drivers/sdhci-aspeed/{un,}bind
// around the EUDA-activation hardware reset. Update per-machine if the
// platform DTS device name differs.
inline constexpr const char* kSdhciControllerName = "12090100.sdhci";

inline constexpr std::array<EmmcPart, 7> kPartitions{{
    {1, EmmcPart::Type::Primary,    64ULL * kSectorsPerMiB,  nullptr,                "rwfs"},
    {2, EmmcPart::Type::Primary,  2048ULL * kSectorsPerMiB, "/var/emmc/fdr",        nullptr},
    {3, EmmcPart::Type::Primary,  1024ULL * kSectorsPerMiB, "/var/emmc/user-logs",  nullptr},
    {4, EmmcPart::Type::Extended,        0,                  nullptr,                nullptr},
    {5, EmmcPart::Type::Logical,  2048ULL * kSectorsPerMiB, "/var/emmc/ist",        nullptr},
    {6, EmmcPart::Type::Logical,     1ULL * kSectorsPerMiB, "/var/emmc/uboot-env",  nullptr},
    {7, EmmcPart::Type::Logical,  1900ULL * kSectorsPerMiB, "/var/emmc/misc",       nullptr},
}};

inline constexpr unsigned int kSizeTolerancePercent = 2;

} // namespace emmc::layout
