// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for gb200nvl-hmc.

#include "compute_hmc_common.hpp"
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

    if (!std::filesystem::exists(eeprom_sysfs))
    {
        std::cerr << "gb200nvl-hmc: fru_check: binding 24c02 at 3-0057\n";
        i2c::new_device(3, 0x57, "24c02");
        std::this_thread::sleep_for(100ms);
    }

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

static void hmc_cpld_reset_if_needed()
{
    if (gpio::get("FPGA0_READY-I") == 0)
    {
        std::cerr << "gb200nvl-hmc: FPGA not ready — executing CPLD reset\n";
        // CPLD reset is GPIO_P3, unnamed in the DTS.
        gpio::set_raw(0, 123, 0);
        std::this_thread::sleep_for(1s);
        gpio::set_raw(0, 123, 1);
    }
}

int init_gb200nvl_hmc()
{
    std::cerr << "gb200nvl-hmc: platform init start\n";

    iox_bind(1, 0x24);

    hmc_fru_check();

    discover_modules();

    hmc_cpld_reset_if_needed();

    // Armed before FPGA_RST_L-O is released so the edge is not missed.
    gpio::Event fpga0_ready("FPGA0_READY-I", 1);
    gpio::Event fpga1_ready("FPGA1_READY-I", 1);

    std::cerr << "gb200nvl-hmc: releasing FPGA from reset\n";
    gpio::set("FPGA_RST_L-O", 1);

    // FPGA backdrive; needs FPGA 1v44+, so errors are ignored.
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

    std::cerr << "gb200nvl-hmc: waiting for FPGA0_READY-I\n";
    if (fpga0_ready.wait() != gpio::EventResult::Asserted)
        std::cerr << "gb200nvl-hmc: WARNING: FPGA0_READY-I timeout\n";
    else
        std::cerr << "gb200nvl-hmc: FPGA0 ready\n";

    std::cerr << "gb200nvl-hmc: waiting for FPGA1_READY-I\n";
    if (fpga1_ready.wait() != gpio::EventResult::Asserted)
        std::cerr << "gb200nvl-hmc: WARNING: FPGA1_READY-I timeout\n";
    else
        std::cerr << "gb200nvl-hmc: FPGA1 ready\n";

    // Let ERoT finish its post-update background copy before I2C auth.
    std::cerr << "gb200nvl-hmc: 10 s ERoT reset delay\n";
    std::this_thread::sleep_for(10s);

    if (filesystem_check() != EXIT_SUCCESS)
    {
        std::cerr << "gb200nvl-hmc: FATAL: filesystem check failure\n";
        return EXIT_FAILURE;
    }

    // gpio-leds owns HMC_READY-O.
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
