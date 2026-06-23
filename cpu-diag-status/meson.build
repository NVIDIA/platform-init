# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 NVIDIA

executable(
    'cpu-diag-status',
    'cpu_diag_status.cpp',
    dependencies: [sdbusplus, libsystemd_dep],
    install: true,
    install_dir: get_option('libexecdir'),
)

install_data(
    'cpu-diag-status.service',
    install_dir: systemd_system_unit_dir,
)
