// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for vr-nvl-hmc.

#include "compute_hmc_common.hpp"
#include "utilities.hpp"

#include <array>
#include <iostream>
#include <thread>

namespace nvidia
{

// Initial HMC outputs: SPI and hub out of reset, CPU recovery type USB-RCM,
// CPU boot device 0.
static constexpr std::array<compute::GpioOut, 13> initial_outputs{{
    {"QSPI2_RST_L-O", 1},
    {"BMC_DDR4_TEN-O", 0},
    {"HUB_RST_L-O", 1},
    {"BRD0_CPU_RECOVERY_TYPE0-O", 0},
    {"BRD0_CPU_RECOVERY_TYPE1-O", 1},
    {"BRD1_CPU_RECOVERY_TYPE0-O", 0},
    {"BRD1_CPU_RECOVERY_TYPE1-O", 1},
    {"BRD0_CPU_BOOT_DEV_SEL0-O", 0},
    {"BRD0_CPU_BOOT_DEV_SEL1-O", 0},
    {"BRD0_CPU_BOOT_DEV_SEL2-O", 0},
    {"BRD1_CPU_BOOT_DEV_SEL0-O", 0},
    {"BRD1_CPU_BOOT_DEV_SEL1-O", 0},
    {"BRD1_CPU_BOOT_DEV_SEL2-O", 0},
}};

static constexpr std::array<const char*, 2> mcu_enable_lines{
    "BRD0_HMC_TO_MCU_I2C_EN-O", "BRD1_HMC_TO_MCU_I2C_EN-O"};

// BRD0 bus 1 must come up within 120 s; BRD1 bus 7 is optional.
static constexpr compute::StandbyConfig standby{
    .brd0 = {.bus = 1, .iox = {0x20, 0x21}, .attempts = 120, .fru = 0x50},
    .brd1 = {.bus = 7, .iox = {0x20, 0x21}, .attempts = 5, .fru = 0x50}};

// rtc-sync.sh: the RTC at 0x6f on bus 9 is one of these parts.
static constexpr std::array<unsigned, 1> rtc_buses{9};
static constexpr std::array<RtcId, 2> rtc_parts{{
    {0x6f, 0x10, 0x5c, "nct3018y"},
    {0x6f, 0x10, 0x4e, "pcf85053a"},
}};
static constexpr compute::RtcSync rtc_sync{
    .external = {rtc_buses, rtc_parts},
    .host_power_good = "BRD0_MODULE_PWR_GOOD-I"};

static constexpr auto hmc_ready_pin =
    "/sys/class/leds/hmc_ready_noled/brightness";

// Before sysinit.target, as the initramfs ran it: the init's GPIO outputs
// and the RTC sync's power-good input live on the expanders the wait binds.
int early_init_vr_nvl_hmc()
{
    static constexpr auto tag = "vr-nvl-hmc";
    compute::wait_for_standby_power(tag, standby);
    compute::sync_clock_from_rtc(tag, rtc_sync);
    return EXIT_SUCCESS;
}

int init_vr_nvl_hmc()
{
    static constexpr auto tag = "vr-nvl-hmc";
    std::cerr << "vr-nvl-hmc: platform init start\n";

    compute::create_state_dir();

    compute::hmc_set_initial_gpio_out(tag, initial_outputs);
    compute::release_hpm_mcu_reset(tag, mcu_enable_lines);

    std::this_thread::sleep_for(compute::erot_reset_delay);

    // hmc-mailbox reports and clears dataflashrecovery.
    if (filesystem_check_mtd("/run/initramfs/rw", "/run/initramfs/rw/work",
                             "/dev/mtd6", false) != EXIT_SUCCESS)
    {
        std::cerr << "vr-nvl-hmc: FATAL: filesystem check failure\n";
        return EXIT_FAILURE;
    }

    return compute::hmc_ready(tag, hmc_ready_pin);
}

} // namespace nvidia
