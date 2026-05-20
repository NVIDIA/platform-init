// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-vr-nvl-bmc.
// Replaces bmc_ready.sh. Supports NVL POR PDB and GB300 PDB variants,
// detected at runtime via I2C sysfs path presence.

#include "platform_utils.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <array>
#include <filesystem>
#include <iostream>

namespace nvidia
{

// ── Platform config ───────────────────────────────────────────────────────
// GPIO line names sourced from nvidia-vr-nvl-bmc-core.dtsi

// Source: create_eeprom_devices.sh (NVL-BMC variant)
static constexpr std::array<EepromDevice, 14> boot_eeproms{{
    {1,  0x54, "24c02",  "CBC 0 FRU",              false},
    {1,  0x55, "24c02",  "CBC 1 FRU",              false},
    {2,  0x57, "24c02",  "HMC FRU",                false},
    {2,  0x50, "24c64",  "Module 0 FRU",           false},
    {2,  0x54, "24c02",  "CBC 2 FRU",              false},
    {2,  0x55, "24c02",  "CBC 3 FRU",              false},
    {4,  0x50, "24c64",  "Module 0 Aux EEPROM",    false},
    {9,  0x50, "24c64",  "Module 0 Aux (bus9)",    false},
    {9,  0x51, "24c64",  "Module 1 Aux (bus9)",    false},
    {14, 0x50, "24c128", "BF3 Left FRU",           false},
    {15, 0x50, "24c128", "BF3 Right FRU",          false},
    {21, 0x50, "24c64",  "Module 0 IO Board FRU",  false},
    {23, 0x57, "24c128", "FrontIO FRU",            false},
    {33, 0x50, "24c64",  "Module 1 IO Board FRU",  false},
}};

static constexpr std::array<EepromDevice, 9> poweron_eeproms{{
    {14, 0x55, "24c128", "IPEX Left FRU",           true},
    {14, 0x56, "24c128", "HDD BP Left FRU",         true},
    {15, 0x55, "24c128", "IPEX Right FRU",          true},
    {15, 0x56, "24c128", "HDD BP Right FRU",        true},
    {17, 0x56, "24c128", "CX8 E1S BP FRU (bus17)",  true},
    {21, 0x52, "24c128", "OSFP Board Left",         true},
    {29, 0x56, "24c128", "CX8 E1S BP FRU (bus29)",  true},
    {33, 0x52, "24c128", "OSFP Board Right",        true},
    {54, 0x51, "24c128", "1G NIC FRU",              true},
}};

// Source: eeprom-crc-verifier golden_eeprom.bin (bus=11 addr=0x50 type=24c128)
// Embedded here so the file is not needed at runtime.
static constexpr std::array<uint8_t, 158> golden_eeprom{{
    0x9e, 0x80, 0xca, 0x12, 0x01, 0x00, 0xca, 0x12, 0x01, 0x00, 0x8c, 0x08,
    0x00, 0x24, 0x00, 0x1b, 0xc0, 0x14, 0x03, 0x1b, 0x34, 0x02, 0x00, 0x12,
    0xc0, 0x7f, 0x03, 0x1d, 0x10, 0x01, 0x16, 0x1d, 0x69, 0x0a, 0x28, 0x27,
    0x00, 0x00, 0x68, 0x27, 0x00, 0x00, 0x38, 0x20, 0x98, 0x01, 0x64, 0x20,
    0x98, 0x01, 0x24, 0x20, 0xe1, 0x01, 0x64, 0x20, 0xe1, 0x01, 0x20, 0x20,
    0x40, 0x11, 0x60, 0x20, 0x40, 0x11, 0x38, 0x00, 0x00, 0x00, 0x78, 0x00,
    0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x3b, 0x20, 0x16, 0x80, 0x3c, 0x20,
    0x00, 0x0b, 0x7b, 0x20, 0x16, 0x80, 0x7c, 0x20, 0x00, 0x0b, 0x3b, 0x20,
    0xa7, 0x83, 0x3c, 0x20, 0x0c, 0x16, 0x7b, 0x20, 0xa7, 0x83, 0x7c, 0x20,
    0x0c, 0x16, 0x02, 0x66, 0x20, 0x0f, 0x01, 0x66, 0x01, 0x00, 0x00, 0x66,
    0xc0, 0x00, 0x02, 0x66, 0x16, 0x46, 0x01, 0x66, 0x0d, 0x00, 0x00, 0x66,
    0xc0, 0x00, 0x95, 0x1d, 0x00, 0x3f, 0x02, 0x66, 0xe0, 0x70, 0x01, 0x66,
    0x02, 0x00, 0x00, 0x66, 0xc0, 0x00, 0x95, 0x1d, 0x00, 0x22, 0x11, 0x1d,
    0x40, 0x17,
}};


// ── Platform init ─────────────────────────────────────────────────────────

int init_vr_nvl_bmc()
{
    std::cerr << "vr-nvl-bmc: platform init start\n";

    // Source: bmc_set_initial_gpio_out() in bmc_ready.sh, called when
    //         B0_M0_STBY_POWER_PG-I == 0.
    if (gpio::get("B0_M0_STBY_POWER_PG-I") == 0)
    {
        // Source: bmc_set_initial_gpio_out() in bmc_ready.sh
        gpio::set("GLOBAL_WP_BMC-O", 0);

        // Source: HSC mask-alert + clear-faults loop in bmc_set_initial_gpio_out()
        // HSC mask all alerts:  SMBus write-word 0xd8 <= 0xffff
        // HSC clear faults:      SMBus send-byte  0x03
        static constexpr uint8_t hsc_addrs[] = {0x10, 0x12, 0x14, 0x16};
        for (uint8_t addr : hsc_addrs)
        {
            try
            {
                i2c::RawDevice dev{9, addr};
                if (int rc = dev.write_word(0xd8, 0xffff); rc != 0)
                    std::cerr << std::format(
                        "vr-nvl-bmc: HSC mask alert failed bus=9 addr={:#x}: {}\n",
                        addr, std::strerror(rc));
                if (int rc = dev.send_byte(0x03); rc != 0)
                    std::cerr << std::format(
                        "vr-nvl-bmc: HSC clear faults failed bus=9 addr={:#x}: {}\n",
                        addr, std::strerror(rc));
            }
            catch (const std::exception& e)
            {
                std::cerr << std::format(
                    "vr-nvl-bmc: HSC bus=9 addr={:#x} not accessible: {}\n",
                    addr, e.what());
            }
        }

        // Source: bmc_set_initial_gpio_out() in bmc_ready.sh
        gpio::set("BF3_AOC_NCSI_PRSNT_L-I", 0);
        gpio::set("MUX_I2C_ESPI_SEL-O",      0);
        gpio::set("EEDO_LED2-O",              1);
    }

    // Source: PDB IOX bind block in bmc_ready.sh (main), line ~569
    iox_bind(9, 0x20);

    // Source: PDB IOX bind block in bmc_ready.sh (main), line ~580
    iox_bind(9, 0x75);

    // Source: `set_gpio_level "PDB_12V_EN_N_R-O" $HIGH; sleep 1` in bmc_ready.sh
    gpio::set("PDB_12V_EN_N_R-O", 1);
    std::this_thread::sleep_for(std::chrono::seconds{1});

    gpio::Event stby_pg("STBY_POWER_PG-I", 1);

    // Source: `if [ -d .../9-0020 ]; then set_gpio_level "STBY_POWER_EN-O" $HIGH`
    //         in bmc_ready.sh
    if (iox_present(9, 0x20))
    {
        std::cerr << "vr-nvl-bmc: PDB IOX 9-0020 present — asserting STBY_POWER_EN-O\n";
        gpio::set("STBY_POWER_EN-O", 1);
    }

    // Source: wait_standby_pwr "BMC" in bmc_ready.sh
    std::cerr << "vr-nvl-bmc: waiting for STBY_POWER_PG-I\n";
    if (stby_pg.wait() != gpio::EventResult::Asserted)
    {
        std::cerr << "vr-nvl-bmc: FATAL: STBY_POWER_PG-I timeout\n";
        return EXIT_FAILURE;
    }

    // Source: `set_gpio_level "HMC_RST_R_L-O" $HIGH` in bmc_ready.sh
    gpio::set("HMC_RST_R_L-O", 1);

    gpio::Event pdb_pg("STBY_PWR_OK-I", 1);

    // Source: `set_gpio_level "PDB_12V_EN_N_R-O" $LOW` in bmc_ready.sh
    gpio::set("PDB_12V_EN_N_R-O", 0);

    // Source: `if [ -d .../9-0020 ]; then wait_standby_pwr "PDB"` in bmc_ready.sh
    if (iox_present(9, 0x20))
    {
        std::cerr << "vr-nvl-bmc: waiting for PDB STBY_PWR_OK-I\n";
        if (pdb_pg.wait() != gpio::EventResult::Asserted)
        {
            std::cerr << "vr-nvl-bmc: FATAL: STBY_PWR_OK-I (PDB standby) timeout\n";
            return EXIT_FAILURE;
        }
    }

    // MOD1_B2B_CABLE_PRESENT_L-I and MOD1_CLINK_CABLE_PRESENT_L-I are on
    // PDB IOX (9-0020, now bound). Both are active-low: 0 = cable present.
    // B1_M0 is considered present if either cable is connected.
    // Source: multi_module_detection.sh MODULE_B2B_GPIO_ARR / MODULE_CLINK_GPIO_ARR
    const bool b1_present = (gpio::get("MOD1_B2B_CABLE_PRESENT_L-I")   == 0)
                          || (gpio::get("MOD1_CLINK_CABLE_PRESENT_L-I") == 0);
    std::cerr << std::format("vr-nvl-bmc: B1_M0 module {} detected\n",
                             b1_present ? "" : "NOT");

    // Source: hmc_bypass() in bmc_ready.sh
    hmc_mux_select();

    // Source: bind_gpio_expanders() in bmc_ready.sh
    // 1-0020, 1-0021: HMC bypass expanders
    iox_bind(1, 0x20);
    iox_bind(1, 0x21);
    // 2-0020 … 2-0024: B0_M0 MGMT board expanders
    // Bind before waiting: after an aux cycle the BMC kernel restarts fresh
    // and the device tree does not auto-probe these expanders, so iox_wait
    // alone would always time out.
    iox_bind(2, 0x20);
    iox_bind(2, 0x21);
    iox_bind(2, 0x22);
    iox_bind(2, 0x23);
    iox_bind(2, 0x24);
    // 7-0020 … 7-0024: B1_M0 MGMT board expanders
    if (b1_present)
    {
        iox_bind(7, 0x20);
        iox_bind(7, 0x21);
        iox_bind(7, 0x22);
        iox_bind(7, 0x23);
        iox_bind(7, 0x24);
    }

    // Source: check_iox_2_20_present() in bmc_ready.sh, exit 1 on failure
    std::cerr << "vr-nvl-bmc: waiting for IOX 2-0020\n";
    if (!iox_wait(2, 0x20, std::chrono::seconds{30}))
    {
        std::cerr << "vr-nvl-bmc: FATAL: IOX 2-0020 not present\n";
        return EXIT_FAILURE;
    }

    // Source: check_iox_7_20_present() in bmc_ready.sh, exit 1 on failure
    if (b1_present)
    {
        std::cerr << "vr-nvl-bmc: waiting for IOX 7-0020\n";
        if (!iox_wait(7, 0x20, std::chrono::seconds{30}))
        {
            std::cerr << "vr-nvl-bmc: FATAL: IOX 7-0020 not present\n";
            return EXIT_FAILURE;
        }
    }
    else
    {
        std::cerr << "vr-nvl-bmc: B1_M0 not present — skipping IOX 7-0020 wait\n";
    }

    // Source: `set_gpio_level "USB_MUX_EN-O" $HIGH` in bmc_ready.sh
    gpio::set("USB_MUX_EN-O", 1);

    // Source: `set_gpio_level "BMC_MUX_PI3DP_SEL-O" $LOW` in bmc_ready.sh
    gpio::set("BMC_MUX_PI3DP_SEL-O", 0);

    // bmc_set_stby_pg_gpio_out() in bmc_ready.sh set B0_M0_PWR_BRAKE_L-O,
    // B0_M0_SHDN_REQ_L-O, B0_M0_SHDN_FORCE_L-O, B0_M0_PRE_SYS_RST_L-O
    // using `gpioset -m exit` — set-and-release, never held.
    // x86-power-control owns these signals and sets identical safe defaults
    // ("Setting GPIOs to default state for host state OFF") on startup.
    // Platform-init must not touch them — holding them blocks x86-power-control.

    // Source: `if [ -d .../2-0020 ]; then wait_standby_pwr "B0_M0"` in bmc_ready.sh
    if (iox_present(2, 0x20))
    {
        std::cerr << "vr-nvl-bmc: waiting for B0_M0_STBY_POWER_PG-I\n";
        if (!wait_gpio_high("B0_M0_STBY_POWER_PG-I", 200, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc: FATAL: B0_M0_STBY_POWER_PG-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../7-0020 ]; then wait_standby_pwr "B1_M0"` in bmc_ready.sh
    if (iox_present(7, 0x20))
    {
        std::cerr << "vr-nvl-bmc: waiting for B1_M0_STBY_POWER_PG-I\n";
        if (!wait_gpio_high("B1_M0_STBY_POWER_PG-I", 200, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc: FATAL: B1_M0_STBY_POWER_PG-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../2-0020 ]; then wait_gpio_assert "B0_M0_HPM_MCU_OK-I" 100 0.1`
    //         in bmc_ready.sh
    if (iox_present(2, 0x20))
    {
        if (!wait_gpio_high("B0_M0_HPM_MCU_OK-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc: FATAL: B0_M0_HPM_MCU_OK-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../7-0020 ]; then wait_gpio_assert "B1_M0_HPM_MCU_OK-I" 100 0.1`
    //         in bmc_ready.sh
    if (iox_present(7, 0x20))
    {
        if (!wait_gpio_high("B1_M0_HPM_MCU_OK-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc: FATAL: B1_M0_HPM_MCU_OK-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../2-0020 ]; then wait_gpio_assert "B0_M0_CPLD_READY-I" 100 0.1`
    //         in bmc_ready.sh, exit 1 on failure
    if (iox_present(2, 0x20))
    {
        if (!wait_gpio_high("B0_M0_CPLD_READY-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc: FATAL: B0_M0_CPLD_READY-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: `if [ -d .../7-0020 ]; then wait_gpio_assert "B1_M0_CPLD_READY-I" 100 0.1`
    //         in bmc_ready.sh, exit 1 on failure
    if (iox_present(7, 0x20))
    {
        if (!wait_gpio_high("B1_M0_CPLD_READY-I", 100, std::chrono::milliseconds{100}))
        {
            std::cerr << "vr-nvl-bmc: FATAL: B1_M0_CPLD_READY-I timeout\n";
            return EXIT_FAILURE;
        }
    }

    // Source: bmc_boot_banner() in bmc_ready.sh
    bmc_boot_banner();

    // Source: USB_HUB reset workaround block in bmc_ready.sh
    // The USB_HUB needs to be reset once while AC ON.
    if (!std::filesystem::exists("/dev/ttyUSB0"))
    {
        std::cerr << "vr-nvl-bmc: resetting USB_HUB0 on board 0\n";
        gpio::set("B0_M0_USB_HUB0_RST_L-O", 0);
        gpio::set("B0_M0_USB_HUB0_RST_L-O", 1);
    }
    if (b1_present && !std::filesystem::exists("/dev/ttyUSB4"))
    {
        std::cerr << "vr-nvl-bmc: resetting USB_HUB0 on board 1\n";
        gpio::set("B1_M0_USB_HUB0_RST_L-O", 0);
        gpio::set("B1_M0_USB_HUB0_RST_L-O", 1);
    }

    // Source: check_rw_filesystems() / check_rofs() in bmc_ready.sh
    emmc_partition("mmcblk0");

    // Replaces emmc-partition-format.sh mount logic.
    // Idempotent: skips already-mounted partitions; formats only on first boot
    // when a partition has no ext4 superblock.  Mount table mirrors emmc-mount.conf
    // (now removed from the image — emmc_format_and_mount is the single source).
    emmc_format_and_mount("mmcblk0");

    // Source: check_rw_filesystems() + check_rofs() in bmc_ready.sh
    filesystem_check();

    // Source: create-partition.sh / repartition_check.sh equivalents
    partition_check("mmcblk0");

    // Source: create_eeprom_devices.sh (boot-time EEPROMs) in bmc_ready.sh
    std::cerr << "vr-nvl-bmc: creating boot-time EEPROM devices\n";
    eeprom_init(boot_eeproms);

    // Source: create_eeprom_devices.sh (power-on EEPROMs) in bmc_ready.sh
    std::cerr << "vr-nvl-bmc: creating power-on EEPROM devices\n";
    eeprom_init(poweron_eeproms);

    // Source: nvme_cpld_probe.sh (nvidia-nvme-cpld recipe)
    // i2c14 (0x77): E1.S slots 0-3; i2c15 (0x77): E1.S slots 4-7
    nvme_cpld_probe(14);
    nvme_cpld_probe(15);

    // Source: set_bmc_ready() in bmc_ready.sh + common_platform_var.conf
    // BMC_READY-O is bound to the kernel LED driver (bmc_ready_noled) for
    // reset-tolerant user-space control — it does NOT appear in gpioinfo.
    // Write brightness=1 to assert the GPIO via the LED sysfs interface.
    {
        const char* bmc_ready = "/sys/class/leds/bmc_ready_noled/brightness";
        if (std::ofstream f{bmc_ready}; f)
        {
            f << "1\n";
            std::cerr << "vr-nvl-bmc: BMC_READY-O asserted via LED sysfs\n";
        }
        else
            std::cerr << "vr-nvl-bmc: WARNING: BMC_READY-O LED sysfs not found\n";
    }
    // GLOBAL_WP_BMC-O renamed to GLOBAL_WP-O in this DTS revision (gpiochip2 line 3)
    gpio::set("GLOBAL_WP-O", 0);

    sd_notify(0, "READY=1");
    std::cerr << "vr-nvl-bmc: platform init complete\n";

    // platform-init must not hold any line beyond this point. Downstream
    // daemons (portctrl, x86-power-control, etc.) need to claim them.
    gpio::release_all();

    // Source: eeprom-crc-verifier.service (After=bmc-boot-complete.service)
    // Golden bytes embedded in golden_eeprom constexpr above — no file needed.
    eeprom_crc_verify(11, 0x50, "24c128", golden_eeprom);

    // Source: rtc-detection.service (After=bmc-boot-complete.service)
    static constexpr std::array<unsigned, 2> rtc_buses{{5, 19}};
    rtc_detect(rtc_buses);
    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
