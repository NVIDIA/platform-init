// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for vr-nvl-bmc.

#include "compute_bmc_common.hpp"
#include "utilities.hpp"
#include "vr_pdb.hpp"

#include <array>
#include <filesystem>
#include <format>
#include <iostream>
#include <utility>

namespace nvidia
{

static void apply_ncsi_hsc_gpio_from_networkd()
{
    static constexpr auto cfg = "/etc/systemd/network/00-bmc-ncsi0.network";
    static constexpr auto line = "HSC4_S5_OR_S0_N-O";

    std::ifstream f{cfg};
    if (!f)
    {
        std::cerr << std::format("vr-nvl-bmc: NCSI: no {}\n", cfg);
        return;
    }
    if (!gpiod::find_line(line))
    {
        std::cerr << std::format("vr-nvl-bmc: NCSI: {} not available\n", line);
        return;
    }

    static constexpr std::string_view ws = " \t\r\v\f";
    std::string policy;
    for (std::string l; policy.empty() && std::getline(f, l);)
    {
        std::string_view v{l};
        v.remove_prefix(std::min(v.find_first_not_of(ws), v.size()));
        if (!v.starts_with("ActivationPolicy"))
            continue;
        v.remove_prefix(std::string_view{"ActivationPolicy"}.size());
        v.remove_prefix(std::min(v.find_first_not_of(ws), v.size()));
        if (!v.starts_with('='))
            continue;
        v.remove_prefix(1);
        v.remove_prefix(std::min(v.find_first_not_of(ws), v.size()));
        policy = v.substr(0, v.find_first_of(" \t\r\v\f#;"));
    }
    std::ranges::transform(policy, policy.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (policy == "down" || policy == "always-down" || policy == "manual")
        gpio::set(line, 0);
    else if (policy == "up" || policy == "always-up" || policy == "bound")
        gpio::set(line, 1);
    else
        std::cerr << std::format(
            "vr-nvl-bmc: NCSI: ActivationPolicy '{}'; leaving {} unchanged\n",
            policy, line);
}

static constexpr auto tag = "vr-nvl-bmc";
// The HPM boards, present while their IOX at 0x20 is bound.
static constexpr std::array<compute::BoardLine, 2> hpm_standby_pg{{
    {"B0_M0", 2, 0x20, "B0_M0_STBY_POWER_PG-I"},
    {"B1_M0", 7, 0x20, "B1_M0_STBY_POWER_PG-I"},
}};

static constexpr std::array<compute::BoardLine, 2> hpm_mcu_ok{{
    {"B0_M0", 2, 0x20, "B0_M0_HPM_MCU_OK-I"},
    {"B1_M0", 7, 0x20, "B1_M0_HPM_MCU_OK-I"},
}};

static constexpr std::array<compute::BoardLine, 2> hpm_cpld_ready{{
    {"B0_M0", 2, 0x20, "B0_M0_CPLD_READY-I"},
    {"B1_M0", 7, 0x20, "B1_M0_CPLD_READY-I"},
}};

// rtc-detection.sh's buses.
static constexpr std::array<unsigned, 2> rtc_buses{{5, 19}};
// The RTC parts the BMC module may carry, at 0x6f.
static constexpr std::array<RtcId, 2> rtc_parts{{
    {0x6f, 0x10, 0x5c, "nct3018y"},
    {0x6f, 0x10, 0x4e, "pcf85053a"},
}};
static constexpr compute::Rtc rtc{rtc_buses, rtc_parts};

int init_vr_nvl_bmc()
{
    std::cerr << "vr-nvl-bmc: platform init start\n";

    compute::create_state_dir();

    if (compute::bmc_cold_boot("B0_M0_STBY_POWER_PG-I"))
    {
        gpio::set("GLOBAL_WP-O", 0);
        mask_pdb_hsc_faults(9);
        configure_pdb_tmp_alert_threshold(9);
        gpio::set("BF3_AOC_NCSI_PRSNT_L-I", 0);
        gpio::set("MUX_I2C_ESPI_SEL-O", 0);
        gpio::set("1GB_NIC_ENABLE-O", 1);
    }

    compute::bind_expanders(9, {0x20, 0x75});

    apply_ncsi_hsc_gpio_from_networkd();

    compute::enable_pdb_standby_power(tag, 9);
    if (!compute::wait_bmc_standby_power(tag, compute::PgSource::Gpio,
                                         "STBY_POWER_PG-I", "STBY_POWER_PG-I"))
        return compute::init_failed(rtc);

    gpio::set("HMC_RST_R_L-O", 1);

    if (!compute::wait_pdb_standby_power(tag, 9, compute::PgSource::Gpio))
        return compute::init_failed(rtc);

    hmc_mux_select("HMC_PRSNT_R-I", "BMC_HMC_MUX_SEL-O");

    compute::bind_expanders(1, {0x20, 0x21});
    compute::bind_expanders(2, {0x20, 0x21, 0x22, 0x23, 0x24});
    compute::bind_expanders(7, {0x20, 0x21, 0x22, 0x23, 0x24});

    // Unused SOCAMM select pins: drive them low instead of floating.
    for (auto [bus, board] : {std::pair{2, "B0"}, std::pair{7, "B1"}})
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

    // USB recovery is best effort and must not block BMC startup.
    if (!compute::reset_usb_hub_if_absent(tag, "B0_M0_USB_HUB0_RST_L-O",
                                          "/dev/ttyUSB0"))
        std::cerr << tag << ": USB_HUB0 reset failed on board 0; continuing\n";
    if (!compute::reset_usb_hub_if_absent(tag, "B1_M0_USB_HUB0_RST_L-O",
                                          "/dev/ttyUSB4"))
        std::cerr << tag << ": USB_HUB0 reset failed on board 1; continuing\n";

    if (!compute::check_rofs_and_mtd(tag))
        return compute::init_failed(rtc);

    // Before READY: FruDevice and EntityManager start after it.
    select_pdb_profile(9);

    return compute::bmc_ready(tag, rtc);
}

} // namespace nvidia
