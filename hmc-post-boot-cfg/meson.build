# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 NVIDIA

executable(
    'i2c-boot-progress',
    'i2c-boot-progress.cpp',
    dependencies: [sdbusplus, libsystemd_dep],
    install: true,
    install_dir: get_option('bindir'),
)

install_data(
    'i2c-boot-progress.service',
    install_dir: systemd_system_unit_dir,
)
