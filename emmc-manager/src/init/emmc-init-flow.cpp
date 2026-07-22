// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-config.h"
#include "emmc-format.h"
#include "emmc-init.h"
#include "emmc-ioctl.h"
#include "emmc-uenv.h"
#include "emmc-util.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <string_view>

namespace emmc::init
{

namespace
{

// BLKRRPART after secure-erase so the kernel drops its cached partition
// table; without this createPartitions() short-circuits on stale
// /sys/block nodes and the on-disk MBR stays empty (next cold boot has
// no partitions to enumerate).
void wipeWholeDevice(std::string_view device, uint64_t sectors)
{
    if (!ioctl::secureEraseRange(device, 0, sectors - 1, 3))
    {
        std::cerr << "nvidia-emmc-init: secure-erase failed\n";
        return;
    }
    ioctl::triggerPartitionTableReread(device);
}

// Returns true if rwfs_recovery is set — force-format rwfs regardless of
// its current state.
bool consumeRwfsRecovery()
{
    if (uenv::getCounter("rwfs_recovery") <= 0)
    {
        return false;
    }
    std::cerr << "nvidia-emmc-init: rwfs_recovery set — will reformat rwfs "
                 "after partition create\n";
    return true;
}

// openbmconce=factory-reset (or clean-rwfs-filesystem) — force an rwfs
// reformat. Still set in the env during initramfs; clear-once.service
// clears openbmconce later, so we don't touch it here.
bool consumeFactoryReset()
{
    auto once = uenv::get("openbmconce");
    if (!once)
    {
        return false;
    }
    // openbmconce is a space-separated list; word-match like `grep -w`.
    auto hasToken = [](std::string_view s, std::string_view t) {
        for (auto p = s.find(t); p != std::string_view::npos;
             p = s.find(t, p + 1))
        {
            bool left = (p == 0) || s[p - 1] == ' ';
            auto end = p + t.size();
            bool right = (end == s.size()) || s[end] == ' ';
            if (left && right)
            {
                return true;
            }
        }
        return false;
    };
    if (hasToken(*once, "factory-reset") ||
        hasToken(*once, "clean-rwfs-filesystem"))
    {
        std::cerr << std::format(
            "nvidia-emmc-init: openbmconce='{}' requests factory reset — "
            "will reformat rwfs\n",
            *once);
        return true;
    }
    return false;
}

// emmc_secure_erase: back up rwfs, secure-erase the whole device, leave a
// backup image at kRwfsBackupImage for step 8 to restore from.
void consumeSecureErase(std::string_view device, uint64_t sectors)
{
    auto flag = uenv::get("emmc_secure_erase");
    if (!flag || *flag != "yes")
    {
        return;
    }
    std::cerr << "nvidia-emmc-init: emmc_secure_erase=yes — backing up "
                 "rwfs and secure-erasing whole device\n";
    if (!backupRwfs(kRwfsBackupImage))
    {
        std::cerr << "nvidia-emmc-init: rwfs backup failed; rwfs will be "
                     "wiped without restore\n";
    }
    wipeWholeDevice(device, sectors);
    uenv::clear("emmc_secure_erase");
}

// emmc_full_secure_erase: destructive wipe, no backup/restore.
void consumeFullSecureErase(std::string_view device, uint64_t sectors)
{
    auto flag = uenv::get("emmc_full_secure_erase");
    if (!flag || *flag != "yes")
    {
        return;
    }
    std::cerr << "nvidia-emmc-init: emmc_full_secure_erase=yes — wiping "
                 "whole device including rwfs\n";
    wipeWholeDevice(device, sectors);
    uenv::clear("emmc_full_secure_erase");
}

// emmc_repartition: forced repartition path triggered by the monitor's
// layout-mismatch handler.
void consumeRepartition(std::string_view device, uint64_t sectors)
{
    auto flag = uenv::get("emmc_repartition");
    if (!flag || flag->empty())
    {
        return;
    }
    std::cerr << std::format(
        "nvidia-emmc-init: emmc_repartition={} — secure-erasing for fresh "
        "layout\n",
        *flag);
    wipeWholeDevice(device, sectors);
}

int programEuda(std::string_view device)
{
    switch (eudaConfigWithHwReset(device))
    {
        case EudaResult::Ok:
            return kExitOk;
        case EudaResult::Failed:
            std::cerr << "nvidia-emmc-init: EUDA programming failed\n";
            return kExitEudaFailed;
        case EudaResult::NotActivated:
            std::cerr << "nvidia-emmc-init: EUDA not activated after HW "
                         "reset retries\n";
            return kExitEudaNotActivated;
    }
    return kExitEudaFailed; // unreachable
}

// Format rwfs if forced (rwfs_recovery) or if its superblock is missing.
int formatRwfsIfNeeded(std::string_view rwdev, bool force)
{
    bool need_format = force || !format::validateExt4OrRecover(rwdev);
    if (!need_format)
    {
        return kExitOk;
    }
    if (!format::formatPartition(rwdev))
    {
        std::cerr << "nvidia-emmc-init: rwfs format failed\n";
        return kExitRwfsFormat;
    }
    if (force)
    {
        uenv::clear("rwfs_recovery");
    }
    return kExitOk;
}

void restoreRwfsFromBackupIfPresent(std::string_view rwdev)
{
    std::error_code ec;
    if (!std::filesystem::exists(kRwfsBackupImage, ec))
    {
        return;
    }
    std::cerr << "nvidia-emmc-init: restoring rwfs from backup image\n";
    if (!restoreRwfs(kRwfsBackupImage, rwdev))
    {
        std::cerr << "nvidia-emmc-init: rwfs restore failed; continuing "
                     "with empty rwfs\n";
    }
    std::filesystem::remove(kRwfsBackupImage, ec);
}

// Last-resort reformat if rwfs's ext4 is still unrecoverable after
// validate/restore.
int validateOrLastResortReformat(std::string_view rwdev)
{
    if (format::validateExt4OrRecover(rwdev))
    {
        return kExitOk;
    }
    std::cerr << "nvidia-emmc-init: rwfs ext4 unrecoverable — last-resort "
                 "reformat\n";
    if (!format::formatPartition(rwdev))
    {
        return kExitRwfsFormat;
    }
    return kExitOk;
}

} // namespace

int run(std::string_view device)
{
    auto disk = ioctl::readDiskGeometry(device);
    if (!disk)
    {
        std::cerr << std::format(
            "nvidia-emmc-init: device {} not found / unreadable\n", device);
        return kExitEmmcMissing;
    }

    const std::string rwdev =
        emmc::util::partDevice(device, emmc::layout::rwfsPartitionNum());

    const bool force_format_rwfs = consumeRwfsRecovery() || consumeFactoryReset();
    consumeSecureErase(device, disk->sectors);
    consumeFullSecureErase(device, disk->sectors);
    consumeRepartition(device, disk->sectors);

    if (int rc = programEuda(device); rc != kExitOk)
    {
        return rc;
    }

    if (!createPartitions(device, disk->sectors - 1))
    {
        return kExitPartitionCreate;
    }

    if (int rc = formatRwfsIfNeeded(rwdev, force_format_rwfs); rc != kExitOk)
    {
        return rc;
    }

    restoreRwfsFromBackupIfPresent(rwdev);

    if (int rc = validateOrLastResortReformat(rwdev); rc != kExitOk)
    {
        return rc;
    }

    // Mount rwfs at /run/initramfs/rw. Failure here is non-fatal — the
    // shell flow falls back to a RAM-only overlay.
    if (!format::mountFs(rwdev, kRwfsMount))
    {
        std::cerr << "nvidia-emmc-init: rwfs mount failed; obmc-init.sh "
                     "will continue with RAM-only overlay\n";
    }

    return kExitOk;
}

} // namespace emmc::init
