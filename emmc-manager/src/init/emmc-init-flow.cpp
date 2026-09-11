// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-config.h"
#include "emmc-format.h"
#include "emmc-init.h"
#include "emmc-ioctl.h"
#include "emmc-layout-ondisk.h"
#include "emmc-uenv.h"
#include "emmc-util.h"

#include <cctype>
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

constexpr std::string_view kUserLogsMount = "/var/emmc/user-logs";

bool hasEnvToken(std::string_view value, std::string_view token)
{
    std::size_t pos = 0;
    while (pos < value.size())
    {
        while (pos < value.size() &&
               std::isspace(static_cast<unsigned char>(value[pos])) != 0)
        {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < value.size() &&
               std::isspace(static_cast<unsigned char>(value[pos])) == 0)
        {
            ++pos;
        }
        if (value.substr(start, pos - start) == token)
        {
            return true;
        }
    }
    return false;
}

std::string partitionDeviceForMount(std::string_view device,
                                    std::string_view mount_point)
{
    for (const auto& p : emmc::layout::kPartitions)
    {
        if (p.mount != nullptr && std::string_view(p.mount) == mount_point)
        {
            return emmc::util::partDevice(device, p.num);
        }
    }
    return {};
}

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

void logOnceResetRequest(const OnceResetRequest& request,
                         std::string_view openbmconce)
{
    if (request.complete_reset)
    {
        std::cerr << std::format(
            "nvidia-emmc-init: openbmconce='{}' requests complete reset; "
            "will reformat rwfs and user logs\n",
            openbmconce);
        return;
    }
    if (request.factory_reset || request.clean_rwfs)
    {
        std::cerr << std::format(
            "nvidia-emmc-init: openbmconce='{}' requests factory reset; "
            "will reformat rwfs\n",
            openbmconce);
    }
}

OnceResetRequest readOnceResetRequest()
{
    auto once = uenv::get("openbmconce");
    if (!once)
    {
        return {};
    }

    OnceResetRequest request = parseOpenbmconce(*once);
    logOnceResetRequest(request, *once);
    return request;
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

// Erase when the table on the device disagrees with the configured layout,
// so that createPartitions() below writes a fresh one in *this* boot.
//
// Without this, a device carrying a complete table of the wrong geometry
// (an older layout, or one written by another image on the alternate flash
// slot) was accepted here and rejected by the monitor, which set
// emmc_repartition and rebooted. That only converged if the next boot ran
// the same image; otherwise it repeated indefinitely, erasing the eMMC on
// every cycle. Fixing the geometry here costs the same erase but no reboot,
// and terminates regardless of which image boots next.
//
// Only Mismatch — a partition that is present and carries a size the layout
// does not allow — is erased. Absent and Incomplete are left to
// createPartitions(), which writes the table without erasing, as it always
// has. That keeps a missing sysfs node, which can be a partial write but can
// equally be an enumeration timing artefact, from costing the user their
// eMMC contents.
void repartitionIfLayoutMismatch(std::string_view device, uint64_t sectors)
{
    if (emmc::layout::onDiskState(device) !=
        emmc::layout::OnDiskState::Mismatch)
    {
        return;
    }
    std::cerr << "nvidia-emmc-init: on-disk layout does not match the "
                 "configured geometry — secure-erasing for a fresh layout\n";
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

int formatUserLogsForCompleteReset(std::string_view device)
{
    std::string logdev = partitionDeviceForMount(device, kUserLogsMount);
    if (logdev.empty())
    {
        std::cerr << std::format(
            "nvidia-emmc-init: complete reset requested but no partition "
            "maps to {}\n",
            kUserLogsMount);
        return kExitRwfsFormat;
    }
    if (!format::formatPartition(logdev))
    {
        std::cerr << std::format(
            "nvidia-emmc-init: user log partition format failed: {}\n", logdev);
        return kExitRwfsFormat;
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

OnceResetRequest parseOpenbmconce(std::string_view value)
{
    OnceResetRequest request{};
    request.clean_rwfs = hasEnvToken(value, "clean-rwfs-filesystem");
    request.factory_reset = hasEnvToken(value, "factory-reset");
    request.complete_reset = hasEnvToken(value, "complete-reset");
    return request;
}

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

    const OnceResetRequest once_reset = readOnceResetRequest();
    const bool force_format_rwfs =
        consumeRwfsRecovery() || once_reset.formatsRwfs();
    consumeSecureErase(device, disk->sectors);
    consumeFullSecureErase(device, disk->sectors);
    consumeRepartition(device, disk->sectors);
    repartitionIfLayoutMismatch(device, disk->sectors);

    if (int rc = programEuda(device); rc != kExitOk)
    {
        return rc;
    }

    if (!createPartitions(device, disk->sectors - 1))
    {
        return kExitPartitionCreate;
    }

    if (once_reset.complete_reset)
    {
        if (int rc = formatUserLogsForCompleteReset(device); rc != kExitOk)
        {
            return rc;
        }
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
