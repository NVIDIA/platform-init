// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// HMC temperature sensor telemetry tool.
// Reads HMC SoC temperature from hwmon and writes it into the I2C slave EEPROM
// shared with the FPGA at offset 0xF0. Single-shot binary; one of two modes
// per invocation.
//
// EEPROM layout at offset 0xF0 (see GB200 NVL Firmware EAS Specification):
//   Bytes 0-3 (0xF0): current temperature — [fractional, LSB, MID, MSB]
//   Bytes 4-7 (0xF4): threshold (85°C)    — same encoding
//
// Encoding: 24-bit integer (degrees) + 8-bit fractional (256ths of a degree).
// e.g. 27.275°C → integer=27=0x1B, fractional=0.275×256≈70=0x46
//      bytes = [0x46, 0x1B, 0x00, 0x00]
//
// Modes:
//   hmc-temp-sensor --init    [bus]  one-time slave-device init + threshold
//                                    write at boot. Run from
//                                    hmc-temp-sensor-init.service.
//   hmc-temp-sensor --oneshot [bus]  single hwmon read + EEPROM write, then
//                                    exit. Run from hmc-temp-sensor.service,
//                                    triggered by hmc-temp-sensor.timer (1 Hz)
//                                    and/or phosphor-dbus-monitor on the
//                                    HMC SoC temperature property change.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

static constexpr uint8_t SLAVE_ADDR = 0x4f;
static constexpr const char* SLAVE_TYPE = "slave-24c512ro";
static constexpr const char* TEMP_INPUT = "/sys/class/hwmon/hwmon0/temp1_input";
static constexpr const char* TEMP_MAX = "/sys/class/hwmon/hwmon0/temp1_max";
static constexpr int32_t THRESHOLD_MILLIDEG = 85000; // 85°C
static constexpr int TEMP_OFFSET = 0xF0;
static constexpr int THRESH_OFFSET = 0xF4;

// Encode millidegrees → [fractional, LSB, MID, MSB]
static std::array<uint8_t, 4> encode(int32_t millideg)
{
    uint32_t deg_int = static_cast<uint32_t>(millideg / 1000);
    uint8_t deg_frac = static_cast<uint8_t>((millideg % 1000) * 256 / 1000);
    return {deg_frac, static_cast<uint8_t>(deg_int & 0xFF),
            static_cast<uint8_t>((deg_int >> 8) & 0xFF),
            static_cast<uint8_t>((deg_int >> 16) & 0xFF)};
}

static void eeprom_write(const std::string& mem_file, int offset,
                         const std::array<uint8_t, 4>& data)
{
    std::ofstream f{mem_file, std::ios::binary | std::ios::in | std::ios::out};
    if (!f)
    {
        std::cerr << std::format("hmc-temp-sensor: cannot open {}\n", mem_file);
        return;
    }
    f.seekp(offset);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
}

static int do_init(unsigned bus, const std::string& slave_file,
                   const std::string& new_dev, const std::string& mem_file)
{
    // Wait for hwmon to appear (up to 80 s, matches the original shell script)
    for (int i = 0; !std::filesystem::exists(TEMP_MAX); ++i)
    {
        if (i >= 8)
        {
            std::cerr << "hmc-temp-sensor: hwmon path not found after 80s\n";
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds{10});
    }

    if (std::ofstream f{TEMP_MAX}; f)
        f << THRESHOLD_MILLIDEG;

    if (!std::filesystem::exists(slave_file))
    {
        if (std::ofstream f{new_dev}; f)
            f << std::format("{} 0x10{:02x}\n", SLAVE_TYPE, SLAVE_ADDR);
        else
        {
            std::cerr << std::format(
                "hmc-temp-sensor: cannot create slave on bus {}\n", bus);
            return 1;
        }
    }

    eeprom_write(mem_file, THRESH_OFFSET, encode(THRESHOLD_MILLIDEG));

    std::cerr << std::format(
        "hmc-temp-sensor: init done on bus {} slave 0x{:02x}\n", bus,
        SLAVE_ADDR);
    return 0;
}

static int do_oneshot(const std::string& mem_file)
{
    int32_t millideg = 0;
    if (std::ifstream f{TEMP_INPUT}; f)
        f >> millideg;
    eeprom_write(mem_file, TEMP_OFFSET, encode(millideg));
    return 0;
}

static void usage()
{
    std::cerr << "usage: hmc-temp-sensor --init|--oneshot [bus]\n";
}

int main(int argc, char** argv)
{
    enum class Mode
    {
        None,
        Init,
        Oneshot
    };
    Mode mode = Mode::None;
    unsigned bus = 3;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg{argv[i]};
        if (arg == "--init")
        {
            mode = Mode::Init;
        }
        else if (arg == "--oneshot")
        {
            mode = Mode::Oneshot;
        }
        else
        {
            try
            {
                bus = std::stoul(argv[i]);
            }
            catch (...)
            {
                std::cerr << std::format("hmc-temp-sensor: invalid arg '{}'\n",
                                         arg);
                usage();
                return 1;
            }
        }
    }

    if (mode == Mode::None)
    {
        usage();
        return 1;
    }

    const std::string slave_file = std::format(
        "/sys/bus/i2c/devices/i2c-{}/{}-10{:02x}/name", bus, bus, SLAVE_ADDR);
    const std::string new_dev =
        std::format("/sys/bus/i2c/devices/i2c-{}/new_device", bus);
    const std::string mem_file = std::format(
        "/sys/bus/i2c/devices/{}-10{:02x}/slave-eeprom", bus, SLAVE_ADDR);

    if (mode == Mode::Init)
        return do_init(bus, slave_file, new_dev, mem_file);

    return do_oneshot(mem_file);
}
