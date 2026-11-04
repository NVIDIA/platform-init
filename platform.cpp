// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#include "gpio.hpp"
#include "i2c.hpp"
#include "intel/platforms.hpp"
#include "nvidia/platforms.hpp"
#include "nvidia/switch_mctp.hpp"
#include "nvidia/switch_misc.hpp"
#include "nvidia/switch_preps.hpp"
#include "nvidia/switch_rtc.hpp"
#include "utilities.hpp"

#include <fcntl.h>
#include <systemd/sd-daemon.h>

#include <CLI/CLI.hpp>
#include <gpiod.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>

constexpr auto init_functions =
    std::to_array<std::pair<std::string_view, int (*)()>>(
        {{"intel-acrp", intel::init_acrp},
         {"intel-jcrp", intel::init_jcrp},
         {"nvidia-gb200", nvidia::init_gb200_base},
         {"nvidia-gb200-with-p2020", nvidia::init_gb200_with_p2020},
         {"nvidia-gb200nvl-bmc", nvidia::init_gb200nvl_bmc},
         {"nvidia-gb200nvl-hmc", nvidia::init_gb200nvl_hmc},
         {"nvidia-hgxr-hmc", nvidia::init_hgxr_hmc},
         {"nvidia-nvl32", nvidia::init_nvl32},
         {"nvidia-vr-c2-bmc", nvidia::init_vr_c2_bmc},
         {"nvidia-vr-nvl", nvidia::init_vr_nvl},
         {"nvidia-vr-nvl-bmc", nvidia::init_vr_nvl_bmc},
         {"nvidia-vr-nvl-bmc-ast2700", nvidia::init_vr_nvl_bmc_ast2700},
         {"nvidia-vr-nvl-hmc", nvidia::init_vr_nvl_hmc},
         {"nvidia-vr-nvl-hmc-ast2700", nvidia::init_vr_nvl_hmc_ast2700},
         {"nvidia-vr-e5010-bmc", nvidia::init_vr_e5010_bmc},
         {"nvidia-vr-quark-bmc", nvidia::init_vr_quark_bmc},
         {"nvidia-vr-vhmc-quark", nvidia::init_vr_vhmc_quark},
         {"nvidia-vru-nvl-bmc", nvidia::init_vru_nvl_bmc},
         {"nvidia-vru-nvl-bmc-strata", nvidia::init_vr_nvl_bmc_ast2700},
         {"nvidia-vru-nvl-hmc", nvidia::init_vru_nvl_hmc},
         {"nvidia-switch-rosalind-bmc", nvidia::init_switch_rosalind_bmc},
         {"nvidia-switch-spc6-ast2600-bmc",
          nvidia::init_switch_spc6_ast2600_bmc},
         {"nvidia-switch-ast2700-bmc", nvidia::init_switch_ast2700_bmc}});

// Before sysinit.target (platform_early_init.service), in the initramfs
// scripts' place.
constexpr auto early_init_functions =
    std::to_array<std::pair<std::string_view, int (*)()>>(
        {{"nvidia-vr-nvl-hmc", nvidia::early_init_vr_nvl_hmc},
         {"nvidia-vr-nvl-hmc-ast2700", nvidia::early_init_vr_nvl_hmc_ast2700}});

static int run(std::span<const std::pair<std::string_view, int (*)()>> table,
               std::string_view name, const CLI::App* sub)
{
    const auto it = std::ranges::find_if(table, [name](const auto& val) {
        return val.first == name;
    });
    if (it == table.end())
    {
        std::cerr << sub->help() << "\n";
        return EXIT_FAILURE;
    }
    return it->second();
}

int main(int argc, char** argv)
{
    CLI::App app("Platform init CLI");

    app.require_subcommand();

    CLI::App* init_sub =
        app.add_subcommand("init", "Initialize the platform and daemonize");
    std::string platform_name;
    init_sub
        ->add_option("platform_name", platform_name,
                     "Name of the platform to init")
        ->required();
    CLI::App* early_sub = app.add_subcommand(
        "early-init", "Bring up what the platform needs before sysinit");
    early_sub->add_option("platform_name", platform_name)->required();
    CLI::App* preps_sub = app.add_subcommand(
        "preps", "Stage the platform-specific files before udev starts");
    preps_sub->add_option("platform_name", platform_name)->required();
    CLI::App* rtc_sub =
        app.add_subcommand("rtc-detect", "Bind the external RTC");
    CLI::App* slave_sub = app.add_subcommand(
        "i2c-slave-setup", "Instantiate the recovery EEPROM I2C slave");
    CLI::App* mctp_sub = app.add_subcommand(
        "mctp-ready", "Wait for the MCUs and their MCTP USB links");
    app.require_subcommand();

    CLI11_PARSE(app, argc, argv)

    if (preps_sub->parsed())
    {
        return nvidia::switch_plat_preps(platform_name);
    }
    if (rtc_sub->parsed())
    {
        return nvidia::switch_rtc_detect();
    }
    if (slave_sub->parsed())
    {
        return nvidia::switch_misc::i2c_slave_setup();
    }
    if (mctp_sub->parsed())
    {
        return nvidia::switch_mctp_ready();
    }

    if (early_sub->parsed())
    {
        return run(early_init_functions, platform_name, early_sub);
    }
    return run(init_functions, platform_name, init_sub);
}
