project(
  'discord-canary-launcher',
  'c',
  version : '0.1',
  default_options : ['warning_level=3']
)

curl_dep = dependency('libcurl')
json_dep = dependency('json-c')
archive_dep = dependency('libarchive')

executable(
  'discord-canary-launcher',
  'launcher.c',
  dependencies: [
    curl_dep,
    json_dep,
    archive_dep,
  ],
  install : true
)