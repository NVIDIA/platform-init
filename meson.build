project(
    'platform-init',
    'cpp',
    meson_version: '>=1.7.0',
    version: '0.1',
    default_options: ['warning_level=3', 'cpp_std=c++23'],
)

gpiodcxx_dep = dependency('libgpiodcxx', default_options: ['bindings=cxx'])
systemd_dep = dependency('systemd')
libsystemd_dep = dependency('libsystemd')
sdbusplus = dependency('sdbusplus', include_type: 'system')
cli11_dep = dependency(
    'CLI11',
    include_type: 'system',
    default_options: ['tests=disabled'],
)
i2c_dep = meson.get_compiler('cpp').find_library('i2c')
util_srcs = files('gpio.cpp', 'i2c.cpp', 'utilities.cpp')

platform_srcs = files(
    'platforms/intel/acrp.cpp',
    'platforms/intel/jcrp.cpp',
    'platforms/nvidia/compute_bmc_common.cpp',
    'platforms/nvidia/compute_hmc_common.cpp',
    'platforms/nvidia/gb200.cpp',
    'platforms/nvidia/gb200nvl_bmc.cpp',
    'platforms/nvidia/gb200nvl_hmc.cpp',
    'platforms/nvidia/hgxr_hmc.cpp',
    'platforms/nvidia/nvidia_platform_utils.cpp',
    'platforms/nvidia/nvl32.cpp',
    'platforms/nvidia/switch_ast2700_bmc.cpp',
    'platforms/nvidia/switch_common.cpp',
    'platforms/nvidia/switch_mctp.cpp',
    'platforms/nvidia/switch_mcu.cpp',
    'platforms/nvidia/switch_misc.cpp',
    'platforms/nvidia/switch_preps.cpp',
    'platforms/nvidia/switch_rosalind_bmc.cpp',
    'platforms/nvidia/switch_rtc.cpp',
    'platforms/nvidia/switch_spc6_ast2600_bmc.cpp',
    'platforms/nvidia/vr_c2_bmc.cpp',
    'platforms/nvidia/vr_e5010_bmc.cpp',
    'platforms/nvidia/vr_nvl.cpp',
    'platforms/nvidia/vr_nvl_bmc.cpp',
    'platforms/nvidia/vr_nvl_bmc_ast2700.cpp',
    'platforms/nvidia/vr_nvl_hmc.cpp',
    'platforms/nvidia/vr_nvl_hmc_ast2700.cpp',
    'platforms/nvidia/vr_pdb.cpp',
    'platforms/nvidia/vr_quark_bmc.cpp',
    'platforms/nvidia/vr_vhmc_quark.cpp',
    'platforms/nvidia/vru_nvl_bmc.cpp',
    'platforms/nvidia/vru_nvl_hmc.cpp',
)

executable(
    'platform',
    ['platform.cpp'] + platform_srcs + util_srcs,
    dependencies: [gpiodcxx_dep, libsystemd_dep, cli11_dep, i2c_dep, sdbusplus],
    include_directories: ['.', 'platforms'],
    install: true,
    install_dir: get_option('libexecdir'),
)

if get_option('tests').allowed()
    rtc_gtest = dependency(
        'gtest',
        main: true,
        required: get_option('tests').enabled(),
    )
    if rtc_gtest.found()
        test(
            'test-switch-rtc',
            executable(
                'test-switch-rtc',
                'test/test-switch-rtc.cpp',
                'platforms/nvidia/switch_rtc.cpp',
                'platforms/nvidia/nvidia_platform_utils.cpp',
                util_srcs,
                include_directories: ['.', 'platforms'],
                dependencies: [
                    rtc_gtest,
                    gpiodcxx_dep,
                    i2c_dep,
                    libsystemd_dep,
                    sdbusplus,
                ],
            ),
        )
        test(
            'test-compute-hmc',
            executable(
                'test-compute-hmc',
                'test/test-standby-wait.cpp',
                'test/test-rtc-sync.cpp',
                'platforms/nvidia/compute_bmc_common.cpp',
                'platforms/nvidia/compute_hmc_common.cpp',
                'platforms/nvidia/nvidia_platform_utils.cpp',
                util_srcs,
                include_directories: ['.', 'platforms'],
                dependencies: [
                    rtc_gtest,
                    gpiodcxx_dep,
                    i2c_dep,
                    libsystemd_dep,
                    sdbusplus,
                ],
            ),
        )
    endif
endif

platform_name = get_option('platform-name')

systemd_system_unit_dir = systemd_dep.get_variable('systemd_system_unit_dir')
configure_file(
    input: 'platform_init.service.in',
    output: 'platform_init.service',
    install_dir: systemd_system_unit_dir,
    install: true,
    configuration: configuration_data({'PLATFORM_NAME': platform_name}),
)

script_base = 'platforms/nvidia/scripts/platforms'

# Platforms with an early-init step: the standby wait and RTC sync their
# initramfs ran.
if platform_name in ['nvidia-vr-nvl-hmc', 'nvidia-vr-nvl-hmc-ast2700']
    configure_file(
        input: 'platform_early_init.service.in',
        output: 'platform_early_init.service',
        install_dir: systemd_system_unit_dir,
        install: true,
        configuration: configuration_data({'PLATFORM_NAME': platform_name}),
    )
endif

# Replaces bmc-plat-specific-preps.sh under the same unit name.
if platform_name in [
    'nvidia-switch-ast2700-bmc',
    'nvidia-switch-rosalind-bmc',
    'nvidia-switch-spc6-ast2600-bmc',
]
    configure_file(
        input: 'bmc-plat-specific-preps.service.in',
        output: 'bmc-plat-specific-preps.service',
        install_dir: systemd_system_unit_dir,
        install: true,
        configuration: configuration_data({'PLATFORM_NAME': platform_name}),
    )
endif

if get_option('tests').allowed()
    preps_gtest = dependency(
        'gtest',
        main: true,
        required: get_option('tests').enabled(),
    )
    if preps_gtest.found()
        test(
            'test-switch-preps',
            executable(
                'test-switch-preps',
                'platforms/nvidia/test/test_switch_preps.cpp',
                'platforms/nvidia/switch_preps.cpp',
                'i2c.cpp',
                include_directories: ['.', 'platforms'],
                dependencies: [preps_gtest, i2c_dep],
            ),
        )
    endif
endif

subdir('emmc-manager')

# Platforms whose machine layer installs an eMMC layout (emmc-layout.conf)
# for nvidia-emmc-partition. The layout parser and its tests build always.
emmc_partition_platforms = [
    'nvidia-vr-nvl-bmc',
    'nvidia-vr-nvl-hmc',
    'nvidia-vr-c2-bmc',
    'nvidia-vr-quark-bmc',
    'nvidia-hgxr-hmc',
    'nvidia-switch-rosalind-bmc',
    'nvidia-switch-spc6-ast2600-bmc',
]
subdir('emmc-partition')

# Only spc6-ast2600-bmc sets BIND_I3C_ON_BIOS_BOOT=1; elsewhere the handler
# defaults it to 0, so the unit would never be started.
if platform_name.contains('spc6-ast2600')
    subdir('bind-i3c')
endif

if get_option('tests').allowed()
    switch_gtest_dep = dependency(
        'gtest',
        main: true,
        required: get_option('tests').enabled(),
    )
    if switch_gtest_dep.found()
        test(
            'test-switch-mcu',
            executable(
                'test-switch-mcu',
                'test/test-switch-mcu.cpp',
                'test/test-switch-mctp.cpp',
                'platforms/nvidia/switch_mcu.cpp',
                'platforms/nvidia/switch_mctp.cpp',
                'platforms/nvidia/switch_common.cpp',
                'platforms/nvidia/nvidia_platform_utils.cpp',
                util_srcs,
                dependencies: [
                    switch_gtest_dep,
                    gpiodcxx_dep,
                    libsystemd_dep,
                    i2c_dep,
                    sdbusplus,
                ],
                include_directories: ['.', 'platforms', 'platforms/nvidia'],
            ),
        )
        test(
            'test-switch-misc',
            executable(
                'test-switch-misc',
                'test/test-switch-misc.cpp',
                'platforms/nvidia/switch_misc.cpp',
                dependencies: [switch_gtest_dep, sdbusplus],
                include_directories: ['platforms/nvidia'],
            ),
        )
    endif
endif
