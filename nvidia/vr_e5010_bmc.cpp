// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-vr-e5010-bmc.
// Replaces bmc_ready.sh. E5010 has no PDB; board model is B0_M0/B0_M1.

#include "platform_utils.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <array>
#include <iostream>

namespace nvidia
{

// ── Platform config ───────────────────────────────────────────────────────
// GPIO line names sourced from nvidia-vr-e5010-bmc-core.dtsi

// Source: gpio_check — GPIOs asserted by this init sequence, verified after EEPROM init
static constexpr std::array<GpioLevel, 5> gpio_checks{{
    {"FPGA_READY_BMC-I",             1},
    {"BMC_READY-O",                  1},
    {"STBY_POWER_EN-O",              1},
    {"BMC_EROT_FPGA_SPI_MUX_SEL-O", 1},
    {"EROT_FPGA_RST_L-O",            1},
}};

// Source: create_eeprom_devices.sh (E5010 variant, no PDB)
static constexpr std::array<EepromDevice, 5> poweron_eeproms{{
    {30, 0x50, "24c128", "BF3 CEM FRU (P5035 A02)", true},
    {34, 0x50, "24c128", "BF3 CEM FRU (P5035 A01)", true},
    {36, 0x51, "24c02",  "E1.S Backplane FRU",       true},
    {42, 0x50, "24c128", "GPU 0 CEM FRU",            true},
    {46, 0x50, "24c128", "CX8 0 CEM FRU",            true},
}};


// ── Platform init ─────────────────────────────────────────────────────────

int init_vr_e5010_bmc()
{
    std::cerr << "vr-e5010-bmc: platform init start\n";

    // Source: GPIO Expander PERST highZ Sequence block in bmc_ready.sh
    // i2cset -fy 2 0x25 0x02 0x0000 w  → write_word(0x02, 0x0000) output regs LOW
    // i2cset -fy 2 0x25 0x06 0x0000 w  → write_word(0x06, 0x0000) config regs OUTPUT
    // i2cset -fy 2 0x26 0x02 0x0000 w  → write_word(0x02, 0x0000) output regs LOW
    // i2cset -fy 2 0x26 0x06 0x0000 w  → write_word(0x06, 0x0000) config regs OUTPUT
    std::cerr << "vr-e5010-bmc: PERST highZ sequence\n";
    for (uint8_t dev_addr : {uint8_t{0x25}, uint8_t{0x26}})
    {
        try
        {
            i2c::RawDevice dev{2, dev_addr};
            if (int rc = dev.write_word(0x02, 0x0000); rc != 0)
                std::cerr << std::format(
                    "vr-e5010-bmc: PERST write_word(0x02) bus=2 addr={:#x}: {}\n",
                    dev_addr, std::strerror(rc));
            if (int rc = dev.write_word(0x06, 0x0000); rc != 0)
                std::cerr << std::format(
                    "vr-e5010-bmc: PERST write_word(0x06) bus=2 addr={:#x}: {}\n",
                    dev_addr, std::strerror(rc));
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format(
                "vr-e5010-bmc: PERST device bus=2 addr={:#x} not accessible: {}\n",
                dev_addr, e.what());
        }
    }

    // Source: bmc_set_initial_gpio_out() in bmc_ready.sh, called when
    //         B0_M0_STBY_POWER_PG-I == 0.
    if (gpio::get("B0_M0_STBY_POWER_PG-I") == 0)
    {
        // Source: bmc_set_initial_gpio_out() in bmc_ready.sh
        gpio::set("GLOBAL_WP_BMC-O", 0);
        gpio::set("HMC_RESET_L-O",   0);
    }

    gpio::Event stby_pg("STBY_POWER_PG-I", 1);

    // Source: wait_standby_pwr_p4103() in bmc_ready.sh
    std::cerr << "vr-e5010-bmc: waiting for STBY_POWER_PG-I\n";
    if (stby_pg.wait() != gpio::EventResult::Asserted)
    {
        std::cerr << "vr-e5010-bmc: FATAL: STBY_POWER_PG-I timeout\n";
        return EXIT_FAILURE;
    }

    // Source: check_iox_2_20_present() in bmc_ready.sh, exit 1 on failure
    std::cerr << "vr-e5010-bmc: waiting for IOX 2-0020\n";
    if (!iox_wait(2, 0x20, std::chrono::seconds{30}))
    {
        std::cerr << "vr-e5010-bmc: FATAL: IOX 2-0020 not present\n";
        return EXIT_FAILURE;
    }

    // Source: hmc_bypass() in bmc_ready.sh
    hmc_mux_select();

    // Source: bind_gpio_expanders() in bmc_ready.sh (E5010 variant: 10 expanders)
    // 1-0020, 1-0021: HMC bypass
    iox_bind(1, 0x20);
    iox_bind(1, 0x21);
    // 2-0020 … 2-0024: B0_M0 MGMT board
    iox_bind(2, 0x20);
    iox_bind(2, 0x21);
    iox_bind(2, 0x22);
    iox_bind(2, 0x23);
    iox_bind(2, 0x24);
    // 7-0020 … 7-0024: B1_M0 MGMT board (E5010 has these in bind_gpio_expanders)
    iox_bind(7, 0x20);
    iox_bind(7, 0x21);
    iox_bind(7, 0x22);
    iox_bind(7, 0x23);
    iox_bind(7, 0x24);

    // Source: wait_standby_pwr "B0_M0" in bmc_ready.sh; 200 × 100ms = 20s
    //         E5010 wait_standby_pwr maps "B0_M0" → B0_M0_PS_1V8_STBY_PGOOD-I
    std::cerr << "vr-e5010-bmc: waiting for B0_M0_PS_1V8_STBY_PGOOD-I\n";
    if (!wait_gpio_high("B0_M0_PS_1V8_STBY_PGOOD-I", 200, std::chrono::milliseconds{100}))
    {
        std::cerr << "vr-e5010-bmc: FATAL: B0_M0_PS_1V8_STBY_PGOOD-I timeout\n";
        return EXIT_FAILURE;
    }

    // Source: `set_gpio_level "B0_M0_PWR_BRAKE_IOX_L-O" $HIGH` in bmc_ready.sh
    gpio::set("B0_M0_PWR_BRAKE_IOX_L-O", 1);

    // Source: `set_gpio_level "B0_M0_C0_SOCAMM_I2C_SEL-O" $HIGH` in bmc_ready.sh
    gpio::set("B0_M0_C0_SOCAMM_I2C_SEL-O", 1);

    // Source: `set_gpio_level "B0_M0_BMC_SHDN_FORCE_L-O" $HIGH` in bmc_ready.sh
    gpio::set("B0_M0_BMC_SHDN_FORCE_L-O", 1);

    // Source: bmc_set_stby_pg_gpio_out() in bmc_ready.sh
    gpio::set("PWR_BRAKE_L-O",  1);
    gpio::set("SHDN_REQ_L-O",   1);
    gpio::set("SHDN_FORCE_L-O", 1);
    gpio::set("SYS_RST_IN_L-O", 0);

    // Source: bmc_boot_banner() in bmc_ready.sh
    bmc_boot_banner();

    // Source: check_rw_filesystems() + check_rofs() in bmc_ready.sh
    // Note: E5010 has no USB hub reset step.
    emmc_partition("mmcblk0");
    filesystem_check();
    partition_check("mmcblk0");

    // Source: create_eeprom_devices.sh (power-on EEPROMs)
    std::cerr << "vr-e5010-bmc: creating power-on EEPROM devices\n";
    eeprom_init(poweron_eeproms);

    gpio_check(gpio_checks);

    // Source: set_bmc_ready() in bmc_ready.sh
    gpio::set("BMC_READY-O",     1);
    gpio::set("GLOBAL_WP_BMC-O", 0);

    sd_notify(0, "READY=1");
    std::cerr << "vr-e5010-bmc: platform init complete\n";

    // Source: rtc-detection.service (After=bmc-boot-complete.service)
    static constexpr std::array<unsigned, 2> rtc_buses{{5, 19}};
    rtc_detect(rtc_buses);
    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
