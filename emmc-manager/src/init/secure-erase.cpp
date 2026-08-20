// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-config.h"
#include "emmc-init.h"
#include "emmc-ioctl.h"
#include "emmc-util.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace emmc::init
{

namespace
{

// 4 MiB chunks balance memory vs. syscall count on the 8 MiB initramfs.
constexpr size_t kCopyChunkBytes = 4U * 1024U * 1024U;

ssize_t readEintr(int fd, void* buf, size_t n)
{
    while (true)
    {
        ssize_t r = ::read(fd, buf, n);
        if (r < 0 && errno == EINTR)
        {
            continue;
        }
        return r;
    }
}

bool writeAllEintr(int fd, const void* buf, size_t n, const char* path)
{
    const auto* p = static_cast<const uint8_t*>(buf);
    while (n > 0)
    {
        ssize_t w = ::write(fd, p, n);
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::cerr << std::format("secure-erase: write({}) failed: {}\n",
                                     path, std::strerror(errno));
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

// EINTR-tolerant block-image copy with fsync on close. Both fds use the
// emmc::util::Fd RAII wrapper so partial paths can return without
// leaking descriptors.
bool copyBlockImage(std::string_view src, std::string_view dst)
{
    std::string src_z(src);
    std::string dst_z(dst);

    emmc::util::Fd sfd(::open(src_z.c_str(), O_RDONLY | O_CLOEXEC));
    if (!sfd)
    {
        std::cerr << std::format("secure-erase: open({}) failed: {}\n", src_z,
                                 std::strerror(errno));
        return false;
    }
    emmc::util::Fd dfd(
        ::open(dst_z.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
    if (!dfd)
    {
        std::cerr << std::format("secure-erase: open({}) failed: {}\n", dst_z,
                                 std::strerror(errno));
        return false;
    }

    // The 4 MiB chunk is large relative to the 8 MiB initramfs budget;
    // bad_alloc here is recoverable — the caller treats backup failure
    // as non-fatal and continues with an empty rwfs.
    std::vector<uint8_t> buf;
    try
    {
        buf.resize(kCopyChunkBytes);
    }
    catch (const std::bad_alloc& e)
    {
        std::cerr << std::format(
            "secure-erase: failed to allocate {}B chunk buffer: {}\n",
            kCopyChunkBytes, e.what());
        return false;
    }

    bool ok = true;
    while (ok)
    {
        ssize_t n = readEintr(sfd.get(), buf.data(), buf.size());
        if (n < 0)
        {
            std::cerr << std::format("secure-erase: read({}) failed: {}\n",
                                     src_z, std::strerror(errno));
            ok = false;
            break;
        }
        if (n == 0)
        {
            break; // EOF
        }
        if (!writeAllEintr(dfd.get(), buf.data(), static_cast<size_t>(n),
                           dst_z.c_str()))
        {
            ok = false;
            break;
        }
    }
    if (ok && ::fsync(dfd.get()) != 0)
    {
        std::cerr << std::format("secure-erase: fsync({}) failed: {}\n", dst_z,
                                 std::strerror(errno));
        ok = false;
    }
    // Fd destructors close sfd / dfd on scope exit.
    return ok;
}

} // namespace

bool backupRwfs(std::string_view dest)
{
    const std::string rwdev = rwfsDevicePath();
    if (!ioctl::isBlockDevice(rwdev))
    {
        std::cerr << "secure-erase: rwfs partition not present; nothing to "
                     "back up\n";
        return true;
    }
    if (copyBlockImage(rwdev, dest))
    {
        return true;
    }
    // The destination file was created and partially written before the
    // failure — leaving it on disk would let a later restoreRwfs() copy a
    // truncated/corrupt image back into rwfs. Drop it so restore reliably
    // sees "no backup" and skips.
    std::error_code ec;
    const std::filesystem::path dest_path{dest};
    if (std::filesystem::remove(dest_path, ec))
    {
        std::cerr << std::format(
            "secure-erase: removed partial backup image {}\n", dest);
    }
    else if (ec)
    {
        std::cerr << std::format(
            "secure-erase: failed to remove partial backup image {}: {}\n",
            dest, ec.message());
    }
    return false;
}

bool restoreRwfs(std::string_view src, std::string_view rwdev)
{
    std::error_code ec;
    if (!std::filesystem::exists(src, ec) || !ioctl::isBlockDevice(rwdev))
    {
        return true; // nothing to restore
    }
    bool ok = copyBlockImage(src, rwdev);
    std::cerr << std::format("secure-erase: rwfs restore {}\n",
                             ok ? "completed" : "failed");
    return ok;
}

} // namespace emmc::init
