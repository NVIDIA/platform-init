// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "switch_misc.hpp"

#include <gtest/gtest.h>

using nvidia::switch_misc::parse_recovery_conf;

TEST(RecoveryConf, Ast2700)
{
    auto c = parse_recovery_conf(
        "# x\nI2C_BUS=0\nSLAVE_ADDR=0x42\nI2C_DEVICE_TREE_UNIT=\"i2c@880\"\n");
    ASSERT_TRUE(c);
    EXPECT_EQ(c->bus, 0U);
    EXPECT_EQ(c->address, 0x42U);
}

TEST(RecoveryConf, DefaultsAndQuotes)
{
    auto c = parse_recovery_conf("POLL_INTERVAL=1\n");
    ASSERT_TRUE(c);
    EXPECT_EQ(c->bus, 3U);
    EXPECT_EQ(c->address, 0x42U);
    c = parse_recovery_conf("I2C_BUS=\"9\"\nSLAVE_ADDR=\n");
    ASSERT_TRUE(c);
    EXPECT_EQ(c->bus, 9U);
    EXPECT_EQ(c->address, 0x42U);
}

TEST(RecoveryConf, Invalid)
{
    EXPECT_FALSE(parse_recovery_conf("I2C_BUS=abc\n"));
}
