// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Helpers for the compute HMC ports (VR and GB200 NVL).

#pragma once

#include "compute_bmc_common.hpp"

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

// A board whose IO expanders appear on its bus once standby power is up.
struct StandbyBoard
{
    unsigned bus;
    std::array<uint8_t, 2> iox;
    unsigned attempts;
    std::optional<uint8_t> fru{}; // HPM FRU to re-bind, if the board has one
};

struct StandbyConfig
{
    StandbyBoard brd0;
    StandbyBoard brd1;
    std::chrono::milliseconds poll{1000};
};

// As the initramfs standby_power_wait.sh did: polls each board's first IO
// expander, re-binds its devices once found, and leaves
// run/standby_power_failure (BRD0 timed out) or run/brd1_not_present under
// <root>. Boot continues either way.
void wait_for_standby_power(std::string_view tag, const StandbyConfig& cfg,
                            const std::filesystem::path& root = "/",
                            bool (*present)(unsigned, uint8_t) = i2c::probe);

// The external RTC shares its bus with the host, so it is read only while
// the host is down (<host_power_good> low); rtc0 is the internal one. With
// no <host_power_good>, the U-Boot reset_reason decides.
struct RtcSync
{
    Rtc external;
    const char* host_power_good = nullptr;
    unsigned readiness_tries = 5;
    unsigned external_tries = 3;
    unsigned bind_tries = 3;
    unsigned internal_tries = 5;
    std::chrono::milliseconds retry_delay{2000};
    std::chrono::milliseconds bind_delay{1000};
    std::time_t min_epoch = 1700000000;
};

// rtc-sync.sh's get_fw_env_var: <key> from the raw u-boot-env MTD under
// <root>, CRC and flag byte skipped, no CRC check.
std::string uboot_env_read(std::string_view key,
                           const std::filesystem::path& root);

// Test seams; the defaults are the hardware.
struct RtcSyncIo
{
    std::filesystem::path root{"/"};
    int (*power_good)(const char*) = gpio_get;
    std::string (*env)(std::string_view,
                       const std::filesystem::path&) = uboot_env_read;
    bool (*set_clock)(std::time_t) = set_system_clock;
};

// rtc-sync.sh: from the power-good line, else the U-Boot reset_reason.
bool use_external_rtc(std::string_view tag, const RtcSync& cfg,
                      const RtcSyncIo& io = {});

// The newest rtcN but rtc0: the external RTC just bound.
std::optional<std::string> newest_external_rtc(
    const std::filesystem::path& root);

// rtc-sync.sh, as the initramfs ran it: sets the system clock from the
// external RTC when the host is down, else (or failing that) from rtc0.
bool sync_clock_from_rtc(std::string_view tag, const RtcSync& cfg,
                         const RtcSyncIo& io = {});

bool check_hmc_filesystems();

// Asserts HMC_READY through <ready_pin>, then reports ready to systemd.
int hmc_ready(std::string_view tag, std::string_view ready_pin);

} // namespace nvidia::compute
