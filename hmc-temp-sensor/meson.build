# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 NVIDIA

executable(
    'hmc-temp-sensor',
    'hmc-temp-sensor.cpp',
    dependencies: [libsystemd_dep],
    install: true,
    install_dir: get_option('bindir'),
)

install_data(
    'hmc-temp-sensor-init.service',
    'hmc-temp-sensor.service',
    'hmc-temp-sensor.timer',
    install_dir: systemd_system_unit_dir,
)
