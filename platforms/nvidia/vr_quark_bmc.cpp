// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for vr-quark-bmc.

#include "compute_bmc_common.hpp"
#include "utilities.hpp"
#include "vr_pdb.hpp"

#include <array>
#include <chrono>
#include <format>
#include <iostream>
#include <thread>

namespace nvidia
{

using namespace std::chrono_literals;

static constexpr auto tag = "vr-quark-bmc";
// The HPM boards, present while their IOX at 0x20 is bound.
static constexpr std::array<compute::BoardLine, 2> hpm_standby_pg{{
    {"B0_M0", 2, 0x20, "B0_M0_STBY_POWER_PG-I"},
    {"B1_M0", 7, 0x20, "B1_M0_STBY_POWER_PG-I"},
}};

static constexpr std::array<compute::BoardLine, 2> hpm_cpld_ready{{
    {"B0_M0", 2, 0x20, "B0_M0_CPLD_READY-I"},
    {"B1_M0", 7, 0x20, "B1_M0_CPLD_READY-I"},
}};

static constexpr int fan_pwm = 38;
static constexpr std::chrono::seconds mcu_ok_timeout = 10s;
static constexpr std::chrono::milliseconds mcu_ok_poll = 100ms;
static constexpr std::chrono::seconds cp2112_wait = 10s;

static void init_quark_pwms()
{
    std::cerr << std::format("vr-quark-bmc: init FAN PWMs to {}\n", fan_pwm);
    for (std::string_view hwmon : {"hwmon0", "hwmon1"})
        for (std::string_view pwm : {"pwm1", "pwm2", "pwm3"})
            set_hwmon_pwm(hwmon, pwm, fan_pwm);
}

// Without an HMC the BMC creates the bypass IOX itself.
static void select_hmc_mux()
{
    if (gpio_get("HMC_PRSNT_R-I") == 1)
    {
        std::cerr << "vr-quark-bmc: HMC present, disabling I2C_MGMT1 bypass\n";
        gpio::set("BMC_HMC_MUX_SEL-O", 1);
        return;
    }
    std::cerr << "vr-quark-bmc: HMC not present, setting I2C_MGMT1 bypass\n";
    gpio::set("BMC_HMC_MUX_SEL-O", 0);
    if (!iox_present(1, 0x20))
        i2c::new_device(1, 0x20, "pca9555");
}

static constexpr std::array<unsigned, 2> rtc_buses{{6, 19}};
// The RTC parts the BMC module may carry, at 0x6f.
static constexpr std::array<RtcId, 2> rtc_parts{{
    {0x6f, 0x10, 0x5c, "nct3018y"},
    {0x6f, 0x10, 0x4e, "pcf85053a"},
}};
static constexpr compute::Rtc rtc{rtc_buses, rtc_parts};

static bool wait_mcu_ok(const char* line)
{
    if (wait_gpio_asserted(line, GpioLevel::High, mcu_ok_timeout, mcu_ok_poll))
        return true;
    std::cerr << std::format("vr-quark-bmc: {} failed to assert after {} "
                             "seconds\n",
                             line, mcu_ok_timeout.count());
    return false;
}

// The B0 USB hub is reset between the two MCU waits.
static bool wait_hpm_mcu_ok()
{
    bool ok = true;
    if (iox_present(2, 0x20))
    {
        if (!wait_mcu_ok("B0_M0_HPM_MCU_OK-I"))
            ok = false;
        usb_hub_reset_if_absent("B0_M0_USB_HUB0_RST_L-O", "/dev/ttyUSB0");
    }
    if (iox_present(7, 0x20) && !wait_mcu_ok("B1_M0_HPM_MCU_OK-I"))
        ok = false;
    return ok;
}

int init_vr_quark_bmc()
{
    std::cerr << "vr-quark-bmc: platform init start\n";

    compute::create_state_dir();

    if (compute::bmc_cold_boot("B0_M0_STBY_POWER_PG-I"))
    {
        gpio::set("GLOBAL_WP_BMC-O", 0);
        gpio::set("HMC_RST_R_L-O", 0);
    }

    compute::bind_expanders(9, {0x20, 0x75});

    compute::enable_pdb_standby_power(tag, 9);
    if (!compute::wait_bmc_standby_power(tag, compute::PgSource::Monitor,
                                         "STBY_POWER_PG-I", "STBY_POWER_PG-I"))
        return compute::init_failed(rtc);

    gpio::set("HMC_RST_R_L-O", 1);

    gpio::set("USB_PWR_EN-O", 1);
    init_quark_pwms();
    gpio::set("BMC_FAN_PWR_EN-O", 1);
    gpio::set("USB3_MUX_EN_L-O", 0);

    if (!compute::wait_pdb_standby_power(tag, 9, compute::PgSource::Monitor))
        return compute::init_failed(rtc);

    compute::wait_iox(tag, 0x20, {2});

    select_hmc_mux();

    compute::bind_expanders(1, {0x20, 0x21});
    compute::bind_expanders(2, {0x20, 0x21, 0x22, 0x23, 0x24});
    compute::bind_expanders(7, {0x20, 0x21, 0x22, 0x23, 0x24});
    compute::bind_expanders(5, {0x22});

    if (!compute::wait_boards_standby_power(tag, hpm_standby_pg,
                                            compute::PgSource::Monitor) ||
        !wait_hpm_mcu_ok() ||
        !compute::wait_boards_ready(tag, hpm_cpld_ready, "CPLD_READY"))
        return compute::init_failed(rtc);

    if (!compute::check_rofs_and_mtd(tag))
        return compute::init_failed(rtc);

    if (!iox_present(70, 0x53))
    {
        std::cerr << "vr-quark-bmc: waiting for CP2112 to be present\n";
        std::this_thread::sleep_for(cp2112_wait);
    }

    return compute::bmc_ready(tag, rtc);
}

} // namespace nvidia
