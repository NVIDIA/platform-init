// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for vr-c2-bmc.

#include "compute_bmc_common.hpp"
#include "utilities.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>

namespace nvidia
{

using namespace std::chrono_literals;

using I2cDev = std::pair<unsigned, uint8_t>;

static constexpr std::array<I2cDev, 2> gpu1_expanders{{{40, 0x20}, {50, 0x20}}};
static constexpr std::array<I2cDev, 2> e1s_expanders{{{33, 0x20}, {36, 0x20}}};

static constexpr std::array<unsigned, 2> rtc_buses{{5, 19}};
// The RTC parts the BMC module may carry, at 0x6f.
static constexpr std::array<RtcId, 2> rtc_parts{{
    {0x6f, 0x10, 0x5c, "nct3018y"},
    {0x6f, 0x10, 0x4e, "pcf85053a"},
}};
static constexpr compute::Rtc rtc{rtc_buses, rtc_parts};

static constexpr std::chrono::seconds cpld_ready_timeout = 10s;
static constexpr std::chrono::milliseconds cpld_ready_poll = 100ms;

// Binds the I2C mux at <bus>-<addr> to pca954x unless it is bound.
static void i2c_mux_bind(unsigned bus, unsigned addr)
{
    const auto path =
        std::format("/sys/bus/i2c/drivers/pca954x/{}-{:04x}", bus, addr);
    if (std::filesystem::exists(path))
        return;
    if (std::ofstream f{"/sys/bus/i2c/drivers/pca954x/bind"}; f)
    {
        f << std::format("{}-{:04x}", bus, addr);
        std::cerr << std::format("i2c_mux_bind: {}-{:04x} bound\n", bus, addr);
    }
    else
        std::cerr << std::format("i2c_mux_bind: {}-{:04x} FAILED\n", bus, addr);
}

static void unbind_absent_expanders(std::string_view label,
                                    std::span<const I2cDev> devs)
{
    for (const auto& [bus, addr] : devs)
    {
        const auto dev = std::format("{}-{:04x}", bus, addr);
        std::array<uint8_t, 1> reg{0};
        std::array<uint8_t, 1> val{};
        if (i2c::raw_transfer(bus, addr, reg, val) == 0)
        {
            std::cerr << std::format("{}: GPIO Expander {} is present\n", label,
                                     dev);
            continue;
        }
        if (!iox_present(bus, addr))
            continue;
        if (std::ofstream f{"/sys/bus/i2c/drivers/pca953x/unbind"};
            f << dev && f.flush())
            std::cerr << std::format("{}: unbound absent GPIO Expander {}\n",
                                     label, dev);
        else
            std::cerr << std::format(
                "[ERROR] {}: failed to unbind absent GPIO Expander {}\n", label,
                dev);
    }
}

int init_vr_c2_bmc()
{
    static constexpr auto tag = "vr-c2-bmc";
    std::cerr << "vr-c2-bmc: platform init start\n";

    compute::create_state_dir();

    if (compute::bmc_cold_boot("B0_M0_STBY_PWR_PG-I"))
    {
        gpio::set("GLOBAL_WP_BMC-O", 0);
        gpio::set("HMC_RESET_L-O", 0);
        // NIC enable as an output, so it cannot float into an interrupt storm.
        gpio::set("1GB_NIC_ENABLE-O", 1);
    }

    compute::bind_expanders(9, {0x24});

    // PDB type 1 does not use the PDB MCU: hold it in reset.
    gpio::set("PDB_MCU_RECOVERY_L-O", 1);
    gpio::set("PDB_MCU_RST_L-O", gpio_get("PDB_TYPE-I") == 1 ? 0 : 1);

    if (!compute::wait_bmc_standby_power(tag, compute::PgSource::Monitor,
                                         "STBY_POWER_PG-I",
                                         "BMC_PS_3V3_STBY_EN-O"))
        return compute::init_failed(rtc);

    compute::wait_iox(tag, 0x20, {2});

    hmc_mux_select("HMC_PRSNT_R-I", "BMC_HMC_MUX_SEL-O");

    compute::bind_expanders(1, {0x20, 0x21});
    compute::bind_expanders(2, {0x20, 0x21, 0x22, 0x23});

    // Unused and unpulled: keep it at a defined level.
    gpio::set("B0_M0_SNN_GPU_MCU_I2C_ENABLE", 1);

    if (!compute::wait_standby_power(tag, "B0_M0", compute::PgSource::Monitor,
                                     "B0_M0_STBY_PWR_PG-I",
                                     "B0_M0_PS_3V3_STBY_EN-O"))
    {
        std::cerr << "vr-c2-bmc: FATAL: B0_M0 wait_standby_pwr failed\n";
        return compute::init_failed(rtc);
    }

    if (!wait_gpio_asserted("B0_M0_CPLD_READY-I", GpioLevel::High,
                            cpld_ready_timeout, cpld_ready_poll))
    {
        std::cerr << std::format("vr-c2-bmc: FATAL: B0_M0_CPLD_READY-I failed "
                                 "to assert after {} seconds\n",
                                 cpld_ready_timeout.count());
        return compute::init_failed(rtc);
    }

    if (gpio_set_released("B0_M1_I2C_SOCAMM_UPHY_SEL-O", 0))
    {
        i2c_mux_bind(4, 0x70);
        i2c_mux_bind(4, 0x71);
        i2c_mux_bind(4, 0x75);
    }
    else
    {
        std::cerr << "[ERROR] Failed to set B0_M1_I2C_SOCAMM_UPHY_SEL-O to "
                     "low, skipping I2C MUX binding\n";
    }

    unbind_absent_expanders("GPU1", gpu1_expanders);
    unbind_absent_expanders("E1S", e1s_expanders);

    gpio::set("GPU0_MCU_BYPASS_N-O", 1);
    gpio::set("GPU1_MCU_BYPASS_N-O", 1);

    gpio::set("GPU0_MCU_RST_N-O", 1);
    gpio::set("GPU1_MCU_RST_N-O", 1);

    if (!compute::reset_usb_hub_if_absent(tag, "B0_M1_USB1_HUB_RST_L-O",
                                          "/dev/ttyUSB0"))
        return compute::init_failed(rtc);

    if (!compute::check_rofs_and_mtd(tag))
        return compute::init_failed(rtc);

    return compute::bmc_ready(tag, rtc);
}

} // namespace nvidia
