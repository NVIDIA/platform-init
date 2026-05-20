// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

#include "emmc-ioctl.h"

#include <cstdint>
#include <string_view>

namespace emmc::init
{

// Exit codes (kept compatible with the legacy CLI).
inline constexpr int kExitOk = 0;
inline constexpr int kExitEmmcMissing = 1;
inline constexpr int kExitEudaFailed = 2;
inline constexpr int kExitEudaNotActivated = 4;
inline constexpr int kExitPartitionCreate = 5;
inline constexpr int kExitRwfsFormat = 6;
inline constexpr int kExitRepartitionBudget = 8;

enum class EudaResult
{
    Ok = 0,
    Failed = 1,
    NotActivated = 2,
};

int run(std::string_view device);

// expected_bytes is derived inside the function from EXT_CSD's
// MAX_ENH_SIZE_MULT — the chip itself reports what we should program.
// Real on-chip size (SEC_COUNT × 512) is logged but not used for the
// activation comparison.
EudaResult eudaConfig(std::string_view device, bool recheck);
EudaResult eudaConfigWithHwReset(std::string_view device);

// Idempotent — skips if /sys/block/.../mmcblk0p1/size already exists.
bool createPartitions(std::string_view device, uint64_t total_sectors);

bool backupRwfs(std::string_view dest);
bool restoreRwfs(std::string_view src, std::string_view rwdev);

// HW reset is split into three steps so callers can interleave them with
// the kernel sdhci-aspeed unbind/bind. See eudaConfigWithHwReset() for
// the canonical ordering: enable (driver bound) → unbind → pulse → bind.
bool ensureHwResetEnabled(std::string_view device);
bool pulseHwResetGpio();
void unbindSdhciAspeed();
void bindSdhciAspeed();

} // namespace emmc::init
