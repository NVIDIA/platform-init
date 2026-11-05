// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Helpers for the switch BMC ports.

#pragma once

#include "nvidia_platform_utils.hpp"

namespace nvidia
{

inline constexpr std::string_view hw_mgmt_config_path =
    "/var/run/hw-management/config";

inline constexpr std::string_view hw_mgmt_system_path =
    "/var/run/hw-management/system";

namespace detail
{

std::vector<uint8_t> read_file_range(const std::string& path, size_t offset,
                                     size_t length);

bool is_whitespace(uint8_t c);

// Shell `echo $var` semantics: drop NULs, collapse whitespace, trim.
std::string shell_word_join(std::span<const uint8_t> raw);

// First whitespace-separated word, NULs dropped.
std::string first_word(std::span<const uint8_t> raw);

} // namespace detail

void shell_out(std::string_view cmd, std::string_view log_prefix);

// The hwmon instance number is not stable across boots; resolve the glob.
std::string hw_mgmt_hwmon_attr(std::string_view hwmon_dir,
                               std::string_view attribute,
                               std::string_view log_prefix);

std::optional<uint32_t> hw_mgmt_read_u32(std::string_view path);

bool hw_mgmt_write(std::string_view path, std::string_view value);

// The trailing newline is part of the format hw-management expects.
bool hw_mgmt_write_config(std::string_view name, std::string_view value);

// Compared byte for byte by hw-management; write nothing without a CPU part.
void write_cpu_type(std::string_view log_prefix);

// CPU off: the host is not driving the I2C tree, so the BMC may take it.
bool hw_mgmt_take_i2c_when_cpu_off(std::string_view hwmon_dir,
                                   std::string_view log_prefix);

// Deliberately fails open: log and carry on.
void hw_mgmt_wait_system_tree(
    std::string_view log_prefix,
    std::chrono::seconds timeout = std::chrono::seconds{20});

enum class SysfsGpioDir : uint8_t
{
    In,
    Out,
    // Leave the direction and value the controller retained.
    Preserve,
};

struct SysfsGpioPin
{
    int number; // absolute sysfs GPIO number (chip base + offset)
    SysfsGpioDir direction = SysfsGpioDir::Preserve;
    std::optional<int> value = std::nullopt; // applied to outputs only
    std::string symlink;                     // name under system_path, or ""
};

std::string sysfs_gpio_value_path(int number);

// An already-exported line (EBUSY) is skipped, so a re-run is safe. An output
// with value 0 or 1 is programmed as "low" or "high".
void sysfs_gpio_apply(std::span<const SysfsGpioPin> pins);

// Pins without a value attribute are skipped; existing links are replaced.
void hw_mgmt_link_gpio(std::span<const SysfsGpioPin> pins,
                       std::string_view system_path = hw_mgmt_system_path);

namespace detail
{

std::optional<int> gpiochip_attr_int(const std::filesystem::path& chip,
                                     const char* attr);

// Lexicographic order, matching the shell glob.
std::vector<std::filesystem::path> gpiochip_dirs();

} // namespace detail

// quiet suppresses the not-found diagnostic, for probes that have a fallback.
std::optional<int> sysfs_gpiochip_base_by_ngpio(int ngpio, bool quiet = false);

// Some kernels report a different ngpio for the SoC bank.
std::optional<int> sysfs_gpiochip_base_aspeed();

// Disambiguates identical expanders on different buses, e.g. "5-0020".
std::optional<int> sysfs_gpiochip_base_by_i2c(std::string_view i2c_id);

std::optional<int> sysfs_gpiochip_base_for_chip_id(std::string_view chip);

bool wait_sysfs_value(
    std::string_view path, std::string_view expected, int retries = 10,
    std::chrono::milliseconds delay = std::chrono::milliseconds{1000});

std::string vpd_read_field(std::string_view eeprom_path, size_t offset,
                           size_t length);

// Offset of the first "V<digit>-" in the leading bytes.
std::optional<size_t> vpd_find_bom_offset(std::string_view eeprom_path,
                                          size_t scan_length = 128);

// Empty when the HID is not allow-listed or the record cannot be read.
std::string vpd_read_bom(std::string_view eeprom_path, std::string_view hid,
                         std::span<const std::string_view> allowed_hids,
                         std::optional<size_t> offset = std::nullopt,
                         size_t length = 192);

// One sequential read: seeking on at24 re-reads from 0 and races ftgmac100.
std::string fru_rev_id_from_eeprom(std::string_view eeprom_path);

// Unknown HIDs are a silent no-op.
void write_system_hw_rev(std::string_view eeprom_path, std::string_view hid,
                         std::span<const std::string_view> allowed_hids,
                         std::string_view log_prefix);

// Returns 137 on timeout; callers must not fail init on non-zero.
int modprobe_with_timeout(
    std::string_view module,
    std::chrono::seconds timeout = std::chrono::seconds{5},
    bool ignore_install = false);

// /proc/modules uses underscores, so compare normalised names.
bool module_loaded(std::string_view module);

// Read as key=value, not sourced; missing means most conservative mode.
std::string platform_feature(
    std::string_view key,
    std::string_view path = "/etc/platform_features.conf");

// In unset_on_boot mode write protect is dropped once, never re-enabled.
void unset_write_protect_on_boot(
    std::string_view system_path = hw_mgmt_system_path);

// No D-Bus Logging entry: phosphor-log-manager is not up this early.
void phosphor_log_all(std::string_view message);

bool i2c_mux_present();

int bind_i3c_controller();

// A (devicetree model word, HID) pair whose cable cartridge EEPROMs are
// appended to the devtree file.
struct CartridgeBoard
{
    std::string_view model;
    std::string_view hid;
};

namespace detail
{

// The devtree line for a SMBIOS BOM string and a devtree.json BOM map; empty
// when the string or the map is not usable.
std::string devtree_from_bom(std::string_view bom, std::string_view json);

} // namespace detail

// The hw-management.sh start phase: the devtree file from the BOM, then the
// cartridge EEPROMs for the boards listed. Board strings decode only when
// config/cpu_type is one of decode_cpu_types, reproducing the shipped
// script: any other value takes its fold path, and busybox has no fold.
void hw_mgmt_boot_start(std::span<const CartridgeBoard> cartridge_boards,
                        std::span<const std::string_view> decode_cpu_types);

// rtc-detection's PCF85053A step: an unreliable-time flag (CIF/RTCF/OF in
// reg 0x0b) makes the driver refuse every read, so the kernel does not set
// the clock from it. Clears them and the STOP bit, then sets the clock from
// the RTC unless the kernel already did.
void recover_external_rtc(std::string_view tag);

} // namespace nvidia
