// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#include "gpio.hpp"
#include "i2c.hpp"
#include "intel/platforms.hpp"
#include "nvidia/platforms.hpp"
#include "utilities.hpp"

#include <fcntl.h>
#include <systemd/sd-daemon.h>

#include <CLI/CLI.hpp>
#include <gpiod.hpp>

#include <algorithm>
#include <array>
#include <iostream>
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
         {"nvidia-nvl32", nvidia::init_nvl32},
         {"nvidia-vr-c2-bmc", nvidia::init_vr_c2_bmc},
         {"nvidia-vr-nvl", nvidia::init_vr_nvl},
         {"nvidia-vr-nvl-bmc", nvidia::init_vr_nvl_bmc},
         {"nvidia-vr-nvl-bmc-ast2700", nvidia::init_vr_nvl_bmc_ast2700},
         {"nvidia-vr-nvl-hmc", nvidia::init_vr_nvl_hmc},
         {"nvidia-vr-nvl-hmc-ast2700", nvidia::init_vr_nvl_hmc_ast2700},
         {"nvidia-vr-e5010-bmc", nvidia::init_vr_e5010_bmc},
         {"nvidia-vr-quark-bmc", nvidia::init_vr_quark_bmc},
         {"nvidia-vru-nvl-bmc", nvidia::init_vru_nvl_bmc},
         {"nvidia-vru-nvl-bmc-strata", nvidia::init_vr_nvl_bmc_ast2700},
         {"nvidia-vru-nvl-hmc", nvidia::init_vru_nvl_hmc},
         {"nvidia-switch-rosalind-bmc", nvidia::init_switch_rosalind_bmc},
         {"nvidia-switch-spc6-ast2600-bmc",
          nvidia::init_switch_spc6_ast2600_bmc},
         {"nvidia-switch-ast2700-bmc", nvidia::init_switch_ast2700_bmc}});

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
    app.require_subcommand();

    CLI11_PARSE(app, argc, argv)

    const auto* it = std::ranges::find_if(
        init_functions,
        [&platform_name](const std::pair<std::string_view, int (*)()> val) {
            return val.first == platform_name;
        });
    if (it == init_functions.end())
    {
        std::cerr << init_sub->help() << "\n";
        return EXIT_FAILURE;
    }

    return it->second();
}
