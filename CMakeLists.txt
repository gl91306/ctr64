cmake_minimum_required(VERSION 3.20)
project(ctr64 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ctr64 builds with devkitPro's 3DS toolchain (CMakePresets.json "3ds-release",
# scripts/build-3ds.ps1).
if(NOT NINTENDO_3DS)
  message(FATAL_ERROR "ctr64 is a Nintendo 3DS app: configure it with the 3ds-release preset (devkitPro's 3DS toolchain)")
endif()
add_subdirectory(ctr64)
