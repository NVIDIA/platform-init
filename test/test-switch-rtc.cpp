// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

#include "nvidia/switch_rtc.hpp"

#include <sstream>

#include <gtest/gtest.h>

using nvidia::is_i2c_hex_byte;
using nvidia::parse_rtc_config;

TEST(SwitchRtc, ShippedConfig)
{
    std::istringstream in(R"(# comment
rtc 0x6f 0x10 0x4e pcf85053a   # trailing comment
bus 4
  bus 5
)");
    auto cfg = parse_rtc_config(in);
    ASSERT_EQ(cfg.buses, (std::vector<std::string>{"4", "5"}));
    ASSERT_EQ(cfg.devices.size(), 1U);
    EXPECT_EQ(cfg.devices[0].addr, "0x6f");
    EXPECT_EQ(cfg.devices[0].vendor_reg, "0x10");
    EXPECT_EQ(cfg.devices[0].dev_id, "0x4e");
    EXPECT_EQ(cfg.devices[0].name, "pcf85053a");
}

TEST(SwitchRtc, IgnoresIncompleteAndUnknownLines)
{
    std::istringstream in("rtc 0x6f 0x10 0x4e\nfoo 1\nbus\n#bus 3\n");
    auto cfg = parse_rtc_config(in);
    EXPECT_TRUE(cfg.devices.empty());
    EXPECT_TRUE(cfg.buses.empty());
}

TEST(SwitchRtc, HexByte)
{
    EXPECT_TRUE(is_i2c_hex_byte("0x6f"));
    EXPECT_TRUE(is_i2c_hex_byte("0xA"));
    EXPECT_FALSE(is_i2c_hex_byte("0x"));
    EXPECT_FALSE(is_i2c_hex_byte("0x4g"));
    EXPECT_FALSE(is_i2c_hex_byte("0x100"));
    EXPECT_FALSE(is_i2c_hex_byte("6f"));
}
