// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// Switch BMC boot helpers started from their own systemd units.

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

// bmc-i2c-slave-setup.sh: the slave-24c02 recovery EEPROM.
int i2c_slave_setup(const std::string& conf = "/etc/bmc-recovery.conf");

} // namespace nvidia::switch_misc
