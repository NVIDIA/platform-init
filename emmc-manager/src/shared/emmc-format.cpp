// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-format.h"

#include "emmc-ioctl.h"

#include <sys/mount.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>

extern "C"
{
#include <com_err.h>
#include <e2p/e2p.h>
#include <ext2fs/ext2fs.h>
}

namespace
{

// Register libext2fs's error-message table with libcom_err on TU load so
// error_message(EXT2_ET_*) returns the canonical string instead of
// "Unknown code ext2 NNN".
[[maybe_unused]] const int kInitExt2ErrorTable = []() {
    initialize_ext2_error_table();
    return 0;
}();

std::string fmtRc(errcode_t rc)
{
    return std::format("{} (errcode {})", error_message(rc),
                       static_cast<long>(rc));
}

} // namespace

namespace emmc::format
{

namespace
{

// ext4 defaults matching `mke2fs -F -t ext4` on poky 1.47.x.
constexpr unsigned int kBlockLogSize = 2; // 4 KiB blocks
constexpr unsigned int kInodeSize = 256;
constexpr double kReservedRatio = 0.05;

void setupParams(struct ext2_super_block& param, uint64_t size_bytes,
                 uint64_t block_bytes)
{
    param.s_log_block_size = kBlockLogSize;
    param.s_log_cluster_size = kBlockLogSize;
    param.s_inode_size = kInodeSize;
    param.s_rev_level = EXT2_DYNAMIC_REV;
    param.s_feature_compat = EXT3_FEATURE_COMPAT_HAS_JOURNAL;
    param.s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE
                               | EXT3_FEATURE_INCOMPAT_EXTENTS;
    param.s_feature_ro_compat = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER
                                | EXT2_FEATURE_RO_COMPAT_LARGE_FILE;
    param.s_blocks_count = static_cast<__u32>(size_bytes / block_bytes);
    param.s_first_data_block = 0;
}

// Write a zeroed inode struct into each reserved slot (1..s_first_ino-1)
// and mark them in-use. Mirrors mke2fs's write_reserved_inodes() +
// reserve_inodes(). MUST run before ext2fs_mkdir(root) — mkdir overwrites
// inode 2's slot with the real root inode after this zero-pass.
//
// Without the zero-write, inode 1 can have stray EXT4_INLINE_DATA_FL bits
// from leftover device contents, which makes ext2fs_block_iterate2()
// (called by ext2fs_update_bb_inode) return EXT2_ET_INLINE_DATA_CANT_ITERATE.
// Without the bitmap mark, the kernel allocator later picks inode 1 and
// the mount fails with "reserved inode found cleared".
errcode_t zeroAndReserveInodes(ext2_filsys fs, std::string_view dev_z)
{
    struct ext2_inode* zeroed = nullptr;
    errcode_t rc = ext2fs_get_memzero(EXT2_INODE_SIZE(fs->super), &zeroed);
    if (rc != 0)
    {
        std::cerr << std::format(
            "emmc-format: ext2fs_get_memzero({}) failed: {}\n", dev_z,
            fmtRc(rc));
        return rc;
    }
    for (ext2_ino_t i = 1; i < EXT2_FIRST_INODE(fs->super); ++i)
    {
        rc = ext2fs_write_inode_full(fs, i, zeroed,
                                     EXT2_INODE_SIZE(fs->super));
        if (rc != 0)
        {
            std::cerr << std::format(
                "emmc-format: ext2fs_write_inode_full(inode {}, {}) failed: "
                "{}\n",
                i, dev_z, fmtRc(rc));
            ext2fs_free_mem(&zeroed);
            return rc;
        }
        ext2fs_inode_alloc_stats2(fs, i, /*inuse=*/+1, /*isdir=*/0);
    }
    ext2fs_free_mem(&zeroed);
    ext2fs_mark_ib_dirty(fs);
    return 0;
}

// Add an internal journal (s_journal_inum + journal_data mount opt), or
// drop HAS_JOURNAL for partitions too small to hold the tiered mke2fs
// default journal size (e.g. p6 = 1 MiB).
errcode_t addJournalOrSkip(ext2_filsys fs, std::string_view dev_z)
{
    int journal_blocks =
        ext2fs_default_journal_size(fs->super->s_blocks_count);
    if (journal_blocks <= 0)
    {
        std::cerr << std::format(
            "emmc-format: {} ({} blocks) too small for journal; formatting "
            "without HAS_JOURNAL\n",
            dev_z, fs->super->s_blocks_count);
        fs->super->s_feature_compat &= ~EXT3_FEATURE_COMPAT_HAS_JOURNAL;
        return 0;
    }

    struct ext2fs_journal_params jparams{};
    jparams.num_journal_blocks = journal_blocks;
    errcode_t rc =
        ext2fs_add_journal_inode3(fs, &jparams, /*goal=*/0, /*flags=*/0);
    if (rc != 0)
    {
        std::cerr << std::format(
            "emmc-format: ext2fs_add_journal_inode3({}, {} blocks) failed: "
            "{}\n",
            dev_z, journal_blocks, fmtRc(rc));
        return rc;
    }
    if (e2p_edit_mntopts("journal_data", &fs->super->s_default_mount_opts,
                         ~0U) != 0)
    {
        // Non-fatal — tune2fs treats this as a warning.
        std::cerr << "emmc-format: e2p_edit_mntopts(journal_data) failed\n";
    }
    return 0;
}

bool runFormat(std::string_view dev)
{
    std::string dev_z(dev);
    auto geo = emmc::ioctl::readDiskGeometry(dev);
    if (!geo)
    {
        std::cerr << std::format(
            "emmc-format: BLKGETSIZE64({}) failed; cannot size filesystem\n",
            dev_z);
        return false;
    }
    const uint64_t block_bytes = 1024ULL << kBlockLogSize;

    struct ext2_super_block param{};
    setupParams(param, geo->size_bytes, block_bytes);

    ext2_filsys fs = nullptr;
    errcode_t rc = ext2fs_initialize(
        dev_z.c_str(), EXT2_FLAG_EXCLUSIVE | EXT2_FLAG_RW, &param,
        unix_io_manager, &fs);
    if (rc != 0)
    {
        std::cerr << std::format(
            "emmc-format: ext2fs_initialize({}) failed: {}\n", dev_z,
            fmtRc(rc));
        return false;
    }

    std::strncpy(reinterpret_cast<char*>(fs->super->s_volume_name), "rwfs",
                 sizeof(fs->super->s_volume_name) - 1);
    fs->super->s_r_blocks_count = static_cast<__u32>(
        static_cast<double>(fs->super->s_blocks_count) * kReservedRatio);

    rc = ext2fs_allocate_tables(fs);
    if (rc != 0)
    {
        std::cerr << std::format(
            "emmc-format: ext2fs_allocate_tables({}) failed: {}\n", dev_z,
            fmtRc(rc));
        ext2fs_close_free(&fs);
        return false;
    }

    rc = zeroAndReserveInodes(fs, dev_z);
    if (rc != 0)
    {
        ext2fs_close_free(&fs);
        return false;
    }

    rc = ext2fs_mkdir(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, nullptr);
    if (rc != 0 && rc != EXT2_ET_DIR_EXISTS)
    {
        std::cerr << std::format(
            "emmc-format: ext2fs_mkdir(root, {}) failed: {}\n", dev_z,
            fmtRc(rc));
        ext2fs_close_free(&fs);
        return false;
    }

    // Initialise inode 1 as an empty bad-blocks file (mke2fs does this
    // unconditionally so the slot is a valid empty-extent file).
    rc = ext2fs_update_bb_inode(fs, /*bb_list=*/nullptr);
    if (rc != 0)
    {
        std::cerr << std::format(
            "emmc-format: ext2fs_update_bb_inode({}) failed: {}\n", dev_z,
            fmtRc(rc));
        ext2fs_close_free(&fs);
        return false;
    }

    rc = addJournalOrSkip(fs, dev_z);
    if (rc != 0)
    {
        ext2fs_close_free(&fs);
        return false;
    }

    ext2fs_mark_super_dirty(fs);
    rc = ext2fs_close_free(&fs);
    if (rc != 0)
    {
        std::cerr << std::format(
            "emmc-format: ext2fs_close_free({}) failed: {}\n", dev_z,
            fmtRc(rc));
        return false;
    }
    return true;
}

} // namespace

bool formatPartition(std::string_view dev)
{
    std::cerr << std::format("emmc-format: formatting {}\n", dev);
    return runFormat(dev);
}

bool validateExt4OrRecover(std::string_view dev)
{
    std::string dev_z(dev);
    ext2_filsys fs = nullptr;
    errcode_t rc = ext2fs_open(dev_z.c_str(), EXT2_FLAG_RW, /*superblock=*/0,
                               /*block_size=*/0, unix_io_manager, &fs);
    if (rc == 0)
    {
        ext2fs_close_free(&fs);
        return true;
    }

    if (rc == EXT2_ET_BAD_MAGIC || rc == EXT2_ET_SB_CSUM_INVALID)
    {
        // Backup superblock locations for an ext4 filesystem with 4 KiB
        // blocks (blocks_per_group = 32768). Copies live at block 0 of
        // groups 1, 3, 5, 7, 9; each block number is group_num * 32768
        // + 1 (the +1 follows the ext2fs_open convention used when
        // passing a non-primary superblock address). The previous
        // values {8193, 24577, …} were for 1 KiB-block filesystems
        // (blocks_per_group = 8192); they never matched our 4 KiB
        // layout — formatPartition sets s_log_block_size = 2.
        for (uint32_t sb : {32769U, 98305U, 163841U, 229377U, 294913U})
        {
            if (ext2fs_open(dev_z.c_str(), EXT2_FLAG_RW, sb, 4096,
                            unix_io_manager, &fs) == 0)
            {
                ext2fs_mark_super_dirty(fs);
                bool ok = (ext2fs_close_free(&fs) == 0);
                std::cerr << std::format(
                    "emmc-format: recovered {} from backup superblock {}\n",
                    dev_z, sb);
                return ok;
            }
        }
    }
    std::cerr << std::format(
        "emmc-format: validateExt4OrRecover({}) — unrecoverable: {}\n",
        dev_z, fmtRc(rc));
    return false;
}

bool isMounted(std::string_view mount_point)
{
    std::ifstream mounts("/proc/mounts");
    if (!mounts)
    {
        return false;
    }
    std::string mp(mount_point);
    std::string line;
    while (std::getline(mounts, line))
    {
        // <dev> <mountpoint> <fstype> <opts> 0 0
        size_t first_sp = line.find(' ');
        if (first_sp == std::string::npos)
        {
            continue;
        }
        size_t second_sp = line.find(' ', first_sp + 1);
        if (second_sp == std::string::npos)
        {
            continue;
        }
        std::string_view this_mp(line.data() + first_sp + 1,
                                 second_sp - first_sp - 1);
        if (this_mp == mp)
        {
            return true;
        }
    }
    return false;
}

bool mountFs(std::string_view dev, std::string_view mount_point)
{
    std::error_code ec;
    std::filesystem::create_directories(mount_point, ec);
    if (ec)
    {
        std::cerr << std::format("emmc-format: mkdir({}) failed: {}\n",
                                 mount_point, ec.message());
    }

    std::string dev_z(dev);
    std::string mp_z(mount_point);
    if (::mount(dev_z.c_str(), mp_z.c_str(), "ext4", 0, nullptr) != 0)
    {
        std::cerr << std::format(
            "emmc-format: mount({} -> {}) failed: {}\n", dev_z, mp_z,
            std::strerror(errno));
        return false;
    }
    if (!isMounted(mount_point))
    {
        std::cerr << std::format(
            "emmc-format: mount({} -> {}) reported success but {} is not "
            "in /proc/mounts\n",
            dev_z, mp_z, mp_z);
        return false;
    }
    return true;
}

bool unmount(std::string_view mount_point)
{
    std::string mp(mount_point);
    // Synchronous unmount (flags=0) — the caller (reformat / secure-erase
    // path) needs the filesystem fully unmounted before touching the
    // block device. MNT_DETACH would defer teardown while the mount is
    // busy and let us proceed against a still-mapped fs.
    if (::umount2(mp.c_str(), 0) != 0)
    {
        std::cerr << std::format("emmc-format: umount2({}) failed: {}\n", mp,
                                 std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace emmc::format
