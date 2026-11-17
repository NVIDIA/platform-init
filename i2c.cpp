// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#include "i2c.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>

extern "C"
{
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}

namespace i2c
{

void rebind_controller(std::string_view number)
{
    std::string bindpath =
        std::format("/sys/bus/platform/drivers/aspeed-i2c-bus/unbind", number);
    std::ofstream bindofs(bindpath);
    if (!bindofs)
    {
        std::cerr << std::format("{} unable to open\n", bindpath);
        return;
    }
    try
    {
        bindofs << std::format("{}.i2c\n", number);
    }
    catch (const std::system_error& e)
    {
        std::cerr << std::format("{} unable to write\n", bindpath);
        return;
    }
    bindofs.close();
    std::cerr << std::format("{} unbound\n", number);

    std::string unbindpath =
        std::format("/sys/bus/platform/drivers/aspeed-i2c-bus/bind", number);
    std::ofstream unbindofs(unbindpath);
    if (!unbindofs)
    {
        std::cerr << std::format("{} unable to open\n", unbindpath);
        return;
    }
    try
    {
        unbindofs << std::format("{}.i2c\n", number);
    }
    catch (const std::system_error& e)
    {
        std::cerr << std::format("{} unable to write\n", unbindpath);
        return;
    }
    std::cerr << std::format("{} bound\n", number);
}

void new_device(unsigned int bus, unsigned int address,
                std::string_view device_type)
{
    std::string path =
        std::format("/sys/bus/i2c/devices/i2c-{}/new_device", bus);
    std::cerr << std::format("attempting to open {}\n", path);
    std::ofstream new_device(path);
    if (!new_device)
    {
        std::cerr << "Error: Unable to create I2C device\n";
        return;
    }
    new_device << std::format("{} 0x{:02x}", device_type, address);
    new_device.close();

    std::cerr << std::format("{} device created at bus {}\n", device_type, bus);
}

int raw_transfer(unsigned int bus, uint8_t address,
                 std::span<const uint8_t> write, std::span<uint8_t> read)
{
    if (write.empty() && read.empty())
    {
        return EINVAL;
    }

    // i2c_msg::len is 16-bit, so a longer span would be silently truncated.
    constexpr auto max_len = std::numeric_limits<decltype(i2c_msg::len)>::max();
    if (write.size() > max_len || read.size() > max_len)
    {
        return EINVAL;
    }

    std::string dev_path = std::format("/dev/i2c-{}", bus);
    int fd = open(dev_path.c_str(), O_RDWR);
    if (fd < 0)
    {
        int err = errno;
        std::cerr << std::format("failed to open {}\n", dev_path);
        return err;
    }

    std::array<i2c_msg, 2> msgs{};
    unsigned int count = 0;
    if (!write.empty())
    {
        msgs[count].addr = address;
        msgs[count].flags = 0;
        msgs[count].len = write.size();
        // The kernel does not modify the buffer of a write message; the cast
        // is needed only because i2c_msg::buf is not const-qualified.
        msgs[count].buf = const_cast<uint8_t*>(write.data());
        count++;
    }
    if (!read.empty())
    {
        msgs[count].addr = address;
        msgs[count].flags = I2C_M_RD;
        msgs[count].len = read.size();
        msgs[count].buf = read.data();
        count++;
    }

    i2c_rdwr_ioctl_data data{.msgs = msgs.data(), .nmsgs = count};

    // I2C_RDWR returns the number of messages transferred, not a status. A
    // short count on the write-then-read pair leaves the read buffer as the
    // caller supplied it, which a zero-initialised one cannot be told apart
    // from a real read.
    int rc = ioctl(fd, I2C_RDWR, &data);
    int err = rc < 0 ? errno : static_cast<unsigned>(rc) != count ? EIO : 0;
    close(fd);

    if (err != 0)
    {
        std::cerr << std::format(
            "i2c transfer to bus {} address 0x{:02x} failed: {}\n", bus,
            address, strerror(err));
    }
    return err;
}

bool probe(unsigned int bus, uint8_t address)
{
    const std::string dev_path = std::format("/dev/i2c-{}", bus);
    const int fd = open(dev_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    bool found = false;
    if (ioctl(fd, I2C_SLAVE, address) < 0)
    {
        found = errno == EBUSY;
    }
    else
    {
        found = i2c_smbus_write_quick(fd, I2C_SMBUS_WRITE) >= 0;
    }
    close(fd);
    return found;
}

std::expected<void, std::error_code> bind_device(
    unsigned int bus, unsigned int address, std::string_view driver_name)
{
    std::string path = std::format("/sys/bus/i2c/drivers/{}/bind", driver_name);
    std::ofstream bind_f(path);
    if (!bind_f)
    {
        return std::unexpected(
            errno ? std::error_code(errno, std::system_category())
                  : std::make_error_code(std::errc::io_error));
    }
    std::string device = std::format("{}-{:04x}", bus, address);
    errno = 0;
    bind_f << device << std::flush;
    if (!bind_f)
    {
        return std::unexpected(
            errno ? std::error_code(errno, std::system_category())
                  : std::make_error_code(std::errc::io_error));
    }
    std::cerr << std::format("bound {} to {} driver\n", device, driver_name);
    return {};
}

RawDevice::RawDevice(size_t bus, uint8_t address)
{
    std::string bus_path = std::format("/dev/i2c-{}", bus);
    std::filesystem::path dev_path = bus_path;
    fd = open(dev_path.c_str(), O_RDWR);
    if (fd < 0)
    {
        std::cerr << std::format("failed to open {}\n", dev_path.native());
        throw std::runtime_error(
            std::format("Failed to open {}", dev_path.native()));
    }

    if (ioctl(fd, I2C_SLAVE, address) < 0)
    {
        // dtor won't be called since we never finished constructing it, clean
        // up our fd
        close(fd);
        throw std::runtime_error(
            std::format("Failed to specify address {}", address));
    }
}

RawDevice::~RawDevice()
{
    close(fd);
}

std::expected<uint8_t, std::error_code> RawDevice::read_byte(uint8_t reg)
{
    int result = i2c_smbus_read_byte_data(fd, reg);
    if (result < 0)
    {
        return std::unexpected(
            std::error_code(-result, std::system_category()));
    }

    return result;
}

std::expected<void, std::error_code> RawDevice::write_block(
    std::span<const uint8_t> data)
{
    if (data.empty())
    {
        return std::unexpected(
            std::make_error_code(std::errc::invalid_argument));
    }
    ssize_t n = ::write(fd, data.data(), data.size());
    if (n < 0)
    {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }
    if (static_cast<size_t>(n) != data.size())
    {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    return {};
}

std::expected<std::vector<uint8_t>, std::error_code> RawDevice::read_i2c_block(
    uint8_t reg, size_t len)
{
    if (len == 0 || len > I2C_SMBUS_BLOCK_MAX)
    {
        return std::unexpected(
            std::make_error_code(std::errc::invalid_argument));
    }
    std::vector<uint8_t> buf(len);
    int32_t n = i2c_smbus_read_i2c_block_data(
        fd, reg, static_cast<uint8_t>(len), buf.data());
    if (n < 0)
    {
        return std::unexpected(std::error_code(-n, std::system_category()));
    }
    if (static_cast<size_t>(n) != len)
    {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    return buf;
}

// Source: i2cset -y <bus> <addr> <reg> <val> b  (SMBus write-byte-data)
int RawDevice::write_byte(uint8_t reg, uint8_t val)
{
    return i2c_smbus_write_byte_data(fd, reg, val) < 0 ? errno : 0;
}

// Source: i2cset -y <bus> <addr> <reg> <val> w  (SMBus write-word-data)
int RawDevice::write_word(uint8_t reg, uint16_t val)
{
    return i2c_smbus_write_word_data(fd, reg, val) < 0 ? errno : 0;
}

// SMBus send-byte: single byte to <addr> on <bus>, no register/command code
int RawDevice::send_byte(uint8_t cmd)
{
    return i2c_smbus_write_byte(fd, cmd) < 0 ? errno : 0;
}

} // namespace i2c
