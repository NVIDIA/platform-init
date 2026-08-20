// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-vr-nvl-hmc.
// Replaces hmc_ready.sh. HMC role: monitors and signals, does not
// directly control host power rails.

#include "platform_utils.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace nvidia
{

// ── Platform config ───────────────────────────────────────────────────────
// GPIO line names sourced from nvidia-vr-nvl-hmc-core.dtsi

static constexpr std::array<GpioLevel, 5> gpio_checks{{
    {"QSPI2_RST_L-O", 1},
    {"HUB_RST_L-O", 1},
    {"EMMC_RST_L-O", 1},
    {"BRD0_HMC_TO_MCU_I2C_EN-O", 1},
    {"BRD1_HMC_TO_MCU_I2C_EN-O", 1},
}};

// Source: create_eeprom_devices.sh (HMC variant)
static constexpr std::array<EepromDevice, 2> boot_eeproms{{
    {2, 0x57, "24c02", "HMC FRU", false},
    {2, 0x50, "24c64", "Module 0 FRU", false},
}};

// Source: hw_checkout.sh EID definitions
static constexpr std::array<ErotEndpoint, 7> erot_endpoints{{
    {9, "HGX_ERoT_BMC_0"},
    {11, "HGX_IRoT_GPU_0"},
    {12, "HGX_IRoT_GPU_1"},
    {14, "HGX_IRoT_CPU_0"},
    {21, "HGX_IRoT_GPU_2"},
    {22, "HGX_IRoT_GPU_3"},
    {24, "HGX_IRoT_CPU_1"},
}};

// ── Platform init ─────────────────────────────────────────────────────────

int init_vr_nvl_hmc()
{
    std::cerr << "vr-nvl-hmc: platform init start\n";

    // BRD0 IO expanders (bus 1, 0x20/0x21) are powered by standby power and
    // disabled in DT. Poll until they appear, then bind the pca953x driver.
    // BRD1 (bus 7) is optional — single-board configs won't have it.
    std::cerr << "vr-nvl-hmc: waiting for BRD0 standby power\n";
    if (iox_wait(1, 0x20, std::chrono::seconds{120}))
    {
        std::cerr
            << "vr-nvl-hmc: BRD0 standby power up, binding IO expanders\n";
        iox_bind(1, 0x20);
        iox_bind(1, 0x21);
        if (iox_wait(7, 0x20, std::chrono::seconds{10}))
        {
            iox_bind(7, 0x20);
            iox_bind(7, 0x21);
        }
        else
            std::cerr << "vr-nvl-hmc: BRD1 not present (single-board)\n";
    }
    else
        std::cerr << "vr-nvl-hmc: BRD0 standby power timeout — continuing\n";

    // Source: hmc_set_initial_gpio_out() in hmc_ready.sh
    std::cerr << "vr-nvl-hmc: initializing HMC GPIO output states\n";
    gpio::set("QSPI2_RST_L-O", 1);
    gpio::set("BMC_DDR4_TEN-O", 0);
    gpio::set("HUB_RST_L-O", 1);
    gpio::set("EMMC_RST_L-O", 1);

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
    std::cerr << "vr-nvl-hmc: taking Board 0 HPM MCU out of reset\n";
    gpio::set("BRD0_HMC_TO_MCU_I2C_EN-O", 1);

    // Source: `gpioset BRD1_HMC_TO_MCU_I2C_EN-O=1` in hmc_ready_sequence()
    //         in hmc_ready.sh
    std::cerr << "vr-nvl-hmc: taking Board 1 HPM MCU out of reset\n";
    gpio::set("BRD1_HMC_TO_MCU_I2C_EN-O", 1);

    // Source: hmc_fru_checker.sh (logic migrated to native C++; script
    // eliminated) Instantiate baseboard FRU EEPROM on bus 10 addr 0x50 (24c02)
    // if absent. Then read up to 512 bytes and sync to
    // /etc/fru/baseboard.fru.bin so that FruDevice.service can discover HMC FRU
    // data without parsing the raw EEPROM. Non-fatal: on read failure an empty
    // file is created so FruDevice can start.
    std::cerr << "vr-nvl-hmc: syncing HMC baseboard FRU\n";
    {
        static constexpr std::array<EepromDevice, 1> fru_eeprom{
            {{10, 0x50, "24c02", "HMC Baseboard FRU", false}}};
        eeprom_init(fru_eeprom);

        static constexpr auto eeprom_path =
            "/sys/bus/i2c/devices/10-0050/eeprom";
        static constexpr auto fru_dir = "/etc/fru";
        static constexpr auto fru_file = "/etc/fru/baseboard.fru.bin";
        static constexpr std::size_t fru_size = 512;
        static constexpr int max_retries = 10;

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
                std::cerr << std::format(
                    "vr-nvl-hmc: FRU EEPROM read retry {}/{}\n", i + 1,
                    max_retries);
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }

        if (!read_ok)
        {
            std::cerr
                << "vr-nvl-hmc: FRU EEPROM read failed — creating empty FRU file\n";
            std::ofstream{fru_file}; // empty file lets FruDevice start
        }
        else
        {
            bool needs_update = true;
            if (std::ifstream existing{fru_file, std::ios::binary}; existing)
            {
                std::vector<char> prev(fru_size, 0);
                existing.read(prev.data(),
                              static_cast<std::streamsize>(fru_size));
                if (prev == buf)
                    needs_update = false;
            }
            if (needs_update)
            {
                if (std::ofstream out{fru_file, std::ios::binary}; out)
                    out.write(buf.data(),
                              static_cast<std::streamsize>(fru_size));
                else
                    std::cerr << "vr-nvl-hmc: failed to write FRU file\n";
            }
        }
    }

    // Source: `sleep $erot_reset_delay` (erot_reset_delay=10) in hmc_ready.sh
    // Allows ERoT to complete background FW copy and reset before I2C access.
    std::cerr << "vr-nvl-hmc: sleeping 10s for ERoT reset delay\n";
    std::this_thread::sleep_for(std::chrono::seconds{10});

    // Source: check_rw_filesystems() + check_rofs() in hmc_ready_sequence()
    //         in hmc_ready.sh
    if (filesystem_check() != EXIT_SUCCESS)
    {
        std::cerr << "vr-nvl-hmc: FATAL: filesystem check failure\n";
        return EXIT_FAILURE;
    }

    // Source: nvidia-emmc-partition.service (create-partition.sh +
    //         emmc-partition-format.sh). Layout identical to
    //         vr_bmc_emmc_layout.
    // Replaces create-partition.sh and repartition_check.sh in the bbappend.
    emmc_partition("mmcblk0");
    emmc_format_and_mount("mmcblk0");

    // Source: create_eeprom_devices.sh (HMC boot EEPROMs, consolidated from
    //         separate service)
    std::cerr << "vr-nvl-hmc: creating boot-time EEPROM devices\n";
    eeprom_init(boot_eeproms);

    gpio_check(gpio_checks);

    // Source: set_hmc_ready() in hmc_ready.sh
    // HMC_READY-O (GPIO M3) is claimed by the gpio-leds driver; libgpiod
    // cannot claim it — drive via LED sysfs instead.
    if (std::ofstream f{"/sys/class/leds/hmc_ready_noled/brightness"}; f)
        f << "1\n";
    else
        std::cerr << "vr-nvl-hmc: WARNING: failed to assert HMC_READY-O\n";

    sd_notify(0, "READY=1");
    std::cerr << "vr-nvl-hmc: platform init complete\n";

    // Source: rtc-sync.sh (vr-nvl-hmc initramfs). External RTC on bus 9 @ 0x6f
    // is NCT3018Y (0x5c) or NXP PCF85053A (0x4e); rtc_detect probes the vendor
    // ID and binds the matching driver. phosphor-time-manager sets the clock.
    static constexpr std::array<unsigned, 1> rtc_buses{{9}};
    rtc_detect(rtc_buses);

    // Source: bmc_reset_check.sh / bmc-reset-check.service
    bmc_reset_check();

    // Source: hw_checkout.sh invoked after hmc-boot-complete.service
    hw_checkout(erot_endpoints);
    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
