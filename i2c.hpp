// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

#pragma once

#include <cstdint>
#include <string>

namespace i2c
{

void rebind_controller(const std::string_view number);
void new_device(unsigned int bus, unsigned int address,
                std::string_view device_type);

// a simple RAII wrapper for raw i2c comms
struct RawDevice
{
    RawDevice(size_t bus, uint8_t address);
    ~RawDevice();
    RawDevice(const RawDevice&) = delete;
    RawDevice& operator=(const RawDevice&) = delete;
    RawDevice& operator=(RawDevice&&) = default;
    RawDevice(RawDevice&&) = default;

    int read_byte(uint8_t reg, uint8_t& val);

    // Source: i2cset -y <bus> <addr> <reg> <val> b  (SMBus write-byte-data)
    int write_byte(uint8_t reg, uint8_t val);

    // Source: i2cset -y <bus> <addr> <reg> <val> w  (SMBus write-word-data)
    int write_word(uint8_t reg, uint16_t val);

    // SMBus send-byte: single byte to <addr> on <bus>, no register/command code
    int send_byte(uint8_t cmd);

    int fd;
};
} // namespace i2c
