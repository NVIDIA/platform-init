// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA
//
// emmc-layout-ondisk.h — the single on-disk partition-geometry check,
// shared by nvidia-emmc-init and nvidia-emmc-monitor.
//
// The two binaries used to disagree about what "the layout is fine" means:
// createPartitions() accepted any table whose sysfs partition nodes merely
// existed, while the monitor's runRepartitionCheck() compared every
// partition's size against kPartitions. A board still carrying an older
// layout (e.g. p2 sized 2 GiB where the layout says 3 GiB) therefore passed
// init — "existing layout detected; skipping repartition" — and was then
// rejected by the monitor, which armed emmc_repartition and rebooted. Since
// init never acted on the size difference, the next boot repeated it.
//
// Keeping one implementation here means the two can no longer drift apart.
// The tolerance and the sector counts both come from the per-machine
// emmc-layout-machine.h.

#pragma once

#include "emmc-config.h"

#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace emmc::layout
{

enum class OnDiskState
{
    Match,      // every configured partition present and correctly sized
    Absent,     // none of the configured partitions present — blank device
    Incomplete, // some present, none wrongly sized — partial/stale table
    Mismatch,   // at least one *present* partition has the wrong size
};

inline constexpr double kSizeTolerance =
    static_cast<double>(kSizeTolerancePercent) / 100.0;

// "mmcblk0" from "/dev/mmcblk0". Derived rather than hard-coded so this
// stays correct on any future eMMC device name.
inline std::string_view deviceBaseName(std::string_view device)
{
    if (auto slash = device.rfind('/'); slash != std::string_view::npos)
    {
        device.remove_prefix(slash + 1);
    }
    return device;
}

inline std::string partSizeSysfsPath(std::string_view device, int part_num)
{
    const std::string_view base = deviceBaseName(device);
    return std::format("/sys/block/{}/{}p{}/size", base, base, part_num);
}

// Sector count as reported by sysfs; 0 when the node is absent or unreadable.
inline unsigned long long readPartSectors(std::string_view device, int part_num)
{
    std::ifstream ifs(partSizeSysfsPath(device, part_num));
    if (!ifs)
    {
        return 0;
    }
    unsigned long long sectors = 0;
    ifs >> sectors;
    return sectors;
}

inline double sizeRatio(unsigned long long actual, unsigned long long expected)
{
    return std::abs(static_cast<double>(actual) -
                    static_cast<double>(expected)) /
           static_cast<double>(expected);
}

// Compare the on-disk table against kPartitions. The Extended container is
// metadata-only (no sysfs node, sectors == 0) and is skipped.
//
// The three non-Match results are deliberately distinct, because only one
// of them justifies erasing the device:
//
//   Absent     — blank or freshly erased device (including the one we just
//                erased a few lines earlier in run(): wipeWholeDevice()
//                issues BLKRRPART, so the stale nodes are gone). Writing
//                the table is enough; erasing again would only add ~10 s to
//                every manufacturing first boot.
//   Incomplete — some nodes present, none of them wrongly sized. A partial
//                MBR/EBR write looks like this, but so does a device whose
//                nodes the kernel has not finished enumerating. Rewriting
//                the table is the long-standing behaviour here and is
//                harmless; erasing on what may be a timing artefact is not.
//   Mismatch   — positive evidence of wrong geometry: a partition that *is*
//                present carries a size the layout does not allow. This is
//                the only case worth an erase.
//
// This split follows repartition_check.sh, which likewise failed only on a
// size difference and merely logged "not found in fdisk output" for a
// partition that was absent.
inline OnDiskState onDiskState(std::string_view device)
{
    unsigned int configured = 0;
    unsigned int present = 0;
    bool mismatch = false;

    for (const auto& p : kPartitions)
    {
        if (p.type == EmmcPart::Type::Extended)
        {
            continue;
        }
        ++configured;

        const unsigned long long actual = readPartSectors(device, p.num);
        if (actual == 0)
        {
            continue;
        }
        ++present;

        if (sizeRatio(actual, p.sectors) > kSizeTolerance)
        {
            std::cerr << std::format(
                "emmc-layout: p{} geometry mismatch — actual {} sectors, "
                "expected {} (tolerance {}%)\n",
                p.num, actual, p.sectors, kSizeTolerancePercent);
            mismatch = true;
        }
    }

    if (mismatch)
    {
        return OnDiskState::Mismatch;
    }
    if (present == 0)
    {
        return OnDiskState::Absent;
    }
    if (present != configured)
    {
        std::cerr << std::format(
            "emmc-layout: only {} of {} configured partitions are present "
            "on {}\n",
            present, configured, device);
        return OnDiskState::Incomplete;
    }
    return OnDiskState::Match;
}

} // namespace emmc::layout
