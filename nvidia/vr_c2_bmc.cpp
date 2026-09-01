// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-vr-c2-bmc.
// Replaces bmc_ready.sh. C2 board model is B0_M0.

#include "platform_utils.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <iostream>

namespace nvidia
{

// ── Platform config ───────────────────────────────────────────────────────
// GPIO line names sourced from nvidia-vr-c2-bmc-core.dtsi

// Source: gpio_check — GPIOs asserted by this init sequence, verified after
// EEPROM init
static constexpr std::array<GpioLevel, 5> gpio_checks{{
    {"FPGA_READY_BMC-I", 1},
    {"BMC_READY-O", 1},
    {"STBY_POWER_EN-O", 1},
    {"BMC_EROT_FPGA_SPI_MUX_SEL-O", 1},
    {"EROT_FPGA_RST_L-O", 1},
}};

// Source: create_eeprom_devices.sh / create_poweron_eeprom_devices()
static constexpr std::array<EepromDevice, 7> poweron_eeproms{{
    {30, 0x50, "24c128", "BF3 CEM FRU (P5035 A02)", true},
    {34, 0x50, "24c128", "BF3 CEM FRU (P5035 A01)", true},
    {36, 0x51, "24c02", "E1.S Backplane FRU", true},
    {42, 0x50, "24c128", "GPU 0 CEM FRU", true},
    {46, 0x50, "24c128", "CX8 0 CEM FRU", true},
    {50, 0x50, "24c128", "GPU 1 CEM FRU", true},
    {56, 0x50, "24c128", "CX8 1 CEM FRU", true},
}};

// ── Platform init ─────────────────────────────────────────────────────────

int init_vr_c2_bmc()
{
    std::cerr << "vr-c2-bmc: platform init start\n";

    // Source: bmc_set_initial_gpio_out() in bmc_ready.sh, called when
    //         B0_M0_STBY_PWR_PG-I == 0.
    if (gpio::get("B0_M0_STBY_PWR_PG-I") == 0)
    {
        // Source: bmc_set_initial_gpio_out() in bmc_ready.sh
        gpio::set("GLOBAL_WP_BMC-O", 0);
        gpio::set("HMC_RESET_L-O", 0);
    }

    {
        gpio::Event stby_pg("STBY_POWER_PG-I", 1);

        // Source: PDB IOX bind block in bmc_ready.sh (main, C2 specific)
        iox_bind(9, 0x24);

        // Source: wait_standby_pwr_p4103() in bmc_ready.sh
        std::cerr << "vr-c2-bmc: waiting for STBY_POWER_PG-I\n";
        if (stby_pg.wait() != gpio::EventResult::Asserted)
        {
            std::cerr << "vr-c2-bmc: FATAL: STBY_POWER_PG-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: check_iox_2_20_present() in bmc_ready.sh, exit 1 on failure
    std::cerr << "vr-c2-bmc: waiting for IOX 2-0020\n";
    if (!iox_wait(2, 0x20, std::chrono::seconds{30}))
    {
        std::cerr << "vr-c2-bmc: FATAL: IOX 2-0020 not present\n";
        return EXIT_FAILURE;
    }

    // Source: hmc_bypass() in bmc_ready.sh
    hmc_mux_select();

    // Source: bind_gpio_expanders() in bmc_ready.sh (C2 variant: 5 expanders)
    // 1-0020, 1-0021: HMC bypass
    iox_bind(1, 0x20);
    iox_bind(1, 0x21);
    // 2-0020 … 2-0022: B0_M0 MGMT board
    iox_bind(2, 0x20);
    iox_bind(2, 0x21);
    iox_bind(2, 0x22);

    // Source: wait_standby_pwr "B0_M0" in bmc_ready.sh; 200 × 100ms = 20s
    //         C2 wait_standby_pwr maps "B0_M0" → B0_M0_PS_1V8_STBY_PGOOD-I
    std::cerr << "vr-c2-bmc: waiting for B0_M0_PS_1V8_STBY_PGOOD-I\n";
    if (!wait_gpio_high("B0_M0_PS_1V8_STBY_PGOOD-I", 200,
                        std::chrono::milliseconds{100}))
    {
        std::cerr << "vr-c2-bmc: FATAL: B0_M0_PS_1V8_STBY_PGOOD-I timeout\n";
        return EXIT_FAILURE;
    }

    // Source: wait_gpio_assert "B0_M0_CPLD_READY-I" 100 0.1 in bmc_ready.sh
    if (!wait_gpio_high("B0_M0_CPLD_READY-I", 100,
                        std::chrono::milliseconds{100}))
    {
        std::cerr << "vr-c2-bmc: FATAL: B0_M0_CPLD_READY-I timeout\n";
        return EXIT_FAILURE;
    }

    // Source: `set_gpio_level "B0_M1_I2C_SOCAMM_UPHY_SEL-O" $LOW` in
    // bmc_ready.sh
    gpio::set("B0_M1_I2C_SOCAMM_UPHY_SEL-O", 0);

    // Source: bind_i2c_mux_driver() in bmc_ready.sh (muxes: 4-0070, 4-0071,
    // 4-0075)
    i2c_mux_bind(4, 0x70);
    i2c_mux_bind(4, 0x71);
    i2c_mux_bind(4, 0x75);

    // Source: `set_gpio_level "GPU0_MCU_RST_N-O" $HIGH` in bmc_ready.sh
    //         `set_gpio_level "GPU1_MCU_RST_N-O" $HIGH`
    gpio::set("GPU0_MCU_RST_N-O", 1);
    gpio::set("GPU1_MCU_RST_N-O", 1);

    // Source: bmc_set_stby_pg_gpio_out() in bmc_ready.sh
    gpio::set("PWR_BRAKE_L-O", 1);
    gpio::set("SHDN_REQ_L-O", 1);
    gpio::set("SHDN_FORCE_L-O", 1);
    gpio::set("SYS_RST_IN_L-O", 0);

    // Source: bmc_boot_banner() in bmc_ready.sh
    bmc_boot_banner();

    // Source: USB hub reset block in bmc_ready.sh (C2 specific:
    // B0_M1_USB1_HUB_RST_L-O)
    if (!std::filesystem::exists("/dev/ttyUSB0"))
    {
        std::cerr << "vr-c2-bmc: resetting USB1_HUB\n";
        gpio::set("B0_M1_USB1_HUB_RST_L-O", 0);
        gpio::set("B0_M1_USB1_HUB_RST_L-O", 1);
    }

    // Source: check_rw_filesystems() + check_rofs() in bmc_ready.sh
    emmc_partition("mmcblk0");
    filesystem_check();
    partition_check("mmcblk0");

    // Source: create_eeprom_devices.sh (power-on EEPROMs)
    std::cerr << "vr-c2-bmc: creating power-on EEPROM devices\n";
    eeprom_init(poweron_eeproms);

    gpio_check(gpio_checks);

    // Source: nvme_cpld_probe.sh (nvidia-nvme-cpld recipe)
    nvme_cpld_probe(14);
    nvme_cpld_probe(15);

    // Source: set_bmc_ready() in bmc_ready.sh
    gpio::set("BMC_READY-O", 1);
    gpio::set("GLOBAL_WP_BMC-O", 0);

    sd_notify(0, "READY=1");
    std::cerr << "vr-c2-bmc: platform init complete\n";

    // Source: rtc-detection.service (After=bmc-boot-complete.service)
    static constexpr std::array<unsigned, 2> rtc_buses{{5, 19}};
    rtc_detect(rtc_buses);
    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
