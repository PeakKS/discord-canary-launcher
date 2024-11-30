project(
  'discord-canary-launcher',
  'c',
  version: '0.1',
  default_options: ['warning_level=3']
)

curl_dep = dependency('libcurl')
json_dep = dependency('json-c')
archive_dep = dependency('libarchive')
gtk_dep = dependency('gtk4')
adw_dep = dependency('libadwaita-1')

config = configuration_data({
  'canary_url': get_option('canary_url'),
  'canary_download': get_option('canary_download'),
  'install_prefix': get_option('install_prefix'),
})

configure_file (
  input: 'config.h.in',
  output: 'config.h',
  configuration: config,
)

executable(
  'discord-canary-launcher',
  'launcher.c',
  dependencies: [
    curl_dep,
    json_dep,
    archive_dep,
    gtk_dep,
    adw_dep,
  ],
  install: true,
  install_dir: '/usr/local/bin',
  install_mode: ['rwsr-sr-x', 0, 0],
)