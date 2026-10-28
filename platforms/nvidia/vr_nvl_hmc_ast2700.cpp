// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for vr-nvl-hmc (AST2700).

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

static constexpr auto hmc_ready_pin =
    "/sys/class/leds/hmc_ready_noled/brightness";

int init_vr_nvl_hmc_ast2700()
{
    static constexpr auto tag = "vr-nvl-hmc-ast2700";
    std::cerr << "vr-nvl-hmc-ast2700: platform init start\n";

    compute::create_state_dir();

    compute::hmc_set_initial_gpio_out(tag, initial_outputs);
    compute::release_hpm_mcu_reset(tag, mcu_enable_lines);

    std::this_thread::sleep_for(compute::erot_reset_delay);

    if (!compute::check_hmc_filesystems())
        return EXIT_FAILURE;

    return compute::hmc_ready(tag, hmc_ready_pin);
}

} // namespace nvidia
