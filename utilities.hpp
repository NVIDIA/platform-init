// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#pragma once

#include <chrono>
#include <string_view>

constexpr static const char* app_name = "platform_init";

void sleep_milliseconds(std::chrono::milliseconds milliseconds);

void wait_for_path_to_exist(std::string_view path,
                            std::chrono::milliseconds timeout);

// Write a PWM duty value to /sys/class/hwmon/<hwmon>/<pwm>.
// Generic so platforms can pre-set fan duty without open-coding sysfs paths,
// e.g. set_hwmon_pwm("hwmon0", "pwm1", 38).
void set_hwmon_pwm(std::string_view hwmon, std::string_view pwm, int value);
