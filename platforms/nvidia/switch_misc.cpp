// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "switch_misc.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>

namespace nvidia::switch_misc
{

// Shell arithmetic: 0x hex, leading 0 octal, else decimal.
static std::optional<unsigned> shell_number(std::string_view value)
{
    const std::string s{value};
    if (s.empty())
        return std::nullopt;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 0);
    if (*end != '\0' || v > 0xffff)
        return std::nullopt;
    return static_cast<unsigned>(v);
}

std::optional<SlaveConfig> parse_recovery_conf(std::string_view text)
{
    SlaveConfig cfg;
    std::istringstream in{std::string{text}};
    std::string line;
    while (std::getline(in, line))
    {
        const auto eq = line.find('=');
        if (line.starts_with('#') || eq == std::string::npos)
            continue;
        const std::string_view key{line.data(), eq};
        std::string_view value{line};
        value.remove_prefix(eq + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (key != "I2C_BUS" && key != "SLAVE_ADDR")
            continue;
        if (value.empty())
            continue; // ${VAR:-default}
        const auto n = shell_number(value);
        if (!n)
            return std::nullopt;
        (key == "I2C_BUS" ? cfg.bus : cfg.address) = *n;
    }
    return cfg;
}

int i2c_slave_setup(const std::string& conf)
{
    std::ifstream file{conf};
    if (!file)
    {
        std::cout << std::format("ERROR: Configuration file {} not found\n",
                                 conf);
        return 1;
    }
    std::stringstream text;
    text << file.rdbuf();
    const auto cfg = parse_recovery_conf(text.str());
    if (!cfg)
    {
        std::cout << "ERROR: invalid I2C_BUS or SLAVE_ADDR\n";
        return 1;
    }

    const auto addr = std::format("10{:02x}", cfg->address);
    const auto dev = std::format("/sys/bus/i2c/devices/{}-{}", cfg->bus, addr);
    std::error_code ec;
    if (std::filesystem::exists(dev, ec))
    {
        std::cout << std::format("I2C slave device already exists on bus {}\n",
                                 cfg->bus);
        return 0;
    }

    std::cout << std::format(
        "Creating I2C slave device on bus {}, address 0x{}\n", cfg->bus, addr);
    std::ofstream nd{
        std::format("/sys/bus/i2c/devices/i2c-{}/new_device", cfg->bus)};
    nd << std::format("slave-24c02 0x{}\n", addr) << std::flush;
    if (!nd)
    {
        std::cout << "ERROR: Failed to create I2C slave device\n";
        return 1;
    }
    nd.close();

    if (std::filesystem::exists(dev + "/slave-eeprom", ec))
    {
        std::cout << std::format(
            "I2C slave device created successfully: {}/slave-eeprom\n", dev);
        return 0;
    }
    std::cout << std::format("ERROR: I2C slave device not created on bus {}\n",
                             cfg->bus);
    return 1;
}

void i2c_slave_setup_if_configured(const std::string& conf)
{
    std::error_code ec;
    if (std::filesystem::exists(conf, ec) && i2c_slave_setup(conf) != 0)
        std::cout << "I2C slave setup failed; continuing\n";
}

} // namespace nvidia::switch_misc
