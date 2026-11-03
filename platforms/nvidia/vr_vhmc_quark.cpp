// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// Platform init for the vr-quark-bmc virtual HMC, a systemd-nspawn guest of
// the BMC: no eMMC, no I2C mailbox. It takes the board's HPM MCU out of reset,
// as enable_hpm_mcu.sh did, and brings up the MCTP links, as vhmc-init.sh did.

#include "compute_hmc_common.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

namespace nvidia
{

struct MctpLink
{
    std::string_view name;
    unsigned eid;
};

// The links the BMC hands to the container at start, with this HMC's EID on
// each. The BMC sets up the links it moves in later itself.
static constexpr std::array<MctpLink, 3> mctp_links{{
    {"musb1_1_3_1", 8}, // GPU-MCU USB
    {"musb1_1_3_2", 8}, // CPU USB
    {"mctpi2c1", 200},
}};

static bool run(std::string_view tag, const std::string& cmd)
{
    const int rc = std::system(cmd.c_str()); // NOLINT(cert-env33-c)
    if (rc != 0)
    {
        std::cerr << std::format("{}: '{}' failed (status {})\n", tag, cmd, rc);
    }
    return rc == 0;
}

static void setup_mctp_links(std::string_view tag)
{
    for (const auto& [name, eid] : mctp_links)
    {
        if (!std::filesystem::exists(std::format("/sys/class/net/{}", name)))
        {
            std::cerr << std::format("{}: MCTP link {} not present\n", tag,
                                     name);
            continue;
        }
        if (run(tag, std::format("/usr/bin/mctp link set {} up", name)))
        {
            run(tag,
                std::format("/usr/bin/mctp addr add {} dev {}", eid, name));
        }
    }
}

static constexpr std::array<const char*, 1> mcu_enable_lines{
    "BRD0_HMC_TO_MCU_I2C_EN-O"};

int init_vr_vhmc_quark()
{
    constexpr std::string_view tag = "vr-vhmc-quark";
    compute::release_hpm_mcu_reset(tag, mcu_enable_lines);
    setup_mctp_links(tag);
    return notify_ready_and_wait();
}

} // namespace nvidia
