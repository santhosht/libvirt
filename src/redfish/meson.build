redfish_sources = [
  'redfish_client.c',
  'redfish_conv.c',
  'redfish_driver.c',
]

if conf.has('WITH_REDFISH')
  redfish_lib = static_library(
    'virt_driver_redfish',
    [
      redfish_sources,
    ],
    dependencies: [
      curl_dep,
      src_dep,
    ],
    include_directories: [
      conf_inc_dir,
    ],
  )

  libvirt_libs += redfish_lib
endif
