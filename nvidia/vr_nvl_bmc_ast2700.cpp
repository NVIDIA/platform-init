// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-vr-nvl-bmc (AST2700 / P3809 variant).
// Replaces bmc_ready.sh. Functionally equivalent to init_vr_nvl_bmc() but with
// AST2700 bus remapping: PDB 9→0, HMC bypass 1→10, B0_M0 2→11, B1_M0 7→8.
// HSC also moves to bus 0. PDB standby GPIO is PWBY_PWR_OK-I (vs STBY_PWR_OK-I).
// No BMC_READY-O, EEPROM init, NVMe CPLD probe, or post-ready ERoT checkout on AST2700.

#include "platform_utils.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <filesystem>
#include <iostream>

namespace nvidia
{

// ── Platform init ─────────────────────────────────────────────────────────

int init_vr_nvl_bmc_ast2700()
{
    std::cerr << "vr-nvl-bmc-ast2700: platform init start\n";

    // Source: standby_power_wait.sh (AST2700 variant)
    // AST2700: BRD0 HMC-bypass IOX = bus 10, BRD1 = bus 8 (AST2600: buses 1/7)
    std::cerr << "vr-nvl-bmc-ast2700: waiting for BRD0 standby power (bus 10)\n";
    if (iox_wait(10, 0x20, std::chrono::seconds{600}))
    {
        std::cerr << "vr-nvl-bmc-ast2700: BRD0 standby power up, binding HMC bypass IOX\n";
        iox_bind(10, 0x20);
        iox_bind(10, 0x21);
        if (iox_wait(8, 0x20, std::chrono::seconds{10}))
        {
            iox_bind(8, 0x20);
            iox_bind(8, 0x21);
        }
        else
            std::cerr << "vr-nvl-bmc-ast2700: BRD1 not present (single-board)\n";
    }
    else
        std::cerr << "vr-nvl-bmc-ast2700: BRD0 standby power timeout — continuing\n";

    // ── Initial delay ─────────────────────────────────────────────────────
    // Source: `sleep 60` at start of bmc_ready.sh
    // Workaround for observed timing issue with B0_M0_RUN_POWER_PG-I not
    // asserting when powerctrl.sh power_on runs.
    std::cerr << "vr-nvl-bmc-ast2700: initial 60s delay (RUN_POWER_PG timing workaround)\n";
    std::this_thread::sleep_for(std::chrono::seconds{60});

    // Source: bmc_set_initial_gpio_out() in bmc_ready.sh, called when
    //         B0_M0_STBY_POWER_PG-I == 0.
    if (gpio::get("B0_M0_STBY_POWER_PG-I") == 0)
    {
        gpio::set("GLOBAL_WP_BMC-O", 0);

        // Source: HSC mask-alert + clear-faults loop in bmc_set_initial_gpio_out()
        // hsc_bus=0 on AST2700 (was bus 9 on AST2600)
        // HSC mask all alerts:  SMBus write-word 0xd8 <= 0xffff
        // HSC clear faults:      SMBus send-byte  0x03
        static constexpr uint8_t hsc_addrs[] = {0x10, 0x12, 0x14, 0x16};
        for (uint8_t addr : hsc_addrs)
        {
            try
            {
                i2c::RawDevice dev{0, addr};
                if (int rc = dev.write_word(0xd8, 0xffff); rc != 0)
                    std::cerr << std::format(
                        "vr-nvl-bmc-ast2700: HSC mask alert failed bus=0 addr={:#x}: {}\n",
                        addr, std::strerror(rc));
                if (int rc = dev.send_byte(0x03); rc != 0)
                    std::cerr << std::format(
                        "vr-nvl-bmc-ast2700: HSC clear faults failed bus=0 addr={:#x}: {}\n",
                        addr, std::strerror(rc));
            }
            catch (const std::exception& e)
            {
                std::cerr << std::format(
                    "vr-nvl-bmc-ast2700: HSC bus=0 addr={:#x} not accessible: {}\n",
                    addr, e.what());
            }
        }

        gpio::set("BF3_AOC_NCSI_PRSNT_L-I", 0);
        gpio::set("MUX_I2C_ESPI_SEL-O",      0);
        gpio::set("EEDO_LED2-O",              1);
    }

    // Source: PDB IOX bind block in bmc_ready.sh (main); bus 0 on AST2700 (was 9)
    iox_bind(0, 0x20);

    // Source: PDB IOX bind block in bmc_ready.sh (main); bus 0 on AST2700 (was 9)
    iox_bind(0, 0x75);

    // Source: `set_gpio_level "PDB_12V_EN_N_R-O" $HIGH; sleep 1` in bmc_ready.sh
    gpio::set("PDB_12V_EN_N_R-O", 1);
    std::this_thread::sleep_for(std::chrono::seconds{1});

    gpio::Event stby_pg("STBY_POWER_PG-I", 1);

    // Source: `if [ -d .../0-0020 ]; then set_gpio_level "STBY_POWER_EN-O" $HIGH`
    if (iox_present(0, 0x20))
    {
        std::cerr << "vr-nvl-bmc-ast2700: PDB IOX 0-0020 present — asserting STBY_POWER_EN-O\n";
        gpio::set("STBY_POWER_EN-O", 1);
    }

    // Source: wait_standby_pwr "BMC" in bmc_ready.sh
    std::cerr << "vr-nvl-bmc-ast2700: waiting for STBY_POWER_PG-I\n";
    if (stby_pg.wait() != gpio::EventResult::Asserted)
    {
        std::cerr << "vr-nvl-bmc-ast2700: FATAL: STBY_POWER_PG-I timeout\n";
        return EXIT_FAILURE;
    }

    // NOTE: AST2700 PDB standby is PWBY_PWR_OK-I (was STBY_PWR_OK-I on AST2600)
    gpio::Event pdb_pg("PWBY_PWR_OK-I", 1);

    // Source: `set_gpio_level "PDB_12V_EN_N_R-O" $LOW` in bmc_ready.sh
    // NOTE: HMC_RST_R_L release is intentionally omitted — commented out in bmc_ready.sh
    gpio::set("PDB_12V_EN_N_R-O", 0);

    // Source: `if [ -d .../0-0020 ]; then wait_standby_pwr "PDB"` in bmc_ready.sh
    if (iox_present(0, 0x20))
    {
        std::cerr << "vr-nvl-bmc-ast2700: waiting for PDB PWBY_PWR_OK-I\n";
        if (pdb_pg.wait() != gpio::EventResult::Asserted)
        {
            std::cerr << "vr-nvl-bmc-ast2700: FATAL: PWBY_PWR_OK-I (PDB standby) timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: check_iox_11_20_present() in bmc_ready.sh; exit 1 on failure
    // AST2700 bus 11 (was bus 2 on AST2600)
    std::cerr << "vr-nvl-bmc-ast2700: waiting for IOX 11-0020\n";
    if (!iox_wait(11, 0x20, std::chrono::seconds{30}))
    {
        std::cerr << "vr-nvl-bmc-ast2700: FATAL: IOX 11-0020 not present\n";
        return EXIT_FAILURE;
    }

    // Source: check_iox_8_20_present() in bmc_ready.sh; exit 1 on failure
    // AST2700 bus 8 (was bus 7 on AST2600)
    std::cerr << "vr-nvl-bmc-ast2700: waiting for IOX 8-0020\n";
    if (!iox_wait(8, 0x20, std::chrono::seconds{30}))
    {
        std::cerr << "vr-nvl-bmc-ast2700: FATAL: IOX 8-0020 not present\n";
        return EXIT_FAILURE;
    }

    // Source: hmc_bypass() in bmc_ready.sh
    hmc_mux_select();

    // Source: bind_gpio_expanders() in bmc_ready.sh
    // AST2700 bus remapping vs AST2600: HMC bypass 1→10, B0_M0 2→11, B1_M0 7→8
    // 10-0020, 10-0021: HMC bypass expanders (AST2700 bus 10, was bus 1)
    iox_bind(10, 0x20);
    iox_bind(10, 0x21);
    // 11-0020 … 11-0024: B0_M0 MGMT board expanders (AST2700 bus 11, was bus 2)
    iox_bind(11, 0x20);
    iox_bind(11, 0x21);
    iox_bind(11, 0x22);
    iox_bind(11, 0x23);
    iox_bind(11, 0x24);
    // 8-0020 … 8-0024: B1_M0 MGMT board expanders (AST2700 bus 8, was bus 7)
    iox_bind(8, 0x20);
    iox_bind(8, 0x21);
    iox_bind(8, 0x22);
    iox_bind(8, 0x23);
    iox_bind(8, 0x24);

    // Source: `set_gpio_level "USB_MUX_EN-O" $HIGH` in bmc_ready.sh
    gpio::set("USB_MUX_EN-O", 1);

    // Source: `set_gpio_level "BMC_MUX_PI3DP_SEL-O" $LOW` in bmc_ready.sh
    gpio::set("BMC_MUX_PI3DP_SEL-O", 0);

    // Source: `if [ -d .../11-0020 ]; then wait_standby_pwr "B0_M0"` in bmc_ready.sh
    if (iox_present(11, 0x20))
    {
        std::cerr << "vr-nvl-bmc-ast2700: waiting for B0_M0_STBY_POWER_PG-I\n";
        if (!wait_gpio_high("B0_M0_STBY_POWER_PG-I", 200, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc-ast2700: FATAL: B0_M0_STBY_POWER_PG-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../8-0020 ]; then wait_standby_pwr "B1_M0"` in bmc_ready.sh
    if (iox_present(8, 0x20))
    {
        std::cerr << "vr-nvl-bmc-ast2700: waiting for B1_M0_STBY_POWER_PG-I\n";
        if (!wait_gpio_high("B1_M0_STBY_POWER_PG-I", 200, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc-ast2700: FATAL: B1_M0_STBY_POWER_PG-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../11-0020 ]; then wait_gpio_assert "B0_M0_HPM_MCU_OK-I" 100 0.1`
    if (iox_present(11, 0x20))
    {
        if (!wait_gpio_high("B0_M0_HPM_MCU_OK-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc-ast2700: FATAL: B0_M0_HPM_MCU_OK-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../8-0020 ]; then wait_gpio_assert "B1_M0_HPM_MCU_OK-I" 100 0.1`
    if (iox_present(8, 0x20))
    {
        if (!wait_gpio_high("B1_M0_HPM_MCU_OK-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc-ast2700: FATAL: B1_M0_HPM_MCU_OK-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../11-0020 ]; then wait_gpio_assert "B0_M0_CPLD_READY-I" 100 0.1`
    if (iox_present(11, 0x20))
    {
        if (!wait_gpio_high("B0_M0_CPLD_READY-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc-ast2700: FATAL: B0_M0_CPLD_READY-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../8-0020 ]; then wait_gpio_assert "B1_M0_CPLD_READY-I" 100 0.1`
    if (iox_present(8, 0x20))
    {
        if (!wait_gpio_high("B1_M0_CPLD_READY-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc-ast2700: FATAL: B1_M0_CPLD_READY-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: bmc_boot_banner() in bmc_ready.sh
    bmc_boot_banner();

    // Source: USB_HUB reset workaround block in bmc_ready.sh
    // The USB_HUB needs to be reset once while AC ON.
    if (!std::filesystem::exists("/dev/ttyUSB0"))
    {
        std::cerr << "vr-nvl-bmc-ast2700: resetting USB_HUB0 on board 0\n";
        gpio::set("B0_M0_USB_HUB0_RST_L-O", 0);
        gpio::set("B0_M0_USB_HUB0_RST_L-O", 1);
    }
    if (!std::filesystem::exists("/dev/ttyUSB4"))
    {
        std::cerr << "vr-nvl-bmc-ast2700: resetting USB_HUB0 on board 1\n";
        gpio::set("B1_M0_USB_HUB0_RST_L-O", 0);
        gpio::set("B1_M0_USB_HUB0_RST_L-O", 1);
    }

    // Source: check_rofs() in bmc_ready.sh
    emmc_partition("mmcblk0");

    // Source: check_rofs() + check_rw_filesystems() in bmc_ready.sh
    filesystem_check();

    // Source: check_rw_filesystems() in bmc_ready.sh
    partition_check("mmcblk0");

    // NOTE: BMC_READY-O does not exist on VR P3809 (AST2700 variant).
    // NOTE: No EEPROM init, GPIO validation, NVMe CPLD probe, ERoT checkout,
    //       EEPROM CRC verify, or RTC detection — not present in bmc_ready.sh.
    sd_notify(0, "READY=1");
    std::cerr << "vr-nvl-bmc-ast2700: platform init complete\n";
    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
