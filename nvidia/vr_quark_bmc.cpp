// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-vr-quark-bmc.
// Replaces bmc_ready.sh. Quark is the vHMC carrier board (NVL144 PDB).
// vHMC container lifecycle scripts remain in scripts/platforms/vr-quark-bmc/vhmc/

#include "platform_utils.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace nvidia
{

// ── Platform config ───────────────────────────────────────────────────────
// GPIO line names sourced from nvidia-vr-quark-bmc-core.dtsi

// Source: gpio_check — GPIOs asserted by this init sequence, verified after EEPROM init
static constexpr std::array<GpioLevel, 5> gpio_checks{{
    {"FPGA_READY_BMC-I",             1},
    {"BMC_READY-O",                  1},
    {"STBY_POWER_EN-O",              1},
    {"BMC_EROT_FPGA_SPI_MUX_SEL-O", 1},
    {"EROT_FPGA_RST_L-O",            1},
}};

// Source: create_eeprom_devices.sh (Quark variant)
static constexpr std::array<EepromDevice, 9> boot_eeproms{{
    {1,  0x54, "24c02",  "CBC 0 FRU",    false},
    {1,  0x55, "24c02",  "CBC 1 FRU",    false},
    {2,  0x57, "24c02",  "HMC FRU",      false},
    {2,  0x50, "24c64",  "Module 0 FRU", false},
    {9,  0x50, "24c64",  "Module 0 Aux", false},
    {9,  0x51, "24c64",  "Module 1 Aux", false},
    {14, 0x50, "24c128", "BF3 Left FRU", false},
    {15, 0x50, "24c128", "BF3 Right FRU",false},
    {21, 0x50, "24c64",  "IO Board FRU", false},
}};


// ── Quark-specific: FAN PWM initialization ────────────────────────────────
// Source: init_quark_pwms() in bmc_ready.sh
// Quark: FAN controller power is BMC-owned. Pre-set PWMs to 38 (~15% duty)
// before asserting BMC_FAN_PWR_EN-O to avoid fan surge at full speed.
static void init_quark_pwms()
{
    std::cerr << "vr-quark-bmc: init FAN PWMs to 38\n";
    for (std::string_view hwmon : {"hwmon0", "hwmon1"})
        for (std::string_view pwm : {"pwm1", "pwm2", "pwm3"})
            set_hwmon_pwm(hwmon, pwm, 38);
}

// ── Platform init ─────────────────────────────────────────────────────────

int init_vr_quark_bmc()
{
    std::cerr << "vr-quark-bmc: platform init start\n";

    // Source: bmc_set_initial_gpio_out() in bmc_ready.sh, called when
    //         B0_M0_STBY_POWER_PG-I == 0.
    if (gpio::get("B0_M0_STBY_POWER_PG-I") == 0)
    {
        // Source: bmc_set_initial_gpio_out() in bmc_ready.sh
        gpio::set("GLOBAL_WP_BMC-O", 0);
        gpio::set("HMC_RST_R_L-O",   0);
    }

    // Source: PDB IOX bind block in bmc_ready.sh (main)
    // 9-0020: NVL144 PDB (Strata)
    iox_bind(9, 0x20);
    // 9-0075: Parsec EB2 variant
    iox_bind(9, 0x75);

    gpio::Event stby_pg("STBY_POWER_PG-I", 1);
    gpio::Event pdb_pg("STBY_PWR_OK-I", 1);

    // Source: `if [ -d .../9-0020 ]; then set_gpio_level "STBY_POWER_EN-O" $HIGH`
    //         in bmc_ready.sh
    if (iox_present(9, 0x20))
    {
        std::cerr << "vr-quark-bmc: PDB IOX 9-0020 present — asserting STBY_POWER_EN-O\n";
        gpio::set("STBY_POWER_EN-O", 1);
    }

    // Source: wait_standby_pwr "BMC" in bmc_ready.sh
    std::cerr << "vr-quark-bmc: waiting for STBY_POWER_PG-I\n";
    if (stby_pg.wait() != gpio::EventResult::Asserted)
    {
        std::cerr << "vr-quark-bmc: FATAL: STBY_POWER_PG-I timeout\n";
        return EXIT_FAILURE;
    }

    // Source: `set_gpio_level "HMC_RST_R_L-O" $HIGH` in bmc_ready.sh
    gpio::set("HMC_RST_R_L-O", 1);

    // Source: `set_gpio_level "USB_PWR_EN-O" $HIGH` in bmc_ready.sh
    gpio::set("USB_PWR_EN-O", 1);

    // Source: init_quark_pwms(); set_gpio_level "BMC_FAN_PWR_EN-O" $HIGH
    //         in bmc_ready.sh
    init_quark_pwms();
    gpio::set("BMC_FAN_PWR_EN-O", 1);

    // Source: `set_gpio_level "USB3_MUX_EN_L-O" $LOW` in bmc_ready.sh
    // Quark workaround: USB3_MUX=low to enable USB 2.0 path between FIO board
    // USB3_A connector and host's USB controller.
    gpio::set("USB3_MUX_EN_L-O", 0);

    // Source: `if [ -d .../9-0020 ]; then wait_standby_pwr "PDB"` in bmc_ready.sh
    if (iox_present(9, 0x20))
    {
        std::cerr << "vr-quark-bmc: waiting for PDB STBY_PWR_OK-I\n";
        if (pdb_pg.wait() != gpio::EventResult::Asserted)
        {
            std::cerr << "vr-quark-bmc: FATAL: STBY_PWR_OK-I (PDB standby) timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: check_iox_2_20_present() in bmc_ready.sh, exit 1 on failure
    std::cerr << "vr-quark-bmc: waiting for IOX 2-0020\n";
    if (!iox_wait(2, 0x20, std::chrono::seconds{30}))
    {
        std::cerr << "vr-quark-bmc: FATAL: IOX 2-0020 not present\n";
        return EXIT_FAILURE;
    }

    // Source: `#stby_up=$(check_iox_7_20_present)` commented out in bmc_ready.sh
    // No 2P for Quark

    // Source: hmc_bypass() in bmc_ready.sh
    hmc_mux_select();

    // Source: bind_gpio_expanders() in bmc_ready.sh (Quark: 12 expanders + 5-0022)
    // 1-0020, 1-0021: HMC bypass
    iox_bind(1, 0x20);
    iox_bind(1, 0x21);
    // 2-0020 … 2-0024: B0_M0 MGMT board
    iox_bind(2, 0x20);
    iox_bind(2, 0x21);
    iox_bind(2, 0x22);
    iox_bind(2, 0x23);
    iox_bind(2, 0x24);
    // 7-0020 … 7-0024: B1_M0 MGMT board
    iox_bind(7, 0x20);
    iox_bind(7, 0x21);
    iox_bind(7, 0x22);
    iox_bind(7, 0x23);
    iox_bind(7, 0x24);
    // 5-0022: 3V3_IOX (E5074 VR Tester)
    iox_bind(5, 0x22);

    // Source: bmc_set_stby_pg_gpio_out() in bmc_ready.sh
    gpio::set("PWR_BRAKE_L-O",  1);
    gpio::set("SHDN_REQ_L-O",   1);
    gpio::set("SHDN_FORCE_L-O", 1);
    gpio::set("SYS_RST_IN_L-O", 0);

    // Source: `if [ -d .../2-0020 ]; then wait_standby_pwr "B0_M0"` in bmc_ready.sh
    if (iox_present(2, 0x20))
    {
        std::cerr << "vr-quark-bmc: waiting for B0_M0_STBY_POWER_PG-I\n";
        if (!wait_gpio_high("B0_M0_STBY_POWER_PG-I", 200, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-quark-bmc: FATAL: B0_M0_STBY_POWER_PG-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../2-0020 ]; then wait_gpio_assert "B0_M0_HPM_MCU_OK-I" 100 0.1`
    //         + USB_HUB reset workaround in bmc_ready.sh
    if (iox_present(2, 0x20))
    {
        if (!wait_gpio_high("B0_M0_HPM_MCU_OK-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-quark-bmc: FATAL: B0_M0_HPM_MCU_OK-I timeout\n";
            return EXIT_FAILURE;
        }

        // Source: USB_HUB reset workaround block within the 2-0020 guard in bmc_ready.sh
        if (!std::filesystem::exists("/dev/ttyUSB0"))
        {
            std::cerr << "vr-quark-bmc: resetting USB_HUB0 on board 0\n";
            gpio::set("B0_M0_USB_HUB0_RST_L-O", 0);
            gpio::set("B0_M0_USB_HUB0_RST_L-O", 1);
        }
    }

    // Source: `if [ -d .../7-0020 ]; then wait_gpio_assert "B1_M0_HPM_MCU_OK-I" 100 0.1`
    //         in bmc_ready.sh
    if (iox_present(7, 0x20))
    {
        if (!wait_gpio_high("B1_M0_HPM_MCU_OK-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-quark-bmc: FATAL: B1_M0_HPM_MCU_OK-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../2-0020 ]; then wait_gpio_assert "B0_M0_CPLD_READY-I" 100 0.1`
    //         in bmc_ready.sh, exit 1 on failure
    if (iox_present(2, 0x20))
    {
        if (!wait_gpio_high("B0_M0_CPLD_READY-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-quark-bmc: FATAL: B0_M0_CPLD_READY-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../7-0020 ]; then wait_gpio_assert "B1_M0_CPLD_READY-I" 100 0.1`
    //         in bmc_ready.sh, exit 1 on failure
    if (iox_present(7, 0x20))
    {
        if (!wait_gpio_high("B1_M0_CPLD_READY-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-quark-bmc: FATAL: B1_M0_CPLD_READY-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: bmc_boot_banner() in bmc_ready.sh
    bmc_boot_banner();

    // Source: check_rw_filesystems() + check_rofs() in bmc_ready.sh
    emmc_partition("mmcblk0");
    filesystem_check();
    partition_check("mmcblk0");

    // Source: create_eeprom_devices.sh (boot-time EEPROMs) in bmc_ready.sh
    std::cerr << "vr-quark-bmc: creating boot-time EEPROM devices\n";
    eeprom_init(boot_eeproms);

    gpio_check(gpio_checks);

    // Source: nvme_cpld_probe.sh (nvidia-nvme-cpld recipe)
    nvme_cpld_probe(14);
    nvme_cpld_probe(15);

    // Source: set_bmc_ready() in bmc_ready.sh
    gpio::set("BMC_READY-O",     1);
    gpio::set("GLOBAL_WP_BMC-O", 0);

    sd_notify(0, "READY=1");
    std::cerr << "vr-quark-bmc: platform init complete\n";

    // Source: rtc-detection.service (After=bmc-boot-complete.service)
    // Quark uses buses 6 and 19 (differs from nvl/c2/e5010 which use 5 and 19)
    static constexpr std::array<unsigned, 2> rtc_buses{{6, 19}};
    rtc_detect(rtc_buses);
    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
