// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "switch_mcu.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace nvidia
{

namespace fs = std::filesystem;

namespace detail
{

McuConfig parse_mcu_config(std::string_view text)
{
    McuConfig config;
    const auto doc = nlohmann::json::parse(text, nullptr, false);
    if (!doc.is_object())
        return config;
    // The script's json_get_bool also takes the string "true".
    if (const auto it = doc.find("reset_pulse_high_low"); it != doc.end())
        config.pulse_high_low = (it->is_boolean() && it->get<bool>()) ||
                                *it == "true";
    const auto mcus = doc.find("mcus");
    if (mcus == doc.end() || !mcus->is_array())
        return config;
    for (const auto& mcu : *mcus)
    {
        auto field = [&mcu](const char* key) {
            const auto it = mcu.is_object() ? mcu.find(key) : mcu.end();
            return it != mcu.end() && it->is_string() ? it->get<std::string>()
                                                      : std::string{};
        };
        config.mcus.push_back(
            {field("name"), field("reset_gpio"), field("ready_gpio")});
    }
    return config;
}

std::optional<int> gpio_number_from_path(std::string_view path)
{
    if (path.ends_with("/value"))
        path.remove_suffix(6);
    if (const auto pos = path.rfind("/gpio"); pos != std::string_view::npos)
        path.remove_prefix(pos + 5);
    if (path.empty() || path.size() > 9 ||
        path.find_first_not_of("0123456789") != std::string_view::npos)
        return std::nullopt;
    return std::stoi(std::string{path});
}

char mcu_ready_level(std::string_view ready_name)
{
    return ready_name.ends_with("_L") ? '0' : '1';
}

} // namespace detail

// readlink -f: every component but the last must exist.
static std::optional<int> mcu_gpio_number(std::string_view gpio_dir,
                                          std::string_view name)
{
    if (name.empty())
        return std::nullopt;
    std::error_code ec;
    fs::path p = fs::path{gpio_dir} / name;
    if (fs::is_symlink(p, ec))
    {
        const auto target = fs::read_symlink(p, ec);
        p = target.is_absolute() ? target : p.parent_path() / target;
    }
    auto resolved = fs::canonical(p, ec);
    if (ec)
    {
        resolved = fs::canonical(p.parent_path(), ec) / p.filename();
        if (ec)
            return std::nullopt;
    }
    return detail::gpio_number_from_path(resolved.string());
}

static std::optional<std::string> mcu_gpio_get(int num)
{
    return detail::sysfs_read(sysfs_gpio_value_path(num));
}

static bool mcu_gpio_set(int num, std::string_view value)
{
    const auto path = sysfs_gpio_value_path(num);
    if (detail::sysfs_write(path, std::format("{}\n", value)) != 0)
    {
        std::cerr << std::format("switch_gpio_set: [warning] Failed to set "
                                 "GPIO {} value to {}\n",
                                 num, value);
        return false;
    }
    return true;
}

static void mcu_pulse(int num, bool high_low)
{
    std::string asserted = "1";
    std::string released = "0";
    if (!high_low)
    {
        const auto idle = mcu_gpio_get(num);
        if (!idle)
            return;
        asserted = *idle == "1" ? "0" : "1";
        released = *idle;
    }
    if (!mcu_gpio_set(num, asserted))
        return;
    std::this_thread::sleep_for(std::chrono::seconds{1});
    mcu_gpio_set(num, released);
}

static void mcu_wait_ready(const McuLines& mcu, std::optional<int> num)
{
    if (!num)
    {
        std::cerr << std::format(
            "MCU {}: ready gpio not set up; skipping ready wait\n", mcu.name);
        return;
    }
    const std::string level(1, detail::mcu_ready_level(mcu.ready));
    for (int waited = 0; waited < 10; ++waited)
    {
        if (mcu_gpio_get(*num).value_or("") == level)
        {
            std::cerr << std::format("MCU {}: ready (gpio {}) after {}s\n",
                                     mcu.name, *num, waited);
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
    std::cerr << std::format("MCU {}: NOT ready (gpio {}) after 10s\n",
                             mcu.name, *num);
}

void hw_mgmt_mcu_reset(std::string_view config, std::string_view gpio_dir)
{
    std::error_code ec;
    if (!fs::is_regular_file(config, ec))
        return;
    std::cerr << std::format("MCU: bring-up via {}\n", config);
    std::ifstream f{std::string{config}};
    const auto cfg = detail::parse_mcu_config(
        std::string{std::istreambuf_iterator<char>{f}, {}});

    // Two passes, as the script: every reset first, then every ready wait.
    for (const auto& mcu : cfg.mcus)
    {
        const auto num = mcu_gpio_number(gpio_dir, mcu.reset);
        if (!num)
        {
            std::cerr << std::format(
                "MCU {}: reset gpio not set up; skipping reset\n", mcu.name);
            continue;
        }
        std::cerr << std::format("MCU {}: pulsing reset (gpio {}) for 1s\n",
                                 mcu.name, *num);
        mcu_pulse(*num, cfg.pulse_high_low);
    }
    for (const auto& mcu : cfg.mcus)
        mcu_wait_ready(mcu, mcu_gpio_number(gpio_dir, mcu.ready));
}

} // namespace nvidia
