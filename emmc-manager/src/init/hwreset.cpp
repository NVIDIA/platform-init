// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-init.h"
#include "emmc-ioctl.h"

#include "emmc-ext-csd.hpp"

#include <gpiod.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <string_view>
#include <thread>

namespace emmc::init
{

namespace
{

// Per-board dts naming: AST2700-a1 uses "EMMC_RST_L-O"; later boards
// renamed to "EMMC_RST-O". First match wins.
constexpr std::array<const char*, 2> kEmmcResetGpioNames = {
    "EMMC_RST_L-O",
    "EMMC_RST-O",
};

// gpiod_line_find returns a borrowed pointer to a line whose owning chip
// has been opened as a side effect; callers must close that chip via
// gpiod_chip_close(gpiod_line_get_chip(line)) to release the handle.
gpiod_line* findResetLine(const char*& found_name)
{
    for (const char* name : kEmmcResetGpioNames)
    {
        gpiod_line* line = gpiod_line_find(name);
        if (line != nullptr)
        {
            found_name = name;
            return line;
        }
    }
    return nullptr;
}

} // namespace

// Chip side: program EXT_CSD byte 162 so the chip honors RST_n pulses.
//
//   0x00 default — chip ignores RST_n entirely
//   0x01         — permanently enabled (one-shot write)
//   0x02         — permanently disabled (cannot be undone)
//
// Must be called BEFORE unbinding the sdhci-aspeed driver: CMD6 SWITCH
// requires the kernel mmc stack to issue ioctls against /dev/mmcblk0.
bool ensureHwResetEnabled(std::string_view device)
{
    std::array<uint8_t, emmc::extcsd::kExtCsdSizeBytes> ext_csd{};
    if (!ioctl::readExtCsd(device, ext_csd))
    {
        std::cerr << "hwreset: readExtCsd failed; cannot check "
                     "RST_n_FUNCTION\n";
        return false;
    }

    const uint8_t current = ext_csd[emmc::extcsd::kRst_n_Function];
    if (current == emmc::extcsd::kRstNFunctionEnabled)
    {
        return true; // already programmed; idempotent fast path
    }
    if (current == emmc::extcsd::kRstNFunctionPermDisabled)
    {
        std::cerr << "hwreset: RST_n_FUNCTION permanently disabled (0x02); "
                     "chip will not honor RST_n pulses\n";
        return false;
    }
    if (current != emmc::extcsd::kRstNFunctionDisabledDefault)
    {
        std::cerr << std::format(
            "hwreset: RST_n_FUNCTION holds unexpected value 0x{:02x}; "
            "not attempting to program\n",
            current);
        return false;
    }

    std::cerr << "hwreset: RST_n_FUNCTION=0x00 (default); programming "
                 "permanent enable (0x01)\n";
    if (!ioctl::writeExtCsdByte(device, emmc::extcsd::kRst_n_Function,
                                emmc::extcsd::kRstNFunctionEnabled))
    {
        std::cerr << "hwreset: writeExtCsdByte for RST_n_FUNCTION failed\n";
        return false;
    }

    // Read back to confirm the one-shot write actually latched.
    if (!ioctl::readExtCsd(device, ext_csd))
    {
        std::cerr << "hwreset: readback of EXT_CSD failed after "
                     "RST_n_FUNCTION write\n";
        return false;
    }
    const uint8_t after = ext_csd[emmc::extcsd::kRst_n_Function];
    if (after != emmc::extcsd::kRstNFunctionEnabled)
    {
        std::cerr << std::format(
            "hwreset: RST_n_FUNCTION write reported success but readback is "
            "0x{:02x} (expected 0x01)\n",
            after);
        return false;
    }
    std::cerr << "hwreset: RST_n_FUNCTION now permanently enabled (0x01)\n";
    return true;
}

// Board side: drive the EMMC_RST_n GPIO low for 100 ms then release.
// Effective only if ensureHwResetEnabled() succeeded earlier — otherwise
// the chip silently ignores the pulse.
//
// The kernel sdhci-aspeed driver should be unbound before calling this
// so it does not race the chip re-init.
bool pulseHwResetGpio()
{
    using namespace std::chrono_literals;
    const char* found_name = nullptr;
    gpiod_line* line = findResetLine(found_name);
    if (line == nullptr)
    {
        std::cerr << "hwreset: GPIO line not found (tried";
        for (const char* name : kEmmcResetGpioNames)
        {
            std::cerr << ' ' << name;
        }
        std::cerr << ")\n";
        return false;
    }
    gpiod_chip* chip = gpiod_line_get_chip(line);
    bool ok = true;
    if (gpiod_line_request_output(line, "nvidia-emmc-init", 0) < 0)
    {
        std::cerr << std::format(
            "hwreset: gpiod_line_request_output({}) failed: {}\n", found_name,
            std::strerror(errno));
        ok = false;
    }
    else
    {
        std::this_thread::sleep_for(100ms);
        if (gpiod_line_set_value(line, 1) < 0)
        {
            std::cerr << std::format(
                "hwreset: gpiod_line_set_value({}, 1) failed: {}\n", found_name,
                std::strerror(errno));
            ok = false;
        }
        else
        {
            std::this_thread::sleep_for(100ms);
        }
        gpiod_line_release(line);
    }
    if (chip != nullptr)
    {
        gpiod_chip_close(chip);
    }
    if (ok)
    {
        std::cerr << std::format("hwreset: toggled GPIO line {}\n", found_name);
    }
    return ok;
}

} // namespace emmc::init
