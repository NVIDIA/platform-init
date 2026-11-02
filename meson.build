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
    'platforms/nvidia/switch_rosalind_bmc.cpp',
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

subdir('emmc-manager')

# No switch image installs the cpu-diag-status recipe that ships the script
# this unit runs, so building it here would install an inert unit.
if platform_name.contains('bmc') and not platform_name.contains('switch')
    subdir('cpu-diag-status')
endif

if platform_name.contains('vr-nvl-hmc')
    subdir('i2c-dump-server')
    subdir('hmc-temp-sensor')
    subdir('hmc-post-boot-cfg')
endif

# Only spc6-ast2600-bmc sets BIND_I3C_ON_BIOS_BOOT=1; elsewhere the handler
# defaults it to 0, so the unit would never be started.
if platform_name.contains('spc6-ast2600')
    subdir('bind-i3c')
endif
