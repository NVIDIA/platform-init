// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA
//
// emmc-util.h — small shared helpers used by both nvidia-emmc-init and
// nvidia-emmc-monitor. Kept header-only where practical; the
// `rebootViaSystemd` implementation lives in emmc-util.cpp because it
// brings in <systemd/sd-bus.h>.

#pragma once

#include <unistd.h>

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace emmc::util
{

// RAII wrapper for a raw POSIX file descriptor — close-on-destruct, move
// transfers ownership, copy disabled.
class Fd
{
  public:
    Fd() = default;
    explicit Fd(int fd_in) noexcept : fd_(fd_in) {}
    ~Fd()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept
    {
        if (this != &other)
        {
            if (fd_ >= 0)
            {
                ::close(fd_);
            }
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

  private:
    int fd_ = -1;
};

// Build a kernel partition device path from a base device.
// Example: partDevice("/dev/mmcblk0", 3) → "/dev/mmcblk0p3".
inline std::string partDevice(std::string_view base_dev, int n)
{
    return std::format("{}p{}", base_dev, n);
}

// Issue a reboot via systemd1.Manager.Reboot. Returns false on D-Bus
// errors. Implementation in emmc-util.cpp.
bool rebootViaSystemd();

} // namespace emmc::util
