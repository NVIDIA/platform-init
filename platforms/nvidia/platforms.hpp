// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#pragma once

namespace nvidia
{
// list your platform initialization callbacks here
int init_gb200_base();
int init_gb200_with_p2020();
int init_gb200nvl_bmc();
int init_gb200nvl_hmc();
int init_nvl32();
int init_vr_nvl();

int init_vr_c2_bmc();
int init_vr_nvl_bmc();
int init_vr_nvl_bmc_ast2700();
int init_vr_nvl_hmc();
int init_vr_nvl_hmc_ast2700();
int init_vr_e5010_bmc();
int init_vr_quark_bmc();
int init_vr_vhmc_quark();
int init_vru_nvl_bmc();
int init_vru_nvl_hmc();

int init_switch_rosalind_bmc();
int init_switch_spc6_ast2600_bmc();
int init_switch_ast2700_bmc();

} // namespace nvidia
