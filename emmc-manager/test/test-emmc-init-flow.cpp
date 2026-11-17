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

TEST(EmmcInitFlow, OpenbmconceFactoryResetFormatsRwfsOnly)
{
    auto request = emmc::init::parseOpenbmconce("factory-reset");
    EXPECT_FALSE(request.clean_rwfs);
    EXPECT_TRUE(request.factory_reset);
    EXPECT_FALSE(request.complete_reset);
    EXPECT_TRUE(request.formatsRwfs());
}

TEST(EmmcInitFlow, OpenbmconceCompleteResetFormatsRwfsAndLogs)
{
    auto request = emmc::init::parseOpenbmconce("complete-reset");
    EXPECT_FALSE(request.clean_rwfs);
    EXPECT_FALSE(request.factory_reset);
    EXPECT_TRUE(request.complete_reset);
    EXPECT_TRUE(request.formatsRwfs());
}

TEST(EmmcInitFlow, OpenbmconceMatchesWholeWhitespaceSeparatedTokens)
{
    auto request = emmc::init::parseOpenbmconce(
        "pre-complete-reset complete-reset-extra\tclean-rwfs-filesystem");
    EXPECT_TRUE(request.clean_rwfs);
    EXPECT_FALSE(request.factory_reset);
    EXPECT_FALSE(request.complete_reset);
    EXPECT_TRUE(request.formatsRwfs());
}
