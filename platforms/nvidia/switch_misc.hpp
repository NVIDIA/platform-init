// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// Switch BMC boot helpers.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace nvidia::switch_misc
{

struct SlaveConfig
{
    unsigned bus = 3;
    unsigned address = 0x42;
};

// I2C_BUS and SLAVE_ADDR from bmc-recovery.conf text; unset keys keep the
// script defaults. nullopt when a value is not a shell number.
std::optional<SlaveConfig> parse_recovery_conf(std::string_view text);

// The slave-24c02 recovery EEPROM on the bus and address bmc-recovery.conf
// names.
int i2c_slave_setup(const std::string& conf = "/etc/bmc-recovery.conf");

// i2c_slave_setup() on boards that ship the recovery conf; not fatal.
void i2c_slave_setup_if_configured(
    const std::string& conf = "/etc/bmc-recovery.conf");

} // namespace nvidia::switch_misc
