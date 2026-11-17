// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "switch_mcu.hpp"

#include <gtest/gtest.h>

using namespace nvidia::detail;

TEST(SwitchMcu, ParsesShippedShape)
{
    const auto cfg = parse_mcu_config(R"({
        "_comment_reset_pulse_high_low": "x",
        "reset_pulse_high_low": true,
        "mcus": [
            {"name": "A", "reset_gpio": "R", "recovery_gpio": "V",
             "ready_gpio": "Y_L"},
            {"name": "B", "reset_gpio": 3}
        ]})");
    EXPECT_TRUE(cfg.pulse_high_low);
    ASSERT_EQ(cfg.mcus.size(), 2U);
    EXPECT_EQ(cfg.mcus[0].ready, "Y_L");
    EXPECT_EQ(cfg.mcus[1].reset, "");
}

TEST(SwitchMcu, PulsePolarityFlag)
{
    EXPECT_FALSE(parse_mcu_config(R"({"mcus": []})").pulse_high_low);
    EXPECT_TRUE(
        parse_mcu_config(R"({"reset_pulse_high_low": "true"})").pulse_high_low);
    EXPECT_FALSE(parse_mcu_config("not json").pulse_high_low);
}

TEST(SwitchMcu, GpioNumberFromPath)
{
    EXPECT_EQ(gpio_number_from_path("/sys/class/gpio/gpio612/value"), 612);
    EXPECT_EQ(gpio_number_from_path("/sys/class/gpio/gpio7"), 7);
    EXPECT_FALSE(gpio_number_from_path("/run/hw-management/system/X"));
    EXPECT_FALSE(gpio_number_from_path("/sys/class/gpio/gpio/value"));
}

TEST(SwitchMcu, ReadyLevel)
{
    EXPECT_EQ(mcu_ready_level("GP_MCU_READY_R_L"), '0');
    EXPECT_EQ(mcu_ready_level("GP_MCU_READY_R"), '1');
    EXPECT_EQ(mcu_ready_level(""), '1');
}
