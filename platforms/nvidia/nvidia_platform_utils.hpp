// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Helpers shared by the NVIDIA platform ports.

#pragma once

#include "gpio.hpp"
#include "i2c.hpp"
#include "utilities.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <systemd/sd-daemon.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace nvidia
{

struct EepromDevice
{
    unsigned int bus;
    unsigned int address;
    std::string_view type;
    std::string_view description;
    bool poweron_only = false;
};

void eeprom_init(std::span<const EepromDevice> devices);

bool iox_present(unsigned bus, unsigned addr);

void iox_bind(unsigned bus, unsigned addr);

// Lines are released before READY=1, as gpioset -m exit does.
int notify_ready_and_wait();

namespace detail
{

// ofstream cannot report errno; callers must tell EBUSY from real errors.
int sysfs_write(const std::string& path, std::string_view data);

std::optional<std::string> sysfs_read(const std::string& path);

bool is_mounted(std::string_view path);

} // namespace detail

std::string uboot_env_get(std::string_view key);

// An empty value unsets the variable, matching `fw_setenv <key>`.
void uboot_env_set(std::string_view key, std::string_view value = {});

int filesystem_check(std::string_view rwfs_mount = "/run/initramfs/rw",
                     std::string_view work_path = "/run/initramfs/rw/work");

// MTD/JFFS2 RWFS variant: erases the partition, no attempt cap. Checks the
// mount after recovery, unlike the shell, which reported a failed erase as a
// recovery.
int filesystem_check_mtd(std::string_view rwfs_mount = "/run/initramfs/rw",
                         std::string_view work_path = "/run/initramfs/rw/work",
                         std::string_view mtd_device = "/dev/mtd6");

// emmc-rwfs check_rofs: work/ must exist and work/work be creatable.
bool check_rofs();

// emmc-rwfs check_rw_filesystems.
bool check_rw_filesystems();

} // namespace nvidia

namespace nvidia::compute
{

// The scripts' mc_lib.sh creates /run/bmc-state when it is sourced.
void create_state_dir();

} // namespace nvidia::compute
