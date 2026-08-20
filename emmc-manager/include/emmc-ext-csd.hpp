// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA
//
// EXT_CSD register offsets and MMC opcodes per JESD84-B51 (eMMC 5.1).
// Defined from the JEDEC standard, not copied from mmc-utils (GPL-2.0).

#pragma once

#include <cstdint>

namespace emmc::extcsd
{

// EXT_CSD byte offsets (JESD84-B51 §7.4).
inline constexpr unsigned int kErase_Group_Def = 175;
inline constexpr unsigned int kPartitioning_Support = 160;
inline constexpr unsigned int kPartition_Setting_Completed = 155;
inline constexpr unsigned int kPartitions_Attribute = 156;
inline constexpr unsigned int kRst_n_Function = 162;
inline constexpr uint8_t kRstNFunctionDisabledDefault =
    0x00;                                             // power-on default
inline constexpr uint8_t kRstNFunctionEnabled = 0x01; // permanently enabled
inline constexpr uint8_t kRstNFunctionPermDisabled =
    0x02;                                             // permanently disabled
inline constexpr uint8_t kPartitionsAttributeEnhUsr =
    static_cast<uint8_t>(1U << 0);
inline constexpr uint8_t kEraseGroupDefHighCapacity = 0x01;
inline constexpr unsigned int kEnh_Start_Addr_Lsb = 136;
inline constexpr unsigned int kEnh_Start_Addr_Byte1 = 137;
inline constexpr unsigned int kEnh_Start_Addr_Byte2 = 138;
inline constexpr unsigned int kEnh_Start_Addr_Msb = 139;
inline constexpr unsigned int kEnh_Size_Mult_Lsb = 140;
inline constexpr unsigned int kEnh_Size_Mult_Mid = 141;
inline constexpr unsigned int kEnh_Size_Mult_Msb = 142;
inline constexpr unsigned int kMax_Enh_Size_Mult_Lsb = 157;
inline constexpr unsigned int kMax_Enh_Size_Mult_Mid = 158;
inline constexpr unsigned int kMax_Enh_Size_Mult_Msb = 159;
inline constexpr unsigned int kHc_Erase_Grp_Size = 224;
inline constexpr unsigned int kHc_Wp_Grp_Size = 221;
inline constexpr unsigned int kPre_Eol_Info = 267;
// SEC_COUNT (§7.4.81): bytes 212–215, little-endian. Total user-area
// sector count reported by the chip; multiply by 512 for byte size.
inline constexpr unsigned int kSec_Count_Lsb = 212;

inline constexpr unsigned int kExtCsdSizeBytes = 512;

// MMC opcodes (JESD84-B51 §6.10).
inline constexpr unsigned int kMmcSwitch = 6;
inline constexpr unsigned int kMmcSendExtCsd = 8;
inline constexpr unsigned int kMmcErase = 38;
inline constexpr unsigned int kMmcEraseGroupStart = 35;
inline constexpr unsigned int kMmcEraseGroupEnd = 36;

// CMD38 erase argument values.
inline constexpr uint32_t kMmcEraseArgSecureErase = 0x80000000U;
inline constexpr uint32_t kMmcEraseArgTrim = 0x00000001U;
inline constexpr uint32_t kMmcEraseArgDiscard = 0x00000003U;

// PRE_EOL_INFO state values.
inline constexpr uint8_t kPreEolNormal = 0x01;
inline constexpr uint8_t kPreEolWarning = 0x02;
inline constexpr uint8_t kPreEolUrgent = 0x03;

// CMD6 SWITCH access mode.
inline constexpr uint8_t kSwitchModeWriteByte = 0x03;

// ENH_SIZE_MULT base unit per JESD84-B51 §7.4.74:
//   Size = ENH_SIZE_MULT × HC_WP_GRP_SIZE × HC_ERASE_GRP_SIZE × 524288
// HC_* multipliers come from EXT_CSD at runtime.
inline constexpr unsigned long long kEnhSizeMultBaseBytes = 512ULL * 1024ULL;

} // namespace emmc::extcsd
