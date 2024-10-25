project(
  'discord-canary-launcher',
  'c',
  version : '0.1',
  default_options : ['warning_level=3']
)

curl_dep = dependency('libcurl')
json_dep = dependency('json-c')

executable(
  'discord-canary-launcher',
  'launcher.c',
  dependencies: [
    curl_dep,
    json_dep,
  ],
  install : true
)