// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

// Partition + mount data live in emmc-layout-machine.h.
#include "emmc-layout-validate.h"

#include <format>
#include <string>

namespace emmc
{

// kSectorsPerMiB / kSectorsPerGiB now live in emmc::layout; re-export
// for legacy call sites that referenced emmc::kSectorsPerMiB directly.
using emmc::layout::kSectorsPerGiB;
using emmc::layout::kSectorsPerMiB;

inline constexpr const char* kEmmcDevice = "/dev/mmcblk0";

// rwfs device path is derived from the layout — the rwfs partition
// number comes from emmc-layout-machine.h's kPartitions entry whose
// purpose == "rwfs" (BSP-configurable; default is p1).
inline std::string rwfsDevicePath()
{
    return std::format("{}p{}", kEmmcDevice, emmc::layout::rwfsPartitionNum());
}

inline constexpr const char* kRwfsMount = "/run/initramfs/rw";
inline constexpr const char* kRwfsBackupImage = "/run/rwfs-backup.img";

inline constexpr const char* kMonitorStateDir = "/var/lib/nvidia-emmc-monitor";
inline constexpr const char* kFwEnvConfig = "/etc/fw_env.config";
inline constexpr const char* kRrEnvConfig = "/etc/rr_env.config";

inline constexpr int kMaxRebootRecoveryAttempts = 3;
inline constexpr int kStrikeThreshold = 3;
inline constexpr int kMonitorTickSeconds = 120;

} // namespace emmc
