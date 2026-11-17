// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-config.h"
#include "emmc-monitor.h"

#include <string_view>

#include <gtest/gtest.h>

TEST(EmmcMonitorLoop, DefaultLayoutHasExpectedMounts)
{
    using namespace emmc::layout;

    // The default AST2700 layout shipped with platform-init has 7
    // partitions: 1 (rwfs), 2 (fdr), 3 (user-logs), 4 (extended),
    // 5 (ist), 6 (uboot-env), 7 (misc). Yocto builds may swap in a
    // different emmc-layout-machine.h, so this test only enforces the
    // schema invariants the source tree ships with by default.
    ASSERT_EQ(kPartitions.size(), 7U);

    int mounted_count = 0;
    int rwfs_count = 0;
    for (const auto& p : kPartitions)
    {
        if (p.mount != nullptr)
        {
            ++mounted_count;
        }
        if (p.purpose != nullptr && std::string_view(p.purpose) == "rwfs")
        {
            ++rwfs_count;
            EXPECT_EQ(p.mount, nullptr); // rwfs is owned by initramfs
        }
    }
    EXPECT_EQ(mounted_count, 5);
    EXPECT_EQ(rwfs_count, 1);

    // Spot-check the default AST2700 layout's first and last mount.
    EXPECT_EQ(kPartitions[1].num, 2);
    EXPECT_STREQ(kPartitions[1].mount, "/var/emmc/fdr");
    EXPECT_EQ(kPartitions[6].num, 7);
    EXPECT_STREQ(kPartitions[6].mount, "/var/emmc/misc");
}

TEST(EmmcMonitorLoop, ConstantsAreSane)
{
    EXPECT_EQ(emmc::kStrikeThreshold, 3);
    EXPECT_EQ(emmc::kMaxRebootRecoveryAttempts, 3);
    EXPECT_GT(emmc::kMonitorTickSeconds, 0);
}

// Integration test: requires a writable /var/lib/nvidia-emmc-monitor and a
// libubootenv environment for triggerAutoRecovery() — out of scope for
// unit tests. checkOneMountPoint() is exercised end-to-end on hardware
// during the periodic monitor loop.
