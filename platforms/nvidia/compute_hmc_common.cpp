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

std::string uboot_env_read(std::string_view key,
                           const std::filesystem::path& root)
{
    std::error_code ec;
    for (const auto& e :
         std::filesystem::directory_iterator(root / "sys/class/mtd", ec))
    {
        if (detail::sysfs_read((e.path() / "name").string()) != "u-boot-env")
            continue;
        std::ifstream f{root / "dev" / e.path().filename()};
        f.ignore(5);
        const auto prefix = std::format("{}=", key);
        for (std::string var; std::getline(f, var, '\0') && !var.empty();)
            if (var.starts_with(prefix))
                return var.substr(prefix.size());
        break;
    }
    return {};
}

bool use_external_rtc(std::string_view tag, const RtcSync& cfg,
                      const RtcSyncIo& io)
{
    for (unsigned attempt = 1; attempt <= cfg.readiness_tries; ++attempt)
    {
        const int pg =
            cfg.host_power_good ? io.power_good(cfg.host_power_good) : -1;
        if (pg == 0 || pg == 1)
        {
            std::cerr << std::format("{}: {} = {}\n", tag, cfg.host_power_good,
                                     pg);
            return pg == 0;
        }
        if (const auto reason = io.env("reset_reason", io.root);
            !reason.empty())
        {
            std::cerr << std::format("{}: boot reason '{}'\n", tag, reason);
            return reason == "Power_On";
        }
        if (attempt < cfg.readiness_tries)
            std::this_thread::sleep_for(cfg.retry_delay);
    }
    std::cerr << std::format("{}: host power and boot reason unknown\n", tag);
    return false;
}

std::optional<std::string> newest_external_rtc(
    const std::filesystem::path& root)
{
    std::optional<std::string> newest;
    unsigned max = 0;
    std::error_code ec;
    for (const auto& e :
         std::filesystem::directory_iterator(root / "sys/class/rtc", ec))
    {
        const auto name = e.path().filename().string();
        unsigned n = 0;
        char extra = 0;
        if (std::sscanf(name.c_str(), "rtc%u%c", &n, &extra) == 1 && n > max)
        {
            max = n;
            newest = name;
        }
    }
    return newest;
}

static bool set_clock_from(std::string_view tag, const RtcSync& cfg,
                           const RtcSyncIo& io, const std::string& rtc)
{
    const auto value = detail::sysfs_read(
        (io.root / "sys/class/rtc" / rtc / "since_epoch").string());
    std::time_t epoch = 0;
    if (!value ||
        std::from_chars(value->data(), value->data() + value->size(), epoch)
                .ec != std::errc{})
        return false;
    if (epoch < cfg.min_epoch)
    {
        std::cerr << std::format("{}: {} reads {}, refusing to set the clock\n",
                                 tag, rtc, epoch);
        return false;
    }
    std::cerr << std::format("{}: clock set from {} ({})\n", tag, rtc, epoch);
    return io.set_clock(epoch);
}

static bool sync_external(std::string_view tag, const RtcSync& cfg,
                          const RtcSyncIo& io)
{
    const auto found = rtc_detect(cfg.external);
    if (!found)
        return false;
    const auto& part = cfg.external.parts[found->index];
    std::optional<std::string> rtc;
    // rtc_detect bound it once already.
    for (unsigned i = 1; !rtc && i <= cfg.bind_tries; ++i)
    {
        if (i > 1)
        {
            std::this_thread::sleep_for(cfg.bind_delay);
            i2c::new_device(found->bus, part.addr, part.driver);
        }
        std::this_thread::sleep_for(cfg.bind_delay);
        rtc = newest_external_rtc(io.root);
    }
    const bool ok = rtc && set_clock_from(tag, cfg, io, *rtc);
    if (ok)
    {
        const int fd = open((io.root / "dev/rtc0").c_str(), O_RDWR | O_CLOEXEC);
        if (const int err = fd < 0 ? errno : rtc_write_system_time(fd); err)
            std::cerr << std::format("{}: cannot write rtc0: {}\n", tag,
                                     std::strerror(err));
        if (fd >= 0)
            close(fd);
    }
    detail::sysfs_write(
        (io.root /
         std::format("sys/bus/i2c/devices/i2c-{}/delete_device", found->bus))
            .string(),
        std::format("{:#04x}", part.addr));
    return ok;
}

static bool sync_internal(std::string_view tag, const RtcSync& cfg,
                          const RtcSyncIo& io)
{
    for (unsigned attempt = 1; attempt <= cfg.internal_tries; ++attempt)
    {
        if (!std::filesystem::exists(io.root / "dev/rtc0"))
            std::cerr << std::format("{}: /dev/rtc0 not present yet\n", tag);
        else if (detail::sysfs_read(
                     (io.root / "sys/class/rtc/rtc0/hctosys").string()) == "1")
        {
            std::cerr << std::format("{}: clock already set from rtc0\n", tag);
            return true;
        }
        else if (set_clock_from(tag, cfg, io, "rtc0"))
            return true;
        if (attempt < cfg.internal_tries)
            std::this_thread::sleep_for(cfg.retry_delay);
    }
    std::cerr << std::format("{}: ERROR: clock not set from any RTC\n", tag);
    return false;
}

bool sync_clock_from_rtc(std::string_view tag, const RtcSync& cfg,
                         const RtcSyncIo& io)
{
    if (use_external_rtc(tag, cfg, io))
    {
        for (unsigned attempt = 1; attempt <= cfg.external_tries; ++attempt)
        {
            if (sync_external(tag, cfg, io))
                return true;
            if (attempt < cfg.external_tries)
                std::this_thread::sleep_for(cfg.retry_delay);
        }
        std::cerr << std::format("{}: external RTC failed, using rtc0\n", tag);
    }
    return sync_internal(tag, cfg, io);
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
