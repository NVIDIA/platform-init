// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// Switch BMC MCU reset, driven by mcu-devices.json (hw-management-mcu.sh).

#pragma once

#include "switch_common.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace nvidia
{

struct McuLines
{
    std::string name;
    std::string reset;
    std::string recovery;
    std::string ready;
};

struct McuConfig
{
    // Reset and recovery are active-high instead of idle-complement.
    bool pulse_high_low = false;
    std::vector<McuLines> mcus;
};

namespace detail
{

McuConfig parse_mcu_config(std::string_view text);

// "/sys/class/gpio/gpio<N>/value" -> N, as the script's ${t##*/gpio}.
std::optional<int> gpio_number_from_path(std::string_view path);

// A trailing "_L" means ready is asserted low.
char mcu_ready_level(std::string_view ready_name);

} // namespace detail

// Pulse every MCU's reset, then wait up to 10 s each for ready. A missing
// config is a no-op.
void hw_mgmt_mcu_reset(std::string_view config = "/etc/mcu-devices.json",
                       std::string_view gpio_dir = hw_mgmt_system_path);

} // namespace nvidia
