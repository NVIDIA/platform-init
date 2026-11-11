// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for vr-nvl-bmc (AST2700); initramfs binds the bypass IOXs.

#include "compute_bmc_common.hpp"
#include "utilities.hpp"
#include "vr_pdb.hpp"

#include <array>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <utility>

namespace nvidia
{

using namespace std::chrono_literals;

static void ehci_rebind_unbound()
{
    const std::string drv = "/sys/bus/platform/drivers/ehci-platform";
    for (const char* dev : {"14121000.usb", "14123000.usb", "12063000.usb"})
    {
        const auto bound = drv + "/" + dev;
        if (std::filesystem::exists(bound))
        {
            std::cerr << std::format("ehci rebind: {} already bound\n", dev);
            continue;
        }
        for (int i = 1; i <= 30; ++i)
        {
            detail::sysfs_write(drv + "/bind", dev);
            if (std::filesystem::exists(bound))
            {
                std::cerr << std::format(
                    "ehci rebind: {} bound on attempt {}\n", dev, i);
                break;
            }
            std::this_thread::sleep_for(1s);
        }
        if (!std::filesystem::exists(bound))
            std::cerr << std::format(
                "ehci rebind: {} FAILED after 30 attempts\n", dev);
    }

    std::string busnum;
    std::error_code ec;
    std::vector<std::string> usbdirs;
    for (const auto& e : std::filesystem::directory_iterator(
             "/sys/bus/platform/devices/14121000.usb", ec))
    {
        const auto name = e.path().filename().string();
        if (name.starts_with("usb"))
            usbdirs.push_back(name);
    }
    if (!usbdirs.empty())
        busnum = std::ranges::min(usbdirs).substr(3);
    if (busnum.empty())
    {
        std::cerr << "ehci rebind: no usb bus for 14121000.usb - skipping "
                     "hub reset\n";
        return;
    }
    const auto hub = std::format("/sys/bus/usb/devices/{}-1", busnum);
    if (!std::filesystem::exists(hub))
        std::this_thread::sleep_for(5s);
    if (std::filesystem::exists(hub))
    {
        std::cerr << std::format(
            "ehci rebind: bus {} top hub present, no reset needed\n", busnum);
        return;
    }
    std::cerr << std::format(
        "ehci rebind: bus {} top hub ABSENT - pulsing USB2_HUB_RST_L\n",
        busnum);
    gpio::set("USB2_HUB_RST_L-O", 0);
    gpio::set("USB2_HUB_RST_L-O", 1);
}

static constexpr auto tag = "vr-nvl-bmc-ast2700";
// The HPM boards, present while their IOX at 0x20 is bound.
static constexpr std::array<compute::BoardLine, 2> hpm_standby_pg{{
    {"B0_M0", 11, 0x20, "B0_M0_STBY_POWER_PG-I"},
    {"B1_M0", 8, 0x20, "B1_M0_STBY_POWER_PG-I"},
}};

static constexpr std::array<compute::BoardLine, 2> hpm_mcu_ok{{
    {"B0_M0", 11, 0x20, "B0_M0_HPM_MCU_OK-I"},
    {"B1_M0", 8, 0x20, "B1_M0_HPM_MCU_OK-I"},
}};

static constexpr std::array<compute::BoardLine, 2> hpm_cpld_ready{{
    {"B0_M0", 11, 0x20, "B0_M0_CPLD_READY-I"},
    {"B1_M0", 8, 0x20, "B1_M0_CPLD_READY-I"},
}};

// rtc-detection.sh's buses.
static constexpr std::array<unsigned, 2> rtc_buses{{14, 19}};
// The RTC parts the BMC module may carry, at 0x6f.
static constexpr std::array<RtcId, 2> rtc_parts{{
    {0x6f, 0x10, 0x5c, "nct3018y"},
    {0x6f, 0x10, 0x4e, "pcf85053a"},
}};
static constexpr compute::Rtc rtc{rtc_buses, rtc_parts};

int init_vr_nvl_bmc_ast2700()
{
    std::cerr << "vr-nvl-bmc-ast2700: platform init start\n";

    ehci_rebind_unbound();

    compute::create_state_dir();

    if (compute::bmc_cold_boot("B0_M0_STBY_POWER_PG-I"))
    {
        gpio::set("GLOBAL_WP_BMC-O", 0);
        mask_pdb_hsc_faults(0);
        configure_pdb_tmp_alert_threshold(0);
        gpio::set("BF3_AOC_NCSI_PRSNT_L-I", 0);
        gpio::set("MUX_I2C_ESPI_SEL-O", 0);
        gpio::set("EEDO_LED2-O", 1);
    }

    compute::bind_expanders(0, {0x20, 0x75});

    gpio::set("PDB_12V_EN_N_R-O", 1);
    std::this_thread::sleep_for(1s);

    compute::enable_pdb_standby_power(tag, 0);
    if (!compute::wait_bmc_standby_power(tag, compute::PgSource::Gpio,
                                         "STBY_POWER_PG-I", "STBY_POWER_PG-I"))
        return compute::init_failed(rtc);

    gpio::set("HMC_RST_R_L-O", 1);

    gpio::set("PDB_12V_EN_N_R-O", 0);

    if (!compute::wait_pdb_standby_power(tag, 0, compute::PgSource::Gpio))
        return compute::init_failed(rtc);

    compute::wait_iox(tag, 0x20, {11, 8});

    hmc_mux_select("HMC_PRSNT_R-I", "BMC_HMC_MUX_SEL-O");

    compute::bind_expanders(10, {0x20, 0x21});
    compute::bind_expanders(11, {0x20, 0x21, 0x22, 0x23, 0x24});
    compute::bind_expanders(8, {0x20, 0x21, 0x22, 0x23, 0x24});

    // Unused SOCAMM select pins: drive them low instead of floating.
    for (auto [bus, board] : {std::pair{11, "B0"}, std::pair{8, "B1"}})
    {
        if (!std::filesystem::exists(
                std::format("/sys/bus/i2c/drivers/pca953x/{}-0021", bus)))
            continue;
        for (const auto* pin :
             {"M0_C0_SOCAMM_DAC_SEL0-O", "M0_C1_SOCAMM_DAC_SEL1-O",
              "M0_C1_SOCAMM_I2C_SEL_R-O"})
            gpio::set(std::format("{}_{}", board, pin).c_str(), 0);
    }

    gpio::set("USB_MUX_EN-O", 1);
    gpio::set("BMC_MUX_PI3DP_SEL-O", 0);

    if (!compute::wait_boards_standby_power(tag, hpm_standby_pg,
                                            compute::PgSource::Gpio) ||
        !compute::wait_boards_ready(tag, hpm_mcu_ok, "HPM_MCU_OK") ||
        !compute::wait_boards_ready(tag, hpm_cpld_ready, "CPLD_READY"))
        return compute::init_failed(rtc);

    usb_hub_reset_if_absent("B0_M0_USB_HUB0_RST_L-O", "/dev/ttyUSB0");
    usb_hub_reset_if_absent("B1_M0_USB_HUB0_RST_L-O", "/dev/ttyUSB4");

    if (!compute::check_rofs_and_rw(tag))
        return compute::init_failed(rtc);

    return compute::bmc_ready(tag, rtc);
}

} // namespace nvidia
