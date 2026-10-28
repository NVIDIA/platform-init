// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for vru-nvl-bmc (strata uses init_vr_nvl_bmc_ast2700).

#include "compute_bmc_common.hpp"
#include "utilities.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace nvidia
{

using namespace std::chrono_literals;

static constexpr auto smm_hub_rst_line = "BMC_TO_HUB_RST_1V8_L-O";
static constexpr std::string_view smm_hub_host = "12063000.usb";
static constexpr std::string_view smm_hub_id = "0424 4715";
static constexpr std::string_view smm_hub_lstp_id = "0424 4940";

static constexpr std::chrono::seconds lstp_poll = 1s;
static constexpr std::chrono::seconds smm_hub_rst_pulse = 1s;
static constexpr int e1s_line_wait_secs = 10;
static constexpr int hpm0_lstp_wait_secs = 10;
static constexpr int hpm1_lstp_wait_secs = 2;
static constexpr std::chrono::seconds hpm_ready_timeout = 10s;
static constexpr std::chrono::milliseconds hpm_ready_poll = 100ms;

static std::optional<std::string> usb_id(const std::filesystem::path& dev)
{
    const auto vid = detail::sysfs_read(dev / "idVendor");
    const auto pid = detail::sysfs_read(dev / "idProduct");
    if (!vid || !pid)
        return std::nullopt;
    return *vid + " " + *pid;
}

static bool smm_hub_lstp_stuck()
{
    std::error_code ec;
    std::vector<std::filesystem::path> devs;
    for (const auto& e :
         std::filesystem::directory_iterator("/sys/bus/usb/devices", ec))
        devs.push_back(e.path());
    std::ranges::sort(devs);

    for (const auto& dev : devs)
    {
        if (usb_id(dev) != smm_hub_lstp_id)
            continue;
        const auto real = std::filesystem::canonical(dev, ec);
        if (ec || !real.string().contains(std::format("/{}/", smm_hub_host)))
            continue;
        if (usb_id(real.parent_path()) != smm_hub_id)
            continue;
        const auto name = dev.filename().string();
        return !std::filesystem::exists(dev / (name + ":1.0") / "driver", ec);
    }
    return false;
}

// False when the reset line is absent and no reset was done.
static bool reset_smm_hub()
{
    if (!gpiod::find_line(smm_hub_rst_line))
        return false;
    std::cerr << "vru-nvl-bmc: resetting the SMM USB hub to recover its LSTP "
                 "function\n";
    gpio::set(smm_hub_rst_line, 0);
    std::this_thread::sleep_for(smm_hub_rst_pulse);
    gpio::set(smm_hub_rst_line, 1);
    return true;
}

static void enable_e1s_stby_power(int max_wait_secs)
{
    static constexpr auto line = "BMC_E1S_DRV_EN_L";
    int waited = 0;
    while (!gpiod::find_line(line))
    {
        if (waited >= max_wait_secs)
        {
            std::cerr << std::format("vru-nvl-bmc: {} not found after {}s, "
                                     "E1.S standby power left off\n",
                                     line, max_wait_secs);
            return;
        }
        // Only a reset that held the line for its pulse counts as waited.
        if (smm_hub_lstp_stuck() && reset_smm_hub())
            ++waited;
        std::this_thread::sleep_for(lstp_poll);
        ++waited;
    }
    gpio::set(line, 0);
    std::cerr << std::format("vru-nvl-bmc: E1.S standby power enabled ({}=0)\n",
                             line);
}

static bool wait_hpm_lstp(std::string_view board, int max_wait_secs)
{
    const auto line = std::format("{}_RUN_POWER_PG-I", board);
    for (int waited = 0; !gpiod::find_line(line); ++waited)
    {
        if (waited >= max_wait_secs)
            return false;
        std::this_thread::sleep_for(lstp_poll);
    }
    std::cerr << std::format("vru-nvl-bmc: {} LSTP GPIOs present\n", board);
    return true;
}

// CPLD_READY is only waited on if STBY_1V2 asserted.
static bool wait_hpm_ready(std::string_view board)
{
    const auto stby = std::format("{}_STBY_1V2_PWR_OK-I", board);
    const auto cpld = std::format("{}_CPLD_READY-I", board);
    if (wait_gpio_asserted(stby.c_str(), GpioLevel::High, hpm_ready_timeout,
                           hpm_ready_poll) &&
        wait_gpio_asserted(cpld.c_str(), GpioLevel::High, hpm_ready_timeout,
                           hpm_ready_poll))
        return true;
    std::cerr << std::format("vru-nvl-bmc: {} standby power or CPLD not ready "
                             "after {} seconds. Exiting w/ failure.\n",
                             board, hpm_ready_timeout.count());
    return false;
}

int init_vru_nvl_bmc()
{
    std::cerr << "vru-nvl-bmc: platform init start\n";

    enable_e1s_stby_power(e1s_line_wait_secs);

    if (!wait_hpm_lstp("B0_M0", hpm0_lstp_wait_secs))
    {
        std::cerr << "vru-nvl-bmc: FATAL: (bmc_ready) Timeout: B0_M0 LSTP "
                     "GPIOs not present\n";
        return EXIT_FAILURE;
    }

    const bool hpm1_present = wait_hpm_lstp("B1_M0", hpm1_lstp_wait_secs);

    // Both boards are polled before failing.
    const bool ok0 = wait_hpm_ready("B0_M0");
    const bool ok1 = !hpm1_present || wait_hpm_ready("B1_M0");
    if (!ok0 || !ok1)
    {
        std::cerr << "vru-nvl-bmc: FATAL: HPM standby power or CPLD not "
                     "ready\n";
        return EXIT_FAILURE;
    }

    if (!compute::check_rofs_and_rw("vru-nvl-bmc"))
        return EXIT_FAILURE;

    return compute::bmc_ready("vru-nvl-bmc");
}

} // namespace nvidia
