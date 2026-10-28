// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

#include "compute_hmc_common.hpp"

namespace nvidia
{

bool set_hmc_ready(std::string_view ready_pin)
{
    const std::string path{ready_pin};
    if (!std::filesystem::exists(path))
    {
        std::cerr << std::format("[ERROR] {} does not exist!\n", path);
        return false;
    }
    if (std::ofstream f{path}; f)
        f << "1\n";
    std::string val;
    if (std::ifstream f{path}; f)
        f >> val;
    if (val != "1")
    {
        std::cerr << "[ERROR] Failed to assert HMC_READY-O\n";
        return false;
    }
    std::cerr << "HMC_READY-O has been asserted\n";
    return true;
}

} // namespace nvidia

namespace nvidia::compute
{

void hmc_set_initial_gpio_out(std::string_view tag,
                              std::span<const GpioOut> outputs)
{
    std::cerr << std::format("{}: initializing HMC GPIO output states\n", tag);
    for (const auto& [line, value] : outputs)
        gpio::set(line, value);
}

void release_hpm_mcu_reset(std::string_view tag,
                           std::span<const char* const> enable_lines)
{
    for (size_t b = 0; b < enable_lines.size(); ++b)
    {
        std::cerr << std::format("{}: taking Board {} HPM MCU out of reset\n",
                                 tag, b);
        gpio::set(enable_lines[b], 1);
    }
}

bool check_hmc_filesystems()
{
    if (!check_rw_filesystems())
    {
        std::cerr << "[ERROR] Filesystem mount check failure\n";
        return false;
    }
    if (!check_rofs())
    {
        std::cerr << "[ERROR] HMC booted in ROFS, Read-Only mode\n";
        return false;
    }
    return true;
}

int hmc_ready(std::string_view tag, std::string_view ready_pin)
{
    std::cerr << "[WARNING] GPU Temp thresholds need to be set\n";

    if (!set_hmc_ready(ready_pin))
        return EXIT_FAILURE;

    std::cerr << std::format("{}: platform init complete\n", tag);
    return notify_ready_and_wait();
}

} // namespace nvidia::compute
