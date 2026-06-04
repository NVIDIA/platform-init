// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA
//
// Generic MBR + EBR writer driven by emmc::layout::kPartitions (supplied
// per-machine via emmc-layout-machine.h). Handles any layout meeting the
// schema constraints checked in emmc-layout-validate.h:
//   - up to 4 Primary partitions, filling MBR slots [0..n_primary)
//   - up to 1 Extended partition, occupying slot [n_primary]
//   - 0..N Logical partitions, chained via EBRs inside the Extended
//
// Sector size = 512 B. Unallocated tail inside the extended container is
// allowed — the layout's logical sizes need not fill it. CHS (H=4, S=16)
// is hard-coded to match what `fdisk -l` shows on the legacy layout;
// LBAs above C=1023 saturate to (1023, max_h, max_s).

#include "emmc-config.h"
#include "emmc-init.h"
#include "emmc-ioctl.h"
#include "emmc-util.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

namespace emmc::init
{

namespace
{

// Track-aligned start + EBR overhead per legacy fdisk geometry.
constexpr uint32_t kStartLba = 16;
constexpr uint32_t kEbrOverhead = 16;

// Upper bound for stack-allocated per-partition arrays. kPartitions.size()
// is a compile-time constant from the layout.
constexpr std::size_t kMaxParts = emmc::layout::kPartitions.size();

constexpr uint8_t kChsHeads = 4;
constexpr uint8_t kChsSectors = 16; // 1..16

#pragma pack(push, 1)
struct MbrEntry
{
    uint8_t status;       // 0x00 / 0x80
    uint8_t chs_first[3];
    uint8_t type;         // 0x83 Linux, 0x05 extended
    uint8_t chs_last[3];
    uint32_t lba_first;   // little endian
    uint32_t lba_count;
};

struct Mbr
{
    uint8_t bootcode[446];
    MbrEntry entries[4];
    uint16_t signature; // 0xaa55
};
#pragma pack(pop)

static_assert(sizeof(MbrEntry) == 16);
static_assert(sizeof(Mbr) == 512);

void encodeChs(uint32_t lba, uint8_t out[3])
{
    const uint32_t spc = static_cast<uint32_t>(kChsHeads) * kChsSectors;
    uint32_t c = lba / spc;
    uint32_t h = (lba / kChsSectors) % kChsHeads;
    uint32_t s = (lba % kChsSectors) + 1;
    if (c > 1023U)
    {
        c = 1023U;
        h = kChsHeads - 1U;
        s = kChsSectors;
    }
    out[0] = static_cast<uint8_t>(h);
    out[1] = static_cast<uint8_t>(((c >> 2) & 0xC0) | (s & 0x3F));
    out[2] = static_cast<uint8_t>(c & 0xFF);
}

// `lba_first` is what's written to the entry (relative-to-extended for
// EBR chain entries, absolute otherwise). `abs_first` is the actual
// on-disk LBA used to compute CHS bytes.
void setEntry(MbrEntry& e, uint8_t type, uint32_t lba_first, uint32_t count,
              uint32_t abs_first)
{
    e.status = 0;
    e.type = type;
    e.lba_first = lba_first;
    e.lba_count = count;
    encodeChs(abs_first, e.chs_first);
    encodeChs(abs_first + count - 1U, e.chs_last);
}

void setEntry(MbrEntry& e, uint8_t type, uint32_t lba_first, uint32_t count)
{
    setEntry(e, type, lba_first, count, lba_first);
}

bool writeSector(int fd, uint64_t sector, const void* buf, size_t len)
{
    off_t off = static_cast<off_t>(sector) * 512;
    ssize_t w = ::pwrite(fd, buf, len, off);
    if (w != static_cast<ssize_t>(len))
    {
        std::cerr << std::format(
            "partition: pwrite(@sector {}) failed ({}/{}): {}\n", sector, w,
            len, std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace

bool createPartitions(std::string_view device, uint64_t total_sectors)
{
    using emmc::layout::EmmcPart;

    // Classify the configured layout into MBR-slot occupants.
    std::array<const EmmcPart*, kMaxParts> primaries{};
    std::array<const EmmcPart*, kMaxParts> logicals{};
    const EmmcPart* extended = nullptr;
    std::size_t n_primary = 0;
    std::size_t n_logical = 0;
    for (auto const& p : emmc::layout::kPartitions)
    {
        switch (p.type)
        {
            case EmmcPart::Type::Primary:
                primaries[n_primary++] = &p;
                break;
            case EmmcPart::Type::Extended:
                extended = &p;
                break;
            case EmmcPart::Type::Logical:
                logicals[n_logical++] = &p;
                break;
        }
    }

    // Idempotent: skip only if *every* Primary and Logical node from the
    // configured layout is already present in sysfs (the Extended is a
    // metadata-only MBR slot and has no node). Matches the legacy
    // obmc-emmc.sh::check_partition contract — declaring "exists" only
    // when the MBR *and* the full EBR chain have landed and the kernel
    // has populated each partition node. Checking only primaries[0]
    // would short-circuit after a partial write that wrote the MBR but
    // not all EBRs, leaving p5..p7 missing.
    //
    // base_name derived from `device` (basename of "/dev/mmcblkN") rather
    // than hardcoded so this stays correct on any future eMMC device name.
    std::error_code ec;
    std::string_view base_name = device;
    if (auto slash = base_name.rfind('/'); slash != std::string_view::npos)
    {
        base_name.remove_prefix(slash + 1);
    }
    bool all_present = true;
    for (auto const& p : emmc::layout::kPartitions)
    {
        if (p.type == EmmcPart::Type::Extended)
        {
            continue;
        }
        const std::string sysfs = std::format("/sys/block/{}/{}p{}/size",
                                              base_name, base_name, p.num);
        if (!std::filesystem::exists(sysfs, ec))
        {
            std::cerr << std::format(
                "partition: sysfs node {} missing; full layout will be "
                "written\n",
                sysfs);
            all_present = false;
            break;
        }
    }
    if (all_present)
    {
        std::cerr
            << "partition: existing layout detected; skipping repartition\n";
        return true;
    }

    // Total sectors needed = first primary's size + remaining primaries +
    // (each logical + its EBR overhead).
    uint64_t needed = primaries[0]->sectors;
    for (std::size_t i = 1; i < n_primary; ++i)
    {
        needed += primaries[i]->sectors;
    }
    for (std::size_t i = 0; i < n_logical; ++i)
    {
        needed += kEbrOverhead + logicals[i]->sectors;
    }
    if (total_sectors < kStartLba + needed)
    {
        std::cerr << "partition: disk too small for the configured layout\n";
        return false;
    }

    // Compute on-disk start LBAs for each primary.
    std::array<uint32_t, kMaxParts> p_start{};
    p_start[0] = kStartLba;
    for (std::size_t i = 1; i < n_primary; ++i)
    {
        p_start[i] = p_start[i - 1]
                     + static_cast<uint32_t>(primaries[i - 1]->sectors);
    }

    // Extended container starts after the last primary and fills the rest
    // of the disk. Required when any Logical exists (schema guarantees
    // exactly one Extended is present).
    const uint32_t ext_start =
        p_start[n_primary - 1]
        + static_cast<uint32_t>(primaries[n_primary - 1]->sectors);
    const uint32_t ext_size = static_cast<uint32_t>(total_sectors - ext_start);

    // Build the MBR: primaries fill [0..n_primary), extended occupies
    // [n_primary]. Remaining slots stay zero.
    Mbr mbr{};
    for (std::size_t i = 0; i < n_primary; ++i)
    {
        setEntry(mbr.entries[i], 0x83, p_start[i],
                 static_cast<uint32_t>(primaries[i]->sectors));
    }
    if (extended != nullptr)
    {
        setEntry(mbr.entries[n_primary], 0x05, ext_start, ext_size);
    }
    mbr.signature = 0xAA55U;

    // Build the EBR chain. Each EBR sits at ebr_lba[i] on disk; its
    // entries[0] describes that logical's data (lba_first relative to
    // its own EBR = kEbrOverhead). entries[1] chains to the next EBR
    // (lba_first relative to the Extended container start). The last
    // EBR's entries[1] stays zeroed → end of chain.
    std::array<Mbr, kMaxParts> ebrs{};
    std::array<uint32_t, kMaxParts> ebr_lba{};
    if (n_logical > 0)
    {
        ebr_lba[0] = ext_start;
        for (std::size_t i = 1; i < n_logical; ++i)
        {
            ebr_lba[i] = ebr_lba[i - 1] + kEbrOverhead
                         + static_cast<uint32_t>(logicals[i - 1]->sectors);
        }

        for (std::size_t i = 0; i < n_logical; ++i)
        {
            const uint32_t data_lba = ebr_lba[i] + kEbrOverhead;
            setEntry(ebrs[i].entries[0], 0x83, kEbrOverhead,
                     static_cast<uint32_t>(logicals[i]->sectors), data_lba);
            if (i + 1 < n_logical)
            {
                const uint32_t next_ebr_rel = ebr_lba[i + 1] - ext_start;
                uint32_t remaining = 0;
                for (std::size_t j = i + 1; j < n_logical; ++j)
                {
                    remaining += kEbrOverhead
                                 + static_cast<uint32_t>(logicals[j]->sectors);
                }
                setEntry(ebrs[i].entries[1], 0x05, next_ebr_rel, remaining,
                         ebr_lba[i + 1]);
            }
            ebrs[i].signature = 0xAA55U;
        }
    }

    // Write MBR and each EBR. fsync once at the end. Fd is closed by RAII
    // on every return path below.
    std::string dev_z(device);
    emmc::util::Fd fd(::open(dev_z.c_str(), O_RDWR | O_CLOEXEC));
    if (!fd)
    {
        std::cerr << std::format("partition: open({}) RW failed: {}\n",
                                 dev_z, std::strerror(errno));
        return false;
    }

    bool ok = writeSector(fd.get(), 0, &mbr, sizeof(mbr));
    for (std::size_t i = 0; ok && i < n_logical; ++i)
    {
        ok = writeSector(fd.get(), ebr_lba[i], &ebrs[i], sizeof(ebrs[i]));
    }
    if (ok)
    {
        ok = (::fsync(fd.get()) == 0);
    }

    if (!ok)
    {
        return false;
    }

    if (!ioctl::triggerPartitionTableReread(device))
    {
        std::cerr << "partition: BLKRRPART failed after MBR write\n";
        return false;
    }
    return true;
}

} // namespace emmc::init
