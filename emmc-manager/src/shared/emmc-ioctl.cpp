// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-ioctl.h"

#include "emmc-ext-csd.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <linux/mmc/ioctl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <thread>

// MMC response / command flags live in kernel-internal <linux/mmc/core.h>,
// not in UAPI. Re-declare locally (same pattern as mmc-utils).
#ifndef MMC_RSP_PRESENT
#define MMC_RSP_PRESENT (1U << 0)
#define MMC_RSP_136     (1U << 1)
#define MMC_RSP_CRC     (1U << 2)
#define MMC_RSP_BUSY    (1U << 3)
#define MMC_RSP_OPCODE  (1U << 4)
#define MMC_RSP_R1      (MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)
#define MMC_RSP_R1B \
    (MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE | MMC_RSP_BUSY)
#define MMC_CMD_AC   (0U << 5)
#define MMC_CMD_ADTC (1U << 5)
#endif

namespace emmc::ioctl
{

namespace
{

// Logical block size for BLK* ioctls on eMMC. The kernel always reports
// sizes/offsets in 512-byte sector units regardless of the underlying
// flash page geometry.
constexpr uint64_t kSectorSizeBytes = 512U;

struct Fd
{
    explicit Fd(int f) : fd(f) {}
    ~Fd()
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd(o.fd) { o.fd = -1; }
    Fd& operator=(Fd&&) = delete;
    int fd;
};

Fd openRw(std::string_view device, const char* purpose)
{
    std::string dev_z(device);
    Fd h(::open(dev_z.c_str(), O_RDWR | O_CLOEXEC));
    if (h.fd < 0)
    {
        std::cerr << std::format("emmc-ioctl: open({}) for {} failed: {}\n",
                                 dev_z, purpose, std::strerror(errno));
    }
    return h;
}

} // namespace

std::optional<DiskGeometry> readDiskGeometry(std::string_view device)
{
    std::string dev_z(device);
    Fd h(::open(dev_z.c_str(), O_RDONLY | O_CLOEXEC));
    if (h.fd < 0)
    {
        std::cerr << std::format("emmc-ioctl: open({}) failed: {}\n", dev_z,
                                 std::strerror(errno));
        return std::nullopt;
    }
    uint64_t bytes = 0;
    if (::ioctl(h.fd, BLKGETSIZE64, &bytes) != 0)
    {
        std::cerr << std::format("emmc-ioctl: BLKGETSIZE64({}) failed: {}\n",
                                 dev_z, std::strerror(errno));
        return std::nullopt;
    }
    return DiskGeometry{bytes, bytes / kSectorSizeBytes};
}

bool isBlockDevice(std::string_view path)
{
    std::string p(path);
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0)
    {
        return false;
    }
    return S_ISBLK(st.st_mode);
}

bool triggerPartitionTableReread(std::string_view device)
{
    std::string dev_z(device);
    Fd h(::open(dev_z.c_str(), O_RDONLY | O_CLOEXEC));
    if (h.fd < 0)
    {
        std::cerr << std::format(
            "emmc-ioctl: open({}) for BLKRRPART failed\n", dev_z);
        return false;
    }
    if (::ioctl(h.fd, BLKRRPART) != 0)
    {
        std::cerr << std::format("emmc-ioctl: BLKRRPART({}) failed: {}\n",
                                 dev_z, std::strerror(errno));
        return false;
    }
    return true;
}

bool secureEraseRange(std::string_view device, uint64_t start_sector,
                      uint64_t end_sector, int retries)
{
    using namespace std::chrono_literals;
    std::string dev_z(device);

    // Guard against caller passing inverted bounds: with unsigned
    // arithmetic, end - start + 1 would wrap and BLKSECDISCARD would
    // target a huge byte range.
    if (end_sector < start_sector)
    {
        std::cerr << std::format(
            "emmc-ioctl: invalid BLKSECDISCARD range on {}: start={} end={}\n",
            dev_z, start_sector, end_sector);
        return false;
    }
    const uint64_t sector_count = end_sector - start_sector + 1U;

    for (int attempt = 0; attempt < retries; ++attempt)
    {
        Fd h = openRw(device, "BLKSECDISCARD");
        if (h.fd < 0)
        {
            std::this_thread::sleep_for(1s);
            continue;
        }
        // BLKSECDISCARD range is in bytes.
        uint64_t range[2] = {start_sector * kSectorSizeBytes,
                             sector_count * kSectorSizeBytes};
        if (::ioctl(h.fd, BLKSECDISCARD, &range) == 0)
        {
            return true;
        }
        std::cerr << std::format(
            "emmc-ioctl: BLKSECDISCARD({}) failed (attempt {}): {}\n", dev_z,
            attempt, std::strerror(errno));
        std::this_thread::sleep_for(1s);
    }
    return false;
}

bool readExtCsd(std::string_view device, std::array<uint8_t, 512>& out)
{
    std::string dev_z(device);
    Fd h = openRw(device, "EXT_CSD");
    if (h.fd < 0)
    {
        return false;
    }

    struct mmc_ioc_cmd cmd{};
    cmd.opcode = emmc::extcsd::kMmcSendExtCsd;
    cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;
    cmd.blksz = emmc::extcsd::kExtCsdSizeBytes;
    cmd.blocks = 1;
    mmc_ioc_cmd_set_data(cmd, reinterpret_cast<uintptr_t>(out.data()));

    if (::ioctl(h.fd, MMC_IOC_CMD, &cmd) != 0)
    {
        std::cerr << std::format(
            "emmc-ioctl: MMC_IOC_CMD MMC_SEND_EXT_CSD({}) failed: {}\n",
            dev_z, std::strerror(errno));
        return false;
    }
    return true;
}

bool writeExtCsdByte(std::string_view device, uint8_t index, uint8_t value)
{
    std::string dev_z(device);
    Fd h = openRw(device, "SWITCH");
    if (h.fd < 0)
    {
        return false;
    }

    // CMD6 SWITCH arg (JESD84-B51 §6.10.4):
    //   [25:24] access mode | [23:16] index | [15:8] value
    struct mmc_ioc_cmd cmd{};
    cmd.opcode = emmc::extcsd::kMmcSwitch;
    cmd.arg = (static_cast<uint32_t>(emmc::extcsd::kSwitchModeWriteByte)
               << 24U)
              | (static_cast<uint32_t>(index) << 16U)
              | (static_cast<uint32_t>(value) << 8U);
    cmd.flags = MMC_RSP_R1B | MMC_CMD_AC;
    cmd.write_flag = 1;

    if (::ioctl(h.fd, MMC_IOC_CMD, &cmd) != 0)
    {
        std::cerr << std::format(
            "emmc-ioctl: MMC_IOC_CMD SWITCH({}, idx={}, val={}) failed: {}\n",
            dev_z, index, value, std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace emmc::ioctl
