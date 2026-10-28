// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Helpers for the compute BMC ports (VR and GB200 NVL).

#pragma once

#include "nvidia_platform_utils.hpp"

namespace nvidia
{

bool iox_wait(unsigned bus, unsigned addr,
              std::chrono::seconds timeout = std::chrono::seconds{30});

void i2c_mux_bind(unsigned bus, unsigned addr);

enum class GpioLevel
{
    Low,
    High
};

namespace detail
{

enum class GpioLineState
{
    Unavailable,
    Deasserted,
    Asserted,
    Error,
};

GpioLineState gpio_read_line_state(const char* line_name,
                                   std::error_code* ec = nullptr);

} // namespace detail

// Like gpio::get, but a busy or unreadable line returns -1 instead
// of throwing.
int gpio_get(const char* line_name);

// Missing or unreadable lines are retried until the timeout.
[[nodiscard]] bool wait_gpio_asserted(const char* line_name, GpioLevel expected,
                                      std::chrono::milliseconds timeout,
                                      std::chrono::milliseconds poll_interval);

// gpioset -m exit: drive the line, then release it.
bool gpio_set_released(gpiod::line& line, int value);

gpiod::line gpio_find(const char* line_name);

// False when the line is missing or cannot be driven.
bool gpio_set_released(const char* line_name, int value);

// The scripts' reset_usb_hub(): the line is looked up once, since the reset
// can take other GPIO providers away, and any failure fails the reset.
bool usb_hub_reset(const char* line_name);

// systemctl is-active.
bool unit_active(sdbusplus::bus_t& bus, const std::string& unit);

// A /run/bmc-state flag as `[[ $(cat file) -gt 0 ]]` reads it.
bool state_file_asserted(const std::string& path);

// The scripts' get_stby_power_pg(): while <monitor> is active the value
// comes from its state file (0 if the file is missing), else from <line>.
// A null <line> reads only the state file, as get_bmc_stby_power_pg() does.
[[nodiscard]] bool wait_standby_power_good(
    const char* line, const std::string& monitor, const std::string& state_file,
    std::chrono::milliseconds timeout, std::chrono::milliseconds poll_interval);

// HMC present (<prsnt_gpio> high) -> <sel_gpio> HIGH (dedicated path);
// absent -> LOW (bypass).
void hmc_mux_select(const char* prsnt_gpio, const char* sel_gpio);

void usb_hub_reset_if_absent(const char* reset_line, const char* tty_dev);

} // namespace nvidia

namespace nvidia::compute
{

// A line on a board that may be absent: waited on only while the board's
// IOX at <bus>-<iox> is bound.
struct BoardLine
{
    std::string_view board;
    unsigned bus;
    unsigned iox;
    const char* line;
};

// True when the standby power line is low or unreadable.
bool bmc_cold_boot(const char* stby_pg_line);

void bind_expanders(unsigned bus, std::initializer_list<unsigned> addrs);

// Where a standby power wait reads its power-good from: always the GPIO, or,
// as the scripts' get_stby_power_pg() does, the standby power monitor's
// /run/bmc-state file while that monitor service is active.
enum class PgSource
{
    Gpio,
    Monitor,
};

// Waits 20s for <line>, the power-good of <board> (BMC, PDB or a board
// name); on a timeout drives <disable_line> low, as the scripts do.
bool wait_standby_power(std::string_view tag, std::string_view board,
                        PgSource src, const std::string& line,
                        const std::string& disable_line);

// Waits 20s for the BMC standby power-good <line>.
bool wait_bmc_standby_power(std::string_view tag, PgSource src,
                            const std::string& line,
                            const std::string& disable_line);

// Waits up to 30s per bus for the IOX at <addr>; a timeout is not fatal.
void wait_iox(std::string_view tag, unsigned addr,
              std::initializer_list<unsigned> buses);

// Waits 20s for each standby power-good line whose board is present,
// stopping at the first failure.
bool wait_boards_standby_power(std::string_view tag,
                               std::span<const BoardLine> lines, PgSource src);

// Waits 10s for each line whose board is present; every line is waited on
// before failing, and <what> names the failure.
bool wait_boards_ready(std::string_view tag, std::span<const BoardLine> lines,
                       std::string_view what);

bool check_rofs_and_mtd(std::string_view tag);
bool check_rofs_and_rw(std::string_view tag);

// The scripts' reset_usb_hub(): pulses <line> when <tty> is missing.
bool reset_usb_hub_if_absent(std::string_view tag, const char* line,
                             const char* tty);

// Binds the RTC driver for the NCT3018Y or PCF85053A found at <addr> on
// the first of <buses> that has one.
void rtc_detect(std::span<const unsigned> buses, uint8_t addr = 0x6f);

// For an init that stops early: detects the RTC on <rtc_buses>, as
// rtc-detection.service did whether or not bmc-boot-complete succeeded.
int init_failed(std::span<const unsigned> rtc_buses);

// Reports ready to systemd, then detects the RTC on <rtc_buses>, as
// rtc-detection.service did after bmc-boot-complete.
int bmc_ready(std::string_view tag, std::span<const unsigned> rtc_buses = {});

} // namespace nvidia::compute
