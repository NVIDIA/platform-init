// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for gb200nvl-hmc.
// Replaces hmc_ready.sh from hmc-post-boot-cfg.
// GPIO line names sourced from nvidia-gb200nvl-hmc-core.dtsi.
//
// Sequence:
//   1. Bind IOX 1-0024 (pca9555)
//   2. Sync HMC FRU EEPROM to /etc/fru/baseboard.fru.bin
//   3. Detect secondary modules (write MODULE_PRSNT_ARR + N_MODULES)
//   4. FPGA power sequence (CPLD reset if needed, release FPGA_RST_L-O)
//   5. Wait for FPGA0_READY-I and FPGA1_READY-I
//   6. Enable FPGA backdrive feature
//   7. 10 s ERoT delay
//   8. Filesystem check
//   9. Assert HMC_READY-O via LED sysfs

#include "platform_utils.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace nvidia
{

// Source: hmc_fru_checker.sh
// Binds the HMC FRU EEPROM at bus 3 addr 0x57 and reads 512 bytes
// into /etc/fru/baseboard.fru.bin so FruDevice can scan it.
// Non-fatal: logs failures and continues.
static void hmc_fru_check()
{
    constexpr const char* eeprom_sysfs =
        "/sys/bus/i2c/devices/i2c-3/3-0057/eeprom";
    constexpr const char* fru_dir = "/etc/fru";
    constexpr const char* fru_file = "/etc/fru/baseboard.fru.bin";

    std::error_code ec;
    std::filesystem::create_directories(fru_dir, ec);
    if (ec)
    {
        std::cerr << std::format("gb200nvl-hmc: fru_check: mkdir {}: {}\n",
                                 fru_dir, ec.message());
        return;
    }

    // Bind the EEPROM driver if not already bound.
    if (!std::filesystem::exists(eeprom_sysfs))
    {
        std::cerr << "gb200nvl-hmc: fru_check: binding 24c02 at 3-0057\n";
        i2c::new_device(3, 0x57, "24c02");
        std::this_thread::sleep_for(100ms);
    }

    // Read 512 bytes with up to 10 retries.
    constexpr int max_retries = 10;
    std::vector<char> buf(512);
    bool ok = false;
    for (int i = 0; i < max_retries; ++i)
    {
        std::ifstream f{eeprom_sysfs, std::ios::binary};
        if (f && f.read(buf.data(), 512) && f.gcount() == 512)
        {
            ok = true;
            break;
        }
        std::this_thread::sleep_for(100ms);
    }

    if (!ok)
    {
        std::cerr << "gb200nvl-hmc: fru_check: could not read EEPROM\n";
        // Create an empty file so FruDevice can start without blocking.
        std::ofstream{fru_file};
        return;
    }

    // Compare and write only if different.
    std::vector<char> existing(512, '\0');
    {
        std::ifstream cur{fru_file, std::ios::binary};
        if (cur)
            cur.read(existing.data(), 512);
    }
    if (buf != existing)
    {
        if (std::ofstream out{fru_file, std::ios::binary}; out)
        {
            out.write(buf.data(), 512);
            std::cerr << "gb200nvl-hmc: fru_check: updated baseboard.fru.bin\n";
        }
    }
    else
    {
        std::cerr << "gb200nvl-hmc: fru_check: FRU up to date\n";
    }
}

// Source: multi_module_detection.sh discover_modules()
// Reads GPIO presence pins for each secondary module (active-low: 0 = present).
// A module is present when both its B2B cable and C-Link cable are asserted.
// Writes space-separated present indices to MODULE_PRSNT_FILE and count to
// MODULE_COUNT_FILE so that downstream scripts (fpga1_ready.sh, etc.) can
// determine the module topology without re-probing GPIOs.
static void discover_modules()
{
    // One entry per possible secondary module; extend both arrays together.
    static constexpr std::array<const char*, 1> b2b_gpios{
        {"MOD1_B2B_CABLE_PRESENT_L-I"}};
    static constexpr std::array<const char*, 1> clink_gpios{
        {"MOD1_CLINK_CABLE_PRESENT_L-I"}};
    static constexpr const char* prsnt_file = "/etc/default/MODULE_PRSNT_ARR";
    static constexpr const char* count_file = "/etc/default/N_MODULES";

    std::vector<int> present;
    for (std::size_t i = 0; i < b2b_gpios.size(); ++i)
    {
        int b2b = gpio::get(b2b_gpios[i]);
        int clink = gpio::get(clink_gpios[i]);
        if (b2b < 0 || clink < 0)
        {
            std::cerr << std::format(
                "gb200nvl-hmc: discover_modules: cannot read module {} presence\n",
                i + 1);
            continue;
        }
        if ((b2b | clink) == 0)
        {
            present.push_back(static_cast<int>(i));
            std::cerr << std::format("gb200nvl-hmc: module {} connected\n",
                                     i + 1);
        }
        else
        {
            std::cerr << std::format("gb200nvl-hmc: module {} not connected\n",
                                     i + 1);
        }
    }

    if (std::ofstream f{prsnt_file}; f)
    {
        for (std::size_t i = 0; i < present.size(); ++i)
        {
            if (i)
                f << ' ';
            f << present[i];
        }
        f << '\n';
    }
    if (std::ofstream f{count_file}; f)
        f << present.size() << '\n';

    std::cerr << std::format("gb200nvl-hmc: {} secondary module(s) present\n",
                             present.size());
}

// Source: fpga_power_sequence.sh execute_hmc_cpld_reset()
// The CPLD reset GPIO is unnamed in the DTS (gpiochip0 line 123 = P3).
// Only executed if FPGA0 is not yet ready at init time.
static void hmc_cpld_reset_if_needed()
{
    if (gpio::get("FPGA0_READY-I") == 0)
    {
        std::cerr << "gb200nvl-hmc: FPGA not ready — executing CPLD reset\n";
        // gpiochip0 line 123 = GPIO_P3 (unnamed in DTS)
        gpio::set_raw(0, 123, 0);
        std::this_thread::sleep_for(1s);
        gpio::set_raw(0, 123, 1);
    }
}

int init_gb200nvl_hmc()
{
    std::cerr << "gb200nvl-hmc: platform init start\n";

    // Source: bind_expanders.sh
    // Bind IO expander 1-0024 (pca9555 on I2C2 / bus 1).
    iox_bind(1, 0x24);

    // Source: hmc_fru_checker.sh
    hmc_fru_check();

    // Source: multi_module_detection.sh
    discover_modules();

    // Source: fpga_power_sequence.sh
    // Perform CPLD reset if FPGA0 is not ready, then arm edge events
    // before releasing FPGA from reset to avoid a race where the FPGA
    // asserts ready before we arm the event listener.
    hmc_cpld_reset_if_needed();

    gpio::Event fpga0_ready("FPGA0_READY-I", 1);
    gpio::Event fpga1_ready("FPGA1_READY-I", 1);

    std::cerr << "gb200nvl-hmc: releasing FPGA from reset\n";
    gpio::set("FPGA_RST_L-O", 1);

    // Source: hmc_ready.sh — enable FPGA backdrive feature.
    // Added in FPGA version 1v44; ignore errors on older builds.
    {
        try
        {
            i2c::RawDevice dev{1, 0x11};
            dev.write_byte(0x13, 0x01);
            std::cerr << "gb200nvl-hmc: FPGA backdrive feature enabled\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format("gb200nvl-hmc: backdrive: {} (ignored)\n",
                                     e.what());
        }
    }

    // Wait for primary FPGA (20 s).
    std::cerr << "gb200nvl-hmc: waiting for FPGA0_READY-I\n";
    if (fpga0_ready.wait() != gpio::EventResult::Asserted)
        std::cerr << "gb200nvl-hmc: WARNING: FPGA0_READY-I timeout\n";
    else
        std::cerr << "gb200nvl-hmc: FPGA0 ready\n";

    // Wait for secondary FPGA (20 s).
    std::cerr << "gb200nvl-hmc: waiting for FPGA1_READY-I\n";
    if (fpga1_ready.wait() != gpio::EventResult::Asserted)
        std::cerr << "gb200nvl-hmc: WARNING: FPGA1_READY-I timeout\n";
    else
        std::cerr << "gb200nvl-hmc: FPGA1 ready\n";

    // Source: hmc_ready.sh — delay to allow ERoT to complete background
    // copy after a firmware update before I2C auth commands are sent.
    std::cerr << "gb200nvl-hmc: 10 s ERoT reset delay\n";
    std::this_thread::sleep_for(10s);

    // Source: check_rw_filesystems / check_rofs in hmc_ready.sh
    if (filesystem_check() != EXIT_SUCCESS)
    {
        std::cerr << "gb200nvl-hmc: FATAL: filesystem check failure\n";
        return EXIT_FAILURE;
    }

    // Assert HMC_READY-O via LED sysfs (gpio-leds driver owns M5).
    if (std::ofstream f{"/sys/class/leds/hmc_ready_noled/brightness"}; f)
        f << "1\n";
    else
        std::cerr << "gb200nvl-hmc: WARNING: failed to assert HMC_READY-O\n";

    sd_notify(0, "READY=1");
    std::cerr << "gb200nvl-hmc: platform init complete\n";

    gpio::release_all();

    pause();
    return EXIT_SUCCESS;
}

} // namespace nvidia
