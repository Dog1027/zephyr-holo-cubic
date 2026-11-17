cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app1)

zephyr_include_directories(include)

add_subdirectory(src/apps)

target_sources(app PRIVATE
  src/main.c
  src/gui.c
  src/tool.c
  src/app_mgr.c
  src/app_list.c
  src/app_thread.c
  src/file.c
  src/input.c
  src/ui.c
  src/icons.c
  src/jpeg.c
  src/led.c
  src/prefs.c
  src/nav.c
  src/i2c_pullup.c
  src/sdconf.c
  src/conf.c
  src/net_job.c
  src/wlan.c
  src/net_time.c
  src/weather.c
  src/net_server.c
  src/lib/tjpgd/tjpgd.c
)
