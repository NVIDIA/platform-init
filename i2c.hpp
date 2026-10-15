// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace i2c
{

void rebind_controller(const std::string_view number);
void new_device(unsigned int bus, unsigned int address,
                std::string_view device_type);

// Source: i2ctransfer -f -y <bus> w<n>@<addr> <write…> r<m>  (I2C_RDWR ioctl)
// Either span may be empty. Used instead of RawDevice because the addresses
// reached this way are already claimed by a kernel driver, so RawDevice's
// I2C_SLAVE ioctl fails with EBUSY in the constructor — the reason the shell
// passed -f. The SMBus accessors are additionally limited to a one-byte
// command code, which cannot address the CPLD's two-byte register pair.
// Returns 0, or a positive errno.
int raw_transfer(unsigned int bus, uint8_t address,
                 std::span<const uint8_t> write, std::span<uint8_t> read);
std::expected<void, std::error_code> bind_device(
    unsigned int bus, unsigned int address, std::string_view driver_name);

// a simple RAII wrapper for raw i2c comms
struct RawDevice
{
    RawDevice(size_t bus, uint8_t address);
    ~RawDevice();
    RawDevice(const RawDevice&) = delete;
    RawDevice& operator=(const RawDevice&) = delete;
    RawDevice& operator=(RawDevice&&) = default;
    RawDevice(RawDevice&&) = default;

    std::expected<uint8_t, std::error_code> read_byte(uint8_t reg);

    std::expected<void, std::error_code> write_block(
        std::span<const uint8_t> data);

    std::expected<std::vector<uint8_t>, std::error_code> read_i2c_block(
        uint8_t reg, size_t len);

    // Source: i2cset -y <bus> <addr> <reg> <val> b  (SMBus write-byte-data)
    int write_byte(uint8_t reg, uint8_t val);

    // Source: i2cset -y <bus> <addr> <reg> <val> w  (SMBus write-word-data)
    int write_word(uint8_t reg, uint16_t val);

    // SMBus send-byte: single byte to <addr> on <bus>, no register/command code
    int send_byte(uint8_t cmd);

    int fd;
};
} // namespace i2c
