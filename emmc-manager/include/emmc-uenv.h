// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace emmc::uenv
{

// Stateless wrappers around libubootenv. Each call opens, performs, and
// closes — `config_file` selects /etc/fw_env.config (default) or
// /etc/rr_env.config. All return false / nullopt on libubootenv error.
//
// /dev/mtd/by-name/<n> paths in the config are resolved to /dev/mtdN
// internally so the env works in initramfs (pre-udev) too.

bool isAvailable(const char* config_file = nullptr);

std::optional<std::string> get(std::string_view key,
                               const char* config_file = nullptr);

bool set(std::string_view key, std::string_view value,
         const char* config_file = nullptr);

// Deletes the variable (libubootenv removes the entry only on NULL value).
bool clear(std::string_view key, const char* config_file = nullptr);

int getCounter(std::string_view key, const char* config_file = nullptr);

bool setCounter(std::string_view key, int value,
                const char* config_file = nullptr);

} // namespace emmc::uenv
