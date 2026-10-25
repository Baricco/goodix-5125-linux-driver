project('goodixfpd', 'c',
  version: '0.1.0',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 0.62',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')
gnome = import('gnome')

glib_dep = dependency('glib-2.0', version: '>= 2.74')
gio_dep = dependency('gio-2.0', version: '>= 2.74')
gio_unix_dep = dependency('gio-unix-2.0', version: '>= 2.74')
polkit_dep = dependency('polkit-gobject-1')
usb_dep = dependency('libusb-1.0')
ssl_dep = dependency('libssl', version: '>= 3.0')
crypto_dep = dependency('libcrypto', version: '>= 3.0')
m_dep = cc.find_library('m')

add_project_arguments(
  '-DG_LOG_DOMAIN="goodixfpd"',
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_74',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  language: 'c',
)

# ---- core del sensore (protocollo, TLS, immagine, SIFT/SIGFM) ----
core_inc = include_directories('src/core')
core_lib = static_library('gxcore',
  'src/core/goodix_base.c',
  'src/core/goodix_capture.c',
  'src/core/goodix_cmd.c',
  'src/core/goodix_crc.c',
  'src/core/goodix_frame.c',
  'src/core/goodix_fwupdate.c',
  'src/core/goodix_imgproc.c',
  'src/core/goodix_init.c',
  'src/core/goodix_match.c',
  'src/core/goodix_otp.c',
  'src/core/goodix_psk.c',
  'src/core/goodix_sift.c',
  'src/core/goodix_tls.c',
  'src/core/transport.c',
  c_args: ['-Wno-sign-compare'],
  dependencies: [usb_dep, ssl_dep, crypto_dep, m_dep],
)
core_dep = declare_dependency(
  include_directories: core_inc,
  link_with: core_lib,
  dependencies: [usb_dep, ssl_dep, crypto_dep, m_dep],
)

# ---- servizio ----
dbus_src = gnome.gdbus_codegen('fprintd-dbus',
  sources: [
    'src/daemon/net.reactivated.Fprint.Manager.xml',
    'src/daemon/net.reactivated.Fprint.Device.xml',
  ],
  interface_prefix: 'net.reactivated.Fprint.',
  namespace: 'FprintDBus',
  object_manager: true,
  autocleanup: 'all',
)

daemon_inc = include_directories('src/daemon')
model_src = files(
  'src/daemon/gxd-common.c',
  'src/daemon/gxd-template.c',
  'src/daemon/gxd-storage.c',
)
model_lib = static_library('gxdmodel', model_src, dbus_src,
  dependencies: [glib_dep, gio_dep, core_dep],
)
model_dep = declare_dependency(
  include_directories: daemon_inc,
  link_with: model_lib,
  sources: dbus_src[1],
  dependencies: [glib_dep, gio_dep, gio_unix_dep, core_dep],
)

daemon_src = files(
  'src/daemon/main.c',
  'src/daemon/gxd-device.c',
  'src/daemon/gxd-manager.c',
)
daemon_deps = [model_dep, polkit_dep]

goodixfpd = executable('goodixfpd',
  daemon_src, 'src/daemon/gxd-sensor.c',
  c_args: ['-DFPRINTD_PATH="@0@"'.format(get_option('fprintd_path'))],
  dependencies: daemon_deps,
  install: true,
  install_dir: get_option('libexecdir'),
)

# Stesso servizio con il sensore simulato: solo per i test, mai installato.
goodixfpd_test = executable('goodixfpd-test',
  daemon_src, 'src/daemon/gxd-sensor-test.c',
  c_args: ['-DGXD_TEST_BACKEND'],
  dependencies: daemon_deps,
)

dropin_dir = get_option('systemd_dropin_dir')
if dropin_dir == ''
  dropin_dir = get_option('prefix') / 'lib/systemd/system/fprintd.service.d'
endif
configure_file(
  input: 'data/goodixfpd.conf.in',
  output: 'goodixfpd.conf',
  configuration: {'libexecdir': get_option('prefix') / get_option('libexecdir')},
  install: true,
  install_dir: dropin_dir,
)

subdir('tests')
