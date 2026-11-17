// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA
//
// emmc-layout-validate.h — compile-time invariant checks for the layout
// data in emmc-layout-machine.h. Any failing static_assert here is a
// schema-level error in the supplied machine layout.

#pragma once

#include "emmc-layout-machine.h"

#include <string_view>

namespace emmc::layout
{

// Constexpr lookup: return the partition number of the entry whose
// `purpose` equals "rwfs". Returns 0 if missing — the static_assert
// below guarantees the rwfs entry exists, so 0 is unreachable.
constexpr int rwfsPartitionNum()
{
    for (const auto& p : kPartitions)
    {
        if (p.purpose != nullptr && std::string_view(p.purpose) == "rwfs")
        {
            return p.num;
        }
    }
    return 0;
}

} // namespace emmc::layout

namespace emmc::layout::detail
{

constexpr bool partitionsAreUnique()
{
    for (std::size_t i = 0; i < kPartitions.size(); ++i)
    {
        for (std::size_t j = i + 1; j < kPartitions.size(); ++j)
        {
            if (kPartitions[i].num == kPartitions[j].num)
            {
                return false;
            }
        }
    }
    return true;
}

constexpr int typeCount(EmmcPart::Type t)
{
    int n = 0;
    for (const auto& p : kPartitions)
    {
        if (p.type == t)
        {
            ++n;
        }
    }
    return n;
}

constexpr bool extendedHasZeroSectors()
{
    for (const auto& p : kPartitions)
    {
        if (p.type == EmmcPart::Type::Extended && p.sectors != 0)
        {
            return false;
        }
    }
    return true;
}

constexpr bool nonExtendedHaveSectors()
{
    for (const auto& p : kPartitions)
    {
        if (p.type != EmmcPart::Type::Extended && p.sectors == 0)
        {
            return false;
        }
    }
    return true;
}

constexpr bool rwfsExactlyOnce()
{
    int n = 0;
    for (const auto& p : kPartitions)
    {
        if (p.purpose != nullptr && std::string_view(p.purpose) == "rwfs")
        {
            ++n;
        }
    }
    return n == 1;
}

constexpr bool primariesBeforeExtended()
{
    bool seen_extended = false;
    for (const auto& p : kPartitions)
    {
        if (p.type == EmmcPart::Type::Extended)
        {
            seen_extended = true;
        }
        else if (p.type == EmmcPart::Type::Primary && seen_extended)
        {
            return false;
        }
    }
    return true;
}

} // namespace emmc::layout::detail

static_assert(emmc::layout::detail::partitionsAreUnique(),
              "emmc-layout-machine.h: partition `num` values must be unique");
static_assert(emmc::layout::detail::typeCount(
                  emmc::layout::EmmcPart::Type::Extended) == 1,
              "emmc-layout-machine.h: exactly one Extended partition required");
static_assert(emmc::layout::detail::extendedHasZeroSectors(),
              "emmc-layout-machine.h: Extended partition must have sectors=0");
static_assert(emmc::layout::detail::nonExtendedHaveSectors(),
              "emmc-layout-machine.h: non-Extended partitions need sectors>0");
static_assert(
    emmc::layout::detail::rwfsExactlyOnce(),
    "emmc-layout-machine.h: exactly one partition must have purpose=\"rwfs\"");
static_assert(
    emmc::layout::detail::primariesBeforeExtended(),
    "emmc-layout-machine.h: all Primary partitions must precede the Extended");

// MBR has 4 entry slots; each Primary and the Extended take one slot.
// partition.cpp builds the MBR by filling slots [0..n_primary) with
// Primary entries and slot [n_primary] with the Extended.
static_assert(
    emmc::layout::detail::typeCount(emmc::layout::EmmcPart::Type::Primary) <= 4,
    "emmc-layout-machine.h: at most 4 Primary partitions allowed (MBR limit)");
static_assert(
    emmc::layout::detail::typeCount(emmc::layout::EmmcPart::Type::Primary) +
            emmc::layout::detail::typeCount(
                emmc::layout::EmmcPart::Type::Extended) <=
        4,
    "emmc-layout-machine.h: Primary + Extended must fit in 4 MBR slots");
static_assert(
    emmc::layout::detail::typeCount(emmc::layout::EmmcPart::Type::Primary) >= 1,
    "emmc-layout-machine.h: at least one Primary partition required "
    "(rwfs is conventionally Primary)");
