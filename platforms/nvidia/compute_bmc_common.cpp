// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

#include "compute_bmc_common.hpp"

namespace nvidia
{

namespace detail
{

GpioLineState gpio_read_line_state(const char* line_name, std::error_code* ec)
{
    try
    {
        gpiod::line line = gpiod::find_line(line_name);
        if (!line)
        {
            return GpioLineState::Unavailable;
        }
        line.request({app_name, gpiod::line_request::DIRECTION_INPUT, 0});
        return line.get_value() == 1 ? GpioLineState::Asserted
                                     : GpioLineState::Deasserted;
    }
    catch (const std::system_error& e)
    {
        if (ec != nullptr)
        {
            *ec = e.code();
        }
        return GpioLineState::Error;
    }
}

} // namespace detail

int gpio_get(const char* line_name)
{
    std::cerr << std::format("{} Request to get\n", line_name);

    std::error_code ec;
    switch (detail::gpio_read_line_state(line_name, &ec))
    {
        case detail::GpioLineState::Asserted:
            std::cerr << std::format("{} was 1\n", line_name);
            return 1;
        case detail::GpioLineState::Deasserted:
            std::cerr << std::format("{} was 0\n", line_name);
            return 0;
        case detail::GpioLineState::Unavailable:
            std::cerr << std::format("{} Set unable to find\n", line_name);
            return -1;
        case detail::GpioLineState::Error:
            std::cerr << std::format("{} unable to get {}\n", line_name,
                                     std::system_error(ec).what());
            return -1;
    }
    return -1;
}

bool wait_gpio_asserted(const char* line_name, GpioLevel expected,
                        std::chrono::milliseconds timeout,
                        std::chrono::milliseconds poll_interval)
{
    using namespace std::chrono_literals;
    const auto level = static_cast<int>(expected);
    if ((expected != GpioLevel::Low && expected != GpioLevel::High) ||
        timeout <= 0ms || poll_interval <= 0ms)
    {
        std::cerr << std::format(
            "Invalid wait for {}: level={}, timeout={}ms, interval={}ms\n",
            line_name, level, timeout.count(), poll_interval.count());
        return false;
    }

    const auto target = expected == GpioLevel::High
                            ? detail::GpioLineState::Asserted
                            : detail::GpioLineState::Deasserted;
    // Counted polls, as the scripts' loops: slow reads stretch the wait
    // instead of cutting it short.
    for (auto polls = timeout / poll_interval; polls > 0; --polls)
    {
        if (detail::gpio_read_line_state(line_name) == target)
        {
            std::cerr << std::format("{} reached level {}\n", line_name, level);
            return true;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    std::cerr << std::format("{} failed to reach level {} within {}ms\n",
                             line_name, level, timeout.count());
    return false;
}

bool gpio_set_released(gpiod::line& line, int value)
{
    try
    {
        line.request({app_name, gpiod::line_request::DIRECTION_OUTPUT, 0},
                     value);
        line.release();
        std::cerr << std::format("{} Set to {}\n", line.name(), value);
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << std::format("{} unable to set to {}: {}\n", line.name(),
                                 value, e.what());
        return false;
    }
}

gpiod::line gpio_find(const char* line_name)
{
    try
    {
        if (auto line = gpiod::find_line(line_name))
            return line;
    }
    catch (const std::exception&)
    {}
    std::cerr << std::format("{} Unable to find\n", line_name);
    return {};
}

bool gpio_set_released(const char* line_name, int value)
{
    auto line = gpio_find(line_name);
    return line && gpio_set_released(line, value);
}

bool usb_hub_reset(const char* line_name)
{
    auto line = gpio_find(line_name);
    if (!line)
        return false;
    const bool asserted = gpio_set_released(line, 0);
    return gpio_set_released(line, 1) && asserted;
}

bool unit_active(sdbusplus::bus_t& bus, const std::string& unit)
{
    try
    {
        auto m = bus.new_method_call(
            "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "GetUnit");
        m.append(unit);
        sdbusplus::object_path path;
        bus.call(m).read(path);
        const std::string& unit_path = path;

        auto get =
            bus.new_method_call("org.freedesktop.systemd1", unit_path.c_str(),
                                "org.freedesktop.DBus.Properties", "Get");
        get.append("org.freedesktop.systemd1.Unit", "ActiveState");
        std::variant<std::string> state;
        bus.call(get).read(state);
        return std::get<std::string>(state) == "active";
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool state_file_asserted(const std::string& path)
{
    std::ifstream f{path};
    long value = 0;
    return f >> value && value > 0;
}

bool wait_standby_power_good(
    const char* line, const std::string& monitor, const std::string& state_file,
    std::chrono::milliseconds timeout, std::chrono::milliseconds poll_interval)
{
    using namespace std::chrono_literals;
    if (timeout <= 0ms || poll_interval <= 0ms)
    {
        std::cerr << std::format(
            "Invalid standby power wait: timeout={}ms, interval={}ms\n",
            timeout.count(), poll_interval.count());
        return false;
    }

    std::optional<sdbusplus::bus_t> bus;
    try
    {
        if (line != nullptr)
            bus.emplace(sdbusplus::bus::new_default_system());
    }
    catch (const std::exception& e)
    {
        std::cerr << std::format("standby power wait: D-Bus: {}\n", e.what());
    }

    const std::string name = line != nullptr ? std::string{line} : state_file;
    // Counted polls, as the scripts' loops: slow reads stretch the wait
    // instead of cutting it short.
    for (auto polls = timeout / poll_interval; polls > 0; --polls)
    {
        const bool from_file = line == nullptr ||
                               (bus && unit_active(*bus, monitor));
        if (from_file ? state_file_asserted(state_file)
                      : detail::gpio_read_line_state(line) ==
                            detail::GpioLineState::Asserted)
        {
            std::cerr << std::format("{} asserted\n", name);
            return true;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    std::cerr << std::format("{} failed to assert within {}ms\n", name,
                             timeout.count());
    return false;
}

void hmc_mux_select(const char* prsnt_gpio, const char* sel_gpio)
{
    const int present = gpio_get(prsnt_gpio);
    if (present == 1)
    {
        std::cerr << std::format(
            "hmc_mux_select: HMC present, {} high (dedicated path)\n",
            sel_gpio);
        gpio::set(sel_gpio, 1);
    }
    else
    {
        std::cerr << std::format(
            "hmc_mux_select: HMC absent, {} low (bypass)\n", sel_gpio);
        gpio::set(sel_gpio, 0);
    }
}

void usb_hub_reset_if_absent(const char* reset_line, const char* tty_dev)
{
    if (std::filesystem::is_character_file(tty_dev))
        return;
    std::cerr << std::format("{} missing, resetting {}\n", tty_dev, reset_line);
    gpio::set(reset_line, 0);
    gpio::set(reset_line, 1);
}

} // namespace nvidia

namespace nvidia::compute
{

using namespace std::chrono_literals;

namespace
{

constexpr auto poll_interval = 100ms;

} // namespace

bool bmc_cold_boot(const char* stby_pg_line)
{
    return gpio_get(stby_pg_line) <= 0;
}

void bind_expanders(unsigned bus, std::initializer_list<unsigned> addrs)
{
    for (unsigned addr : addrs)
        iox_bind(bus, addr);
}

bool wait_standby_power(std::string_view tag, std::string_view board,
                        PgSource src, const std::string& line,
                        const std::string& disable_line)
{
    bool ok = false;
    if (src == PgSource::Gpio)
        ok = wait_gpio_asserted(line.c_str(), GpioLevel::High, 20s,
                                poll_interval);
    else
    {
        std::string id{board};
        std::ranges::transform(id, id.begin(), [](unsigned char c) {
            return c == '_' ? '-' : static_cast<char>(std::tolower(c));
        });
        const auto monitor =
            board == "BMC"
                ? std::string{"nvidia-standby-power-monitor.service"}
                : std::format("nvidia-standby-power-monitor_{}{}.service",
                              board == "PDB" ? "" : "hpm_", id);
        const auto state_file =
            std::format("/run/bmc-state/{}{}_STANDBY_POWER_PG",
                        board == "BMC" || board == "PDB" ? "" : "HPM_", board);
        ok = wait_standby_power_good(line.c_str(), monitor, state_file, 20s,
                                     poll_interval);
    }
    if (ok)
        return true;
    std::cerr << std::format("{}: standby power for {} failed to assert, "
                             "disabling {}\n",
                             tag, board, disable_line);
    gpio::set(disable_line.c_str(), 0);
    return false;
}

bool wait_bmc_standby_power(std::string_view tag, PgSource src,
                            const std::string& line,
                            const std::string& disable_line)
{
    if (wait_standby_power(tag, "BMC", src, line, disable_line))
        return true;
    std::cerr << std::format("{}: FATAL: wait_standby_pwr BMC failed\n", tag);
    return false;
}

void wait_iox(std::string_view tag, unsigned addr,
              std::initializer_list<unsigned> buses)
{
    for (unsigned bus : buses)
        if (!iox_wait(bus, addr, 30s))
            std::cerr << std::format(
                "{}: Timeout: IOx {}-{:04x} is not present\n", tag, bus, addr);
}

bool wait_boards_standby_power(std::string_view tag,
                               std::span<const BoardLine> lines, PgSource src)
{
    for (const auto& l : lines)
    {
        if (iox_present(l.bus, l.iox) &&
            !wait_standby_power(tag, l.board, src, l.line, l.line))
        {
            std::cerr << std::format("{}: FATAL: {} wait_standby_pwr failed\n",
                                     tag, l.board);
            return false;
        }
    }
    return true;
}

bool wait_boards_ready(std::string_view tag, std::span<const BoardLine> lines,
                       std::string_view what)
{
    bool ok = true;
    for (const auto& l : lines)
    {
        if (iox_present(l.bus, l.iox) &&
            !wait_gpio_asserted(l.line, GpioLevel::High, 10s, poll_interval))
        {
            std::cerr << std::format(
                "{}: {} failed to assert after 10 seconds\n", tag, l.line);
            ok = false;
        }
    }
    if (!ok)
        std::cerr << std::format("{}: FATAL: {} timeout\n", tag, what);
    return ok;
}

bool check_rofs_and_mtd(std::string_view tag)
{
    if (!check_rofs())
    {
        std::cerr << std::format("{}: FATAL: check_rofs failed\n", tag);
        return false;
    }
    // RWFS is on SPI flash, hence the MTD variant.
    if (filesystem_check_mtd() != EXIT_SUCCESS)
    {
        std::cerr << std::format("{}: FATAL: check_rw_filesystems failed\n",
                                 tag);
        return false;
    }
    return true;
}

bool check_rofs_and_rw(std::string_view tag)
{
    if (!check_rofs())
    {
        std::cerr << std::format("{}: FATAL: check_rofs failed\n", tag);
        return false;
    }
    return check_rw_filesystems();
}

bool reset_usb_hub_if_absent(std::string_view tag, const char* line,
                             const char* tty)
{
    if (std::filesystem::is_character_file(tty))
        return true;
    std::cerr << std::format("{}: {} missing, resetting {}\n", tag, tty, line);
    if (usb_hub_reset(line))
        return true;
    std::cerr << std::format("{}: {} reset failed\n", tag, line);
    return false;
}

std::optional<RtcFound> rtc_detect(const Rtc& rtc)
{
    if (rtc.buses.empty() || rtc.parts.empty())
        return std::nullopt;
    auto found = detect_rtc(rtc.buses, rtc.parts);
    if (!found)
        return std::nullopt;
    const auto& part = rtc.parts[found->index];
    std::cerr << std::format(
        "rtc_detect: bus={} addr={:#x}: found {} (id={:#x})\n", found->bus,
        part.addr, part.driver, part.id);
    i2c::new_device(found->bus, part.addr, part.driver);
    return found;
}

int init_failed(const Rtc& rtc)
{
    rtc_detect(rtc);
    return EXIT_FAILURE;
}

int bmc_ready(std::string_view tag, const Rtc& rtc)
{
    std::cerr << std::format("{}: platform init complete\n", tag);
    if (rtc.buses.empty())
        return notify_ready_and_wait();
    gpio::release_all();
    sd_notify(0, "READY=1");
    rtc_detect(rtc);
    pause();
    return EXIT_SUCCESS;
}

} // namespace nvidia::compute
