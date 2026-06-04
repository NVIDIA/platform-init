// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-init.h"

#include <gtest/gtest.h>

// Note: the run() orchestration touches /dev/mmcblk0 which only exists on
// the AST2700 target. Unit tests at this layer focus on the helpers that
// are environment-independent. Full integration coverage requires a
// hardware target — out of scope for unit tests.

TEST(EmmcInitFlow, ExitCodesAreCompatibleWithLegacy)
{
    // Smoke check: kExit* constants match the legacy obmc-init.sh contract.
    EXPECT_EQ(emmc::init::kExitOk, 0);
    EXPECT_EQ(emmc::init::kExitEmmcMissing, 1);
    EXPECT_EQ(emmc::init::kExitEudaFailed, 2);
    EXPECT_EQ(emmc::init::kExitEudaNotActivated, 4);
    EXPECT_EQ(emmc::init::kExitPartitionCreate, 5);
    EXPECT_EQ(emmc::init::kExitRwfsFormat, 6);
    EXPECT_EQ(emmc::init::kExitRepartitionBudget, 8);
}

TEST(EmmcInitFlow, RunOnNonexistentDeviceReturnsEmmcMissing)
{
    int rc = emmc::init::run("/dev/this-does-not-exist");
    EXPECT_EQ(rc, emmc::init::kExitEmmcMissing);
}
