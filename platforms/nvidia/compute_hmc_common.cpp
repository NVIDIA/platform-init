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

// Binds a device the kernel gave up on before standby power was up. A
// bound one is left alone: re-binding would pull its gpiochip from under
// services already using its lines.
static void rebind(std::string_view tag, const std::filesystem::path& root,
                   unsigned bus, uint8_t addr, std::string_view driver)
{
    const auto dev = std::format("{}-{:04x}", bus, addr);
    const auto dir = root / "sys/bus/i2c/drivers" / driver;
    if (std::filesystem::exists(dir / dev))
        return;
    if (const int err = detail::sysfs_write(dir / "bind", dev); err != 0)
        std::cerr << std::format("{}: WARNING: failed to bind {} to {}: {}\n",
                                 tag, dev, driver, std::strerror(err));
    else
        std::cerr << std::format("{}: bound {} to {}\n", tag, dev, driver);
}

static bool wait_board(std::string_view tag, std::string_view name,
                       const StandbyBoard& board, const StandbyConfig& cfg,
                       const std::filesystem::path& root,
                       bool (*present)(unsigned, uint8_t))
{
    std::cerr << std::format("{}: waiting for {} (bus {}, {} attempts)\n", tag,
                             name, board.bus, board.attempts);
    for (unsigned attempt = 1; attempt <= board.attempts; ++attempt)
    {
        if (present(board.bus, board.iox[0]))
        {
            std::cerr << std::format("{}: {} detected after {} attempts\n", tag,
                                     name, attempt);
            for (const uint8_t addr : board.iox)
                rebind(tag, root, board.bus, addr, "pca953x");
            if (board.fru)
                rebind(tag, root, board.bus, *board.fru, "at24");
            return true;
        }
        std::this_thread::sleep_for(cfg.poll);
    }
    return false;
}

void wait_for_standby_power(std::string_view tag, const StandbyConfig& cfg,
                            const std::filesystem::path& root,
                            bool (*present)(unsigned, uint8_t))
{
    if (!wait_board(tag, "BRD0", cfg.brd0, cfg, root, present))
    {
        std::cerr << std::format(
            "{}: FAILURE: BRD0 not detected, standby power failure\n", tag);
        std::ofstream marker{root / "run/standby_power_failure"};
        return;
    }
    if (!wait_board(tag, "BRD1", cfg.brd1, cfg, root, present))
    {
        std::cerr << std::format("{}: BRD1 not present\n", tag);
        std::ofstream marker{root / "run/brd1_not_present"};
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
