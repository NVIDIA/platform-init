// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Helpers for the compute HMC ports (VR and GB200 NVL).

#pragma once

#include "nvidia_platform_utils.hpp"

namespace nvidia
{

// Drives the HMC_READY pin high through <ready_pin>, the sysfs file the
// device tree gives it (a gpio-leds node), and reads it back.
bool set_hmc_ready(std::string_view ready_pin);

} // namespace nvidia

namespace nvidia::compute
{

inline constexpr std::chrono::seconds erot_reset_delay{10};

struct GpioOut
{
    const char* line;
    int value;
};

// Drives each output, in order.
void hmc_set_initial_gpio_out(std::string_view tag,
                              std::span<const GpioOut> outputs);

// Drives each board's HMC to HPM MCU I2C enable line high, in order.
void release_hpm_mcu_reset(std::string_view tag,
                           std::span<const char* const> enable_lines);

bool check_hmc_filesystems();

// Asserts HMC_READY through <ready_pin>, then reports ready to systemd.
int hmc_ready(std::string_view tag, std::string_view ready_pin);

} // namespace nvidia::compute
