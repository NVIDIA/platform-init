// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-vr-nvl-hmc (AST2700 / A1 variant).
// Functionally equivalent to init_vr_nvl_hmc() but with AST2700 bus remapping:
// baseboard FRU EEPROM moves from bus 10 to bus 4.

#include "platform_utils.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace nvidia
{

// ── Platform config ───────────────────────────────────────────────────────
// GPIO line names sourced from nvidia-vr-nvl-hmc-core.dtsi

// Source: gpio_check — GPIOs asserted by this init sequence, verified after EEPROM init
static constexpr std::array<GpioLevel, 7> gpio_checks{{
    {"HMC_READY-O",             1},
    {"FPGA_READY-I",            1},
    {"EROT_FPGA_RST_L-O",       1},
    {"FPGA_RST_L-O",            1},
    {"HMC_GLOBAL_WP-I",         1},
    {"EROT_FPGA_SPI_MUX_SEL-O", 1},
    {"EROT_FPGA_RECOVERY_L-I",  1},
}};

// Source: create_eeprom_devices.sh (HMC variant)
static constexpr std::array<EepromDevice, 2> boot_eeproms{{
    {2, 0x57, "24c02", "HMC FRU",      false},
    {2, 0x50, "24c64", "Module 0 FRU", false},
}};


// ── Platform init ─────────────────────────────────────────────────────────

int init_vr_nvl_hmc_ast2700()
{
    std::cerr << "vr-nvl-hmc-ast2700: platform init start\n";

    // Source: standby_power_wait.sh (AST2700 variant; bus mapping differs from AST2600)
    // AST2700: BRD0 = bus 10 (AST2600: bus 1), BRD1 = bus 8 (AST2600: bus 7)
    std::cerr << "vr-nvl-hmc-ast2700: waiting for BRD0 standby power (bus 10)\n";
    if (iox_wait(10, 0x20, std::chrono::seconds{600}))
    {
        std::cerr << "vr-nvl-hmc-ast2700: BRD0 standby power up, binding IO expanders\n";
        iox_bind(10, 0x20);
        iox_bind(10, 0x21);
        if (iox_wait(8, 0x20, std::chrono::seconds{10}))
        {
            iox_bind(8, 0x20);
            iox_bind(8, 0x21);
        }
        else
            std::cerr << "vr-nvl-hmc-ast2700: BRD1 not present (single-board)\n";
    }
    else
        std::cerr << "vr-nvl-hmc-ast2700: BRD0 standby power timeout — continuing\n";

    // Source: hmc_set_initial_gpio_out() in hmc_ready.sh
    std::cerr << "vr-nvl-hmc-ast2700: initializing HMC GPIO output states\n";
    gpio::set("QSPI2_RST_L-O",  1);
    gpio::set("BMC_DDR4_TEN-O", 0);
    gpio::set("HUB_RST_L-O",    1);
    gpio::set("EMMC_RST_L-O",   1);

    // Source: hmc_set_initial_gpio_out() — CPU recovery type = USB-RCM
    gpio::set("BRD0_CPU_RECOVERY_TYPE0-O", 0);
    gpio::set("BRD0_CPU_RECOVERY_TYPE1-O", 1);
    gpio::set("BRD1_CPU_RECOVERY_TYPE0-O", 0);
    gpio::set("BRD1_CPU_RECOVERY_TYPE1-O", 1);

    // Source: hmc_set_initial_gpio_out() — CPU boot device select = 0
    gpio::set("BRD0_CPU_BOOT_DEV_SEL0-O", 0);
    gpio::set("BRD0_CPU_BOOT_DEV_SEL1-O", 0);
    gpio::set("BRD0_CPU_BOOT_DEV_SEL2-O", 0);
    gpio::set("BRD1_CPU_BOOT_DEV_SEL0-O", 0);
    gpio::set("BRD1_CPU_BOOT_DEV_SEL1-O", 0);
    gpio::set("BRD1_CPU_BOOT_DEV_SEL2-O", 0);

    // Source: `gpioset BRD0_HMC_TO_MCU_I2C_EN-O=1` in hmc_ready_sequence()
    //         in hmc_ready.sh
    std::cerr << "vr-nvl-hmc-ast2700: taking Board 0 HPM MCU out of reset\n";
    gpio::set("BRD0_HMC_TO_MCU_I2C_EN-O", 1);

    // Source: `gpioset BRD1_HMC_TO_MCU_I2C_EN-O=1` in hmc_ready_sequence()
    //         in hmc_ready.sh
    std::cerr << "vr-nvl-hmc-ast2700: taking Board 1 HPM MCU out of reset\n";
    gpio::set("BRD1_HMC_TO_MCU_I2C_EN-O", 1);

    // Source: hmc_fru_checker.sh (logic migrated to native C++; script eliminated)
    // AST2700 variant: baseboard FRU EEPROM is on bus 4 addr 0x50 (was bus 10).
    // Confirmed by vr-nvl-hmc-ast2700-a1/hmc_fru_checker.sh and HMC.json (Bus: 4).
    std::cerr << "vr-nvl-hmc-ast2700: syncing HMC baseboard FRU\n";
    {
        static constexpr std::array<EepromDevice, 1> fru_eeprom{{{4, 0x50, "24c02", "HMC Baseboard FRU", false}}};
        eeprom_init(fru_eeprom);

        static constexpr auto eeprom_path = "/sys/bus/i2c/devices/4-0050/eeprom";
        static constexpr auto fru_dir     = "/etc/fru";
        static constexpr auto fru_file    = "/etc/fru/baseboard.fru.bin";
        static constexpr std::size_t fru_size  = 512;
        static constexpr int max_retries   = 10;

        std::filesystem::create_directories(fru_dir);

        std::vector<char> buf(fru_size, 0);
        bool read_ok = false;
        for (int i = 0; i < max_retries && !read_ok; ++i)
        {
            if (std::ifstream f{eeprom_path, std::ios::binary}; f)
            {
                f.read(buf.data(), static_cast<std::streamsize>(fru_size));
                if (f.gcount() > 0)
                    read_ok = true;
            }
            if (!read_ok)
            {
                std::cerr << std::format("vr-nvl-hmc-ast2700: FRU EEPROM read retry {}/{}\n",
                                         i + 1, max_retries);
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }

        if (!read_ok)
        {
            std::cerr << "vr-nvl-hmc-ast2700: FRU EEPROM read failed — creating empty FRU file\n";
            std::ofstream{fru_file}; // empty file lets FruDevice start
        }
        else
        {
            bool needs_update = true;
            if (std::ifstream existing{fru_file, std::ios::binary}; existing)
            {
                std::vector<char> prev(fru_size, 0);
                existing.read(prev.data(), static_cast<std::streamsize>(fru_size));
                if (prev == buf)
                    needs_update = false;
            }
            if (needs_update)
            {
                if (std::ofstream out{fru_file, std::ios::binary}; out)
                    out.write(buf.data(), static_cast<std::streamsize>(fru_size));
                else
                    std::cerr << "vr-nvl-hmc-ast2700: failed to write FRU file\n";
            }
        }
    }

    // Source: `sleep $erot_reset_delay` (erot_reset_delay=10) in hmc_ready.sh
    std::cerr << "vr-nvl-hmc-ast2700: sleeping 10s for ERoT reset delay\n";
    std::this_thread::sleep_for(std::chrono::seconds{10});

    // Source: check_rw_filesystems() + check_rofs() in hmc_ready_sequence()
    //         in hmc_ready.sh
    if (filesystem_check() != EXIT_SUCCESS)
    {
        std::cerr << "vr-nvl-hmc-ast2700: FATAL: filesystem check failure\n";
        return EXIT_FAILURE;
    }

    // Source: nvidia-emmc-partition.service (create-partition.sh +
    //         emmc-partition-format.sh). Layout identical to vr_bmc_emmc_layout.
    emmc_partition("mmcblk0");
    emmc_format_and_mount("mmcblk0");

    // Source: create_eeprom_devices.sh (HMC boot EEPROMs)
    std::cerr << "vr-nvl-hmc-ast2700: creating boot-time EEPROM devices\n";
    eeprom_init(boot_eeproms);

    gpio_check(gpio_checks);

    // Source: set_hmc_ready() in hmc_ready.sh
    gpio::set("HMC_READY-O", 1);

    sd_notify(0, "READY=1");
    std::cerr << "vr-nvl-hmc-ast2700: platform init complete\n";

    // Source: bmc_reset_check.sh / bmc-reset-check.service
    bmc_reset_check();

    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
