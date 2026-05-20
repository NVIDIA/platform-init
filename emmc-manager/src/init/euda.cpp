// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-config.h"
#include "emmc-ext-csd.hpp"
#include "emmc-init.h"
#include "emmc-ioctl.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <format>
#include <iostream>
#include <string>
#include <thread>

namespace emmc::init
{

namespace
{

uint32_t read24Le(const std::array<uint8_t, 512>& ext_csd, unsigned int lsb)
{
    return static_cast<uint32_t>(ext_csd[lsb])
           | (static_cast<uint32_t>(ext_csd[lsb + 1]) << 8U)
           | (static_cast<uint32_t>(ext_csd[lsb + 2]) << 16U);
}

uint32_t read32Le(const std::array<uint8_t, 512>& ext_csd, unsigned int lsb)
{
    return static_cast<uint32_t>(ext_csd[lsb])
           | (static_cast<uint32_t>(ext_csd[lsb + 1]) << 8U)
           | (static_cast<uint32_t>(ext_csd[lsb + 2]) << 16U)
           | (static_cast<uint32_t>(ext_csd[lsb + 3]) << 24U);
}

// ENH_SIZE_MULT unit per JESD84-B51 §7.4.74:
//   unit = HC_WP_GRP_SIZE × HC_ERASE_GRP_SIZE × 524288
uint64_t enhSizeMultUnitBytes(const std::array<uint8_t, 512>& ext_csd)
{
    return static_cast<uint64_t>(ext_csd[emmc::extcsd::kHc_Wp_Grp_Size])
           * static_cast<uint64_t>(ext_csd[emmc::extcsd::kHc_Erase_Grp_Size])
           * emmc::extcsd::kEnhSizeMultBaseBytes;
}

bool sysfsWrite(const char* path, const char* value)
{
    std::ofstream ofs(path);
    if (!ofs)
    {
        std::cerr << std::format("euda: open({}) failed\n", path);
        return false;
    }
    ofs << value << '\n' << std::flush;
    if (!ofs.good())
    {
        std::cerr << std::format("euda: write({}) failed\n", path);
        return false;
    }
    return true;
}

bool writeEnhSizeMult(std::string_view device, uint32_t target)
{
    return ioctl::writeExtCsdByte(device, emmc::extcsd::kEnh_Size_Mult_Lsb,
                                  target & 0xFFU)
           && ioctl::writeExtCsdByte(device, emmc::extcsd::kEnh_Size_Mult_Mid,
                                     (target >> 8U) & 0xFFU)
           && ioctl::writeExtCsdByte(device, emmc::extcsd::kEnh_Size_Mult_Msb,
                                     (target >> 16U) & 0xFFU);
}

} // namespace

void unbindSdhciAspeed()
{
    using namespace std::chrono_literals;
    sysfsWrite("/sys/bus/platform/drivers/sdhci-aspeed/unbind",
               emmc::layout::kSdhciControllerName);
    std::this_thread::sleep_for(1s);
}

void bindSdhciAspeed()
{
    using namespace std::chrono_literals;
    sysfsWrite("/sys/bus/platform/drivers/sdhci-aspeed/bind",
               emmc::layout::kSdhciControllerName);
    std::this_thread::sleep_for(1s);
}

EudaResult eudaConfig(std::string_view device, bool recheck)
{
    std::array<uint8_t, 512> ext_csd{};
    if (!ioctl::readExtCsd(device, ext_csd))
    {
        std::cerr << std::format(
            "euda: readExtCsd({}) failed — cannot read PARTITION_SETTING / "
            "ENH_SIZE_MULT / HC_*_GRP_SIZE; aborting\n",
            device);
        return EudaResult::Failed;
    }

    const uint8_t completed =
        ext_csd[emmc::extcsd::kPartition_Setting_Completed];
    const uint32_t programmed =
        read24Le(ext_csd, emmc::extcsd::kEnh_Size_Mult_Lsb);
    const uint32_t max_enh =
        read24Le(ext_csd, emmc::extcsd::kMax_Enh_Size_Mult_Lsb);
    const uint8_t hc_wp = ext_csd[emmc::extcsd::kHc_Wp_Grp_Size];
    const uint8_t hc_erase = ext_csd[emmc::extcsd::kHc_Erase_Grp_Size];
    const uint64_t unit_bytes = enhSizeMultUnitBytes(ext_csd);
    const uint64_t programmed_bytes =
        static_cast<uint64_t>(programmed) * unit_bytes;
    // Expected size derived from EXT_CSD's MAX_ENH_SIZE_MULT — the
    // maximum enhanced area the chip itself reports.
    const uint64_t expected_bytes =
        static_cast<uint64_t>(max_enh) * unit_bytes;
    // Real user-area size from SEC_COUNT (EXT_CSD bytes 212–215, LE).
    // Per JESD84-B51 §7.4.81, SEC_COUNT is updated when
    // PARTITION_SETTING_COMPLETED is written, and when ENH_SIZE_MULT
    // is programmed to MAX the entire user-addressable area becomes
    // the enhanced region — so real_size_bytes == programmed_bytes is
    // the true post-activation signal. If the values still differ,
    // the chip hasn't applied the new EXT_CSD state yet (HW reset
    // didn't take effect on the card side).
    const uint32_t sec_count =
        read32Le(ext_csd, emmc::extcsd::kSec_Count_Lsb);
    const uint64_t real_size_bytes =
        static_cast<uint64_t>(sec_count) * 512ULL;

    // AST2700: program the maximum enhanced area the device allows.
    const uint32_t target = max_enh;

    std::cerr << std::format(
        "euda: state{} device={} PARTITION_SETTING_COMPLETED={} "
        "ENH_SIZE_MULT={} MAX_ENH_SIZE_MULT={} HC_WP_GRP_SIZE={} "
        "HC_ERASE_GRP_SIZE={} unit={}B programmed={}B expected={}B "
        "real_size={}B (SEC_COUNT={})\n",
        recheck ? " (post-reset recheck)" : "", device, completed, programmed,
        max_enh, hc_wp, hc_erase, unit_bytes, programmed_bytes, expected_bytes,
        real_size_bytes, sec_count);

    if (completed != 0 && programmed == target)
    {
        // EXT_CSD writes succeeded and the bits are sticky. The card
        // is fully activated only when SEC_COUNT reflects the new
        // enhanced-only addressable area. Until then the chip is
        // still serving the pre-activation user area and any I/O
        // would see the wrong geometry.
        if (real_size_bytes == programmed_bytes)
        {
            std::cerr
                << "euda: activation verified "
                   "(PARTITION_SETTING_COMPLETED=1, ENH_SIZE_MULT matches "
                   "MAX, SEC_COUNT × 512 == enhanced area) — no action "
                   "needed\n";
            return EudaResult::Ok;
        }
        std::cerr << std::format(
            "euda: PARTITION_SETTING_COMPLETED=1 and ENH_SIZE_MULT=MAX but "
            "SEC_COUNT × 512 ({}B) ≠ enhanced area ({}B) — chip has not "
            "applied the new layout yet; returning NotActivated so the "
            "caller retries HW reset\n",
            real_size_bytes, programmed_bytes);
        return EudaResult::NotActivated;
    }

    if (completed != 0 && programmed != target)
    {
        // PARTITION_SETTING_COMPLETED is one-time write — once set the
        // device cannot be reprogrammed to a different size.
        std::cerr << std::format(
            "euda: PARTITION_SETTING_COMPLETED already set but "
            "ENH_SIZE_MULT={} ≠ target {} — device is locked to a different "
            "layout; cannot reprogram\n",
            programmed, target);
        return EudaResult::Failed;
    }

    std::cerr << std::format(
        "euda: programming ENH_SIZE_MULT={} (was {}) and setting "
        "PARTITION_SETTING_COMPLETED=1 via CMD6 SWITCH\n",
        target, programmed);

    if (!writeEnhSizeMult(device, target))
    {
        std::cerr << "euda: ENH_SIZE_MULT byte write failed\n";
        return EudaResult::Failed;
    }
    if (!ioctl::writeExtCsdByte(device,
                                emmc::extcsd::kPartition_Setting_Completed,
                                0x01))
    {
        std::cerr
            << "euda: PARTITION_SETTING_COMPLETED byte write failed\n";
        return EudaResult::Failed;
    }

    // EXT_CSD is written but inactive until power cycle / HW reset
    // (JESD84-B51 §6.6.4). Caller retries after reset.
    std::cerr
        << "euda: EXT_CSD written; eMMC needs a power cycle / HW reset to "
           "activate the new EUDA layout (returning NotActivated)\n";
    return EudaResult::NotActivated;
}

EudaResult eudaConfigWithHwReset(std::string_view device)
{
    using namespace std::chrono_literals;
    std::cerr << std::format("euda: configuring {}\n", device);

    auto first = eudaConfig(device, /*recheck=*/false);
    if (first == EudaResult::Ok)
    {
        return first;
    }
    if (first == EudaResult::Failed)
    {
        std::cerr << "euda: initial configuration failed; no HW reset retry\n";
        return first;
    }

    // NotActivated: drive the chip through a real RST_n cycle to make the
    // new ENH_SIZE_MULT / PARTITION_SETTING_COMPLETED take effect.
    std::cerr
        << "euda: triggering unbind → HW reset → rebind to apply the new "
           "EUDA layout\n";

    if (!ensureHwResetEnabled(device))
    {
        std::cerr << "euda: chip-side HW reset enable failed; GPIO pulse "
                     "may not actually reset the eMMC (continuing anyway)\n";
    }

    unbindSdhciAspeed();
    if (!pulseHwResetGpio())
    {
        std::cerr << "euda: GPIO toggle failed; rebinding driver anyway\n";
    }
    bindSdhciAspeed();

    for (int retry = 0; retry < 3; ++retry)
    {
        std::cerr << std::format("euda: post-reset recheck attempt {}/3\n",
                                 retry + 1);
        if (eudaConfig(device, /*recheck=*/true) == EudaResult::Ok)
        {
            std::cerr << std::format(
                "euda: EUDA activated successfully after HW reset "
                "(attempt {}/3)\n",
                retry + 1);
            return EudaResult::Ok;
        }
        std::this_thread::sleep_for(1s);
    }
    std::cerr
        << "euda: EUDA still not activated after 3 post-reset rechecks\n";
    return EudaResult::NotActivated;
}

} // namespace emmc::init
