test('tls',
  executable('test-tls', 'test-tls.c', '../src/core/goodix_tls.c',
    include_directories: core_inc,
    dependencies: [ssl_dep, crypto_dep],
  ),
)

test('template',
  executable('test-template', 'test-template.c',
    dependencies: [model_dep],
  ),
  env: {'GXD_DATASET': get_option('dataset')},
  timeout: 300,
)

pam_dep = cc.find_library('pam')
pam_harness = executable('pam-harness', 'pam-harness.c', dependencies: pam_dep)

python = find_program('python3')
test('dbus',
  python,
  args: [
    files('test-dbus.py'),
    goodixfpd_test,
    pam_harness,
    files('system-bus.conf'),
    files('mock-polkit.py'),
  ],
  timeout: 300,
  is_parallel: false,
)
