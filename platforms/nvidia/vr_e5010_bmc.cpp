// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for vr-e5010-bmc.

#include "compute_bmc_common.hpp"
#include "utilities.hpp"

#include <iostream>

namespace nvidia
{

using namespace std::chrono_literals;

// wait_standby_pwr_p4103: 200 polls at 0.1s.
static constexpr std::chrono::seconds stby_pg_timeout = 20s;
static constexpr std::chrono::milliseconds stby_pg_poll = 100ms;

int init_vr_e5010_bmc()
{
    static constexpr auto tag = "vr-e5010-bmc";
    std::cerr << "vr-e5010-bmc: platform init start\n";

    compute::create_state_dir();

    std::cerr << "vr-e5010-bmc: PERST highZ sequence\n";
    for (uint8_t dev_addr : {uint8_t{0x25}, uint8_t{0x26}})
    {
        // i2cset -f ... w: I2C_RDWR ignores a bound driver, as -f does.
        for (uint8_t reg : {uint8_t{0x02}, uint8_t{0x06}})
        {
            const std::array<uint8_t, 3> word{reg, 0x00, 0x00};
            if (int rc = i2c::raw_transfer(2, dev_addr, word, {}); rc != 0)
                std::cerr << std::format(
                    "vr-e5010-bmc: PERST write_word({:#04x}) bus=2 addr={:#x}: "
                    "{}\n",
                    reg, dev_addr, std::strerror(rc));
        }
    }

    if (compute::bmc_cold_boot("B0_M0_STBY_POWER_PG-I"))
    {
        gpio::set("GLOBAL_WP_BMC-O", 0);
        gpio::set("HMC_RESET_L-O", 0);
    }

    // wait_standby_pwr_p4103 reads only the standby power monitor's file.
    if (!wait_standby_power_good(nullptr, {},
                                 "/run/bmc-state/BMC_STANDBY_POWER_PG",
                                 stby_pg_timeout, stby_pg_poll))
    {
        std::cerr << "vr-e5010-bmc: disabling STBY_POWER_EN-O\n";
        gpio::set("STBY_POWER_EN-O", 0);
        return EXIT_FAILURE;
    }

    compute::wait_iox(tag, 0x20, {2});

    hmc_mux_select("HMC_PRSNT_R-I", "BMC_HMC_MUX_SEL-O");

    // 1-0020, 1-0021: HMC bypass; 2-00xx: B0_M0 and 7-00xx: B1_M0 MGMT board
    compute::bind_expanders(1, {0x20, 0x21});
    compute::bind_expanders(2, {0x20, 0x21, 0x22, 0x23, 0x24});
    compute::bind_expanders(7, {0x20, 0x21, 0x22, 0x23, 0x24});

    if (!compute::wait_standby_power(tag, "B0_M0", compute::PgSource::Monitor,
                                     "B0_M0_STBY_POWER_PG-I",
                                     "B0_M0_PS_3V3_STBY_EN-O"))
    {
        std::cerr << "vr-e5010-bmc: FATAL: B0_M0 wait_standby_pwr failed\n";
        return EXIT_FAILURE;
    }

    gpio::set("B0_M0_PWR_BRAKE_IOX_L-O", 1);

    gpio::set("B0_M0_C0_SOCAMM_I2C_SEL-O", 1);

    gpio::set("B0_M0_BMC_SHDN_FORCE_L-O", 1);

    if (!compute::check_rofs_and_mtd(tag))
        return EXIT_FAILURE;

    return compute::bmc_ready(tag);
}

} // namespace nvidia
