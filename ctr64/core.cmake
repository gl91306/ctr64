# Mupen64Plus-Next core (mupen64plus-core + RSP HLE + libretro glue) as a static library for 3DS.
# Mirrors Makefile.common of mupen64plus-libretro-nx; video is supplied by ctr64 (see src/core_compat).

set(NX ${CMAKE_SOURCE_DIR}/third_party/mupen64plus-libretro-nx)
set(CORE ${NX}/mupen64plus-core)
set(RSPHLE ${NX}/mupen64plus-rsp-hle)
set(LRC ${NX}/libretro-common)

set(CTR64_CORE_SOURCES
  ${CORE}/src/api/callbacks.c
  ${NX}/custom/mupen64plus-core/api/config.c
  ${CORE}/src/api/debugger.c
  ${CORE}/src/api/frontend.c
  ${CORE}/src/backends/plugins_compat/audio_plugin_compat.c
  ${CORE}/src/backends/api/video_capture_backend.c
  ${CORE}/src/backends/plugins_compat/input_plugin_compat.c
  ${CORE}/src/backends/clock_ctime_plus_delta.c
  ${CORE}/src/backends/dummy_video_capture.c
  ${CORE}/src/backends/file_storage.c
  ${CORE}/src/device/cart/cart.c
  ${CORE}/src/device/cart/af_rtc.c
  ${CORE}/src/device/cart/cart_rom.c
  ${CORE}/src/device/cart/eeprom.c
  ${CORE}/src/device/cart/flashram.c
  ${CORE}/src/device/cart/is_viewer.c
  ${CORE}/src/device/cart/sram.c
  ${CORE}/src/device/controllers/game_controller.c
  ${CORE}/src/device/controllers/vru_controller.c
  ${CORE}/src/device/controllers/paks/biopak.c
  ${CORE}/src/device/controllers/paks/mempak.c
  ${CORE}/src/device/controllers/paks/rumblepak.c
  ${CORE}/src/device/controllers/paks/transferpak.c
  ${CORE}/src/device/dd/dd_controller.c
  ${CORE}/src/device/dd/disk.c
  ${CORE}/src/device/device.c
  ${CORE}/src/device/gb/gb_cart.c
  ${CORE}/src/device/gb/mbc3_rtc.c
  ${CORE}/src/device/gb/m64282fp.c
  ${CORE}/src/device/memory/memory.c
  ${CORE}/src/device/pif/bootrom_hle.c
  ${CORE}/src/device/pif/cic.c
  ${CORE}/src/device/pif/n64_cic_nus_6105.c
  ${CORE}/src/device/pif/pif.c
  ${CORE}/src/device/r4300/cached_interp.c
  ${CORE}/src/device/r4300/cp0.c
  ${CORE}/src/device/r4300/cp1.c
  ${CORE}/src/device/r4300/cp2.c
  ${CORE}/src/device/r4300/idec.c
  ${CORE}/src/device/r4300/interrupt.c
  ${CORE}/src/device/r4300/pure_interp.c
  ${CORE}/src/device/r4300/r4300_core.c
  ${CORE}/src/device/r4300/tlb.c
  ${CORE}/src/device/r4300/new_dynarec/new_dynarec.c
  ${CORE}/src/device/r4300/new_dynarec/arm/arm_cpu_features.c
  ${CORE}/src/device/r4300/new_dynarec/arm/linkage_arm.S
  ${CORE}/src/device/rcp/ai/ai_controller.c
  ${CORE}/src/device/rcp/mi/mi_controller.c
  ${CORE}/src/device/rcp/pi/pi_controller.c
  ${CORE}/src/device/rcp/rdp/fb.c
  ${CORE}/src/device/rcp/rdp/rdp_core.c
  ${CORE}/src/device/rcp/ri/ri_controller.c
  ${CORE}/src/device/rcp/rsp/rsp_core.c
  ${CORE}/src/device/rcp/si/si_controller.c
  ${CORE}/src/device/rcp/vi/vi_controller.c
  ${CORE}/src/device/rdram/rdram.c
  ${CORE}/src/main/main.c
  ${CORE}/src/main/util.c
  ${CORE}/src/main/cheat.c
  ${CORE}/src/main/rom.c
  ${CORE}/src/main/savestates.c
  ${CORE}/src/plugin/plugin.c
  ${CORE}/src/plugin/dummy_audio.c
  ${CORE}/src/plugin/dummy_input.c
  ${CORE}/src/plugin/dummy_video.c
  ${CORE}/subprojects/md5/md5.c
  ${CORE}/subprojects/minizip/zip.c
  ${CORE}/subprojects/minizip/unzip.c
  ${CORE}/subprojects/minizip/ioapi.c
  ${NX}/GLideN64/src/osal/osal_files_unix.c
  # libretro glue
  ${NX}/libretro/libretro.c
  ${LRC}/memmap/memalign.c
  ${LRC}/libco/libco.c
  ${NX}/custom/mupen64plus-core/plugin/emulate_game_controller_via_libretro.c
  ${LRC}/audio/resampler/drivers/sinc_resampler.c
  ${LRC}/audio/resampler/drivers/nearest_resampler.c
  ${LRC}/audio/resampler/audio_resampler.c
  ${NX}/custom/mupen64plus-core/plugin/audio_libretro/audio_backend_libretro.c
  ${LRC}/file/config_file.c
  ${LRC}/file/config_file_userdata.c
  ${LRC}/file/file_path.c
  ${LRC}/file/file_path_io.c
  ${LRC}/time/rtime.c
  ${LRC}/compat/compat_strl.c
  ${LRC}/compat/compat_posix_string.c
  ${LRC}/compat/compat_strcasestr.c
  ${LRC}/audio/conversion/float_to_s16.c
  ${LRC}/audio/conversion/s16_to_float.c
  ${LRC}/features/features_cpu.c
  ${LRC}/lists/string_list.c
  ${LRC}/encodings/encoding_utf.c
  ${LRC}/string/stdstring.c
  ${LRC}/vfs/vfs_implementation.c
  ${LRC}/streams/file_stream.c
  ${LRC}/compat/fopen_utf8.c
  ${NX}/custom/mupen64plus-core/api/vidext_libretro.c
  # RSP HLE
  ${RSPHLE}/src/alist.c
  ${RSPHLE}/src/alist_audio.c
  ${RSPHLE}/src/alist_naudio.c
  ${RSPHLE}/src/alist_nead.c
  ${RSPHLE}/src/audio.c
  ${RSPHLE}/src/cicx105.c
  ${RSPHLE}/src/hle.c
  ${RSPHLE}/src/hvqm.c
  ${RSPHLE}/src/jpeg.c
  ${RSPHLE}/src/memory.c
  ${RSPHLE}/src/mp3.c
  ${RSPHLE}/src/musyx.c
  ${RSPHLE}/src/re2.c
  ${RSPHLE}/src/plugin.c
  # ctr64 replacements for OpenGL-only pieces
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core_compat/glsm_ctr.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core_compat/gln64_ctr.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ctr_jit.c)

set(CTR64_CORE_DEFINES
  __LIBRETRO__ M64P_PLUGIN_API M64P_CORE_PROTOTYPES _ENDUSER_RELEASE SINC_LOWER_QUALITY
  MUPENPLUSAPI USE_FILE32API __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS NDEBUG
  ARM ARMv5_ONLY NEW_DYNAREC=3 DYNAREC)

set(CTR64_CORE_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core_compat     # must precede libretro-common (glsm shim)
  ${NX}/custom
  ${NX}/custom/mupen64plus-core
  ${NX}/custom/android/include
  ${NX}/custom/GLideN64
  ${NX}/GLideN64/src
  ${NX}/GLideN64/src/osal
  ${CORE}/src
  ${CORE}/src/api
  ${CORE}/subprojects/md5
  ${CORE}/subprojects/minizip
  ${NX}/custom/mupen64plus-core/plugin/audio_libretro
  ${LRC}/include
  ${NX}/libretro
  ${NX}/GLideN64/src/inc
  ${NX}/xxHash
  ${NX}/custom/dependencies/libzlib)

# --- asm_defines_gas.h: struct offsets for linkage_arm.S, extracted from a compiled object ---
add_library(ctr64_asm_defines OBJECT ${CORE}/src/asm_defines/asm_defines.c)
target_compile_definitions(ctr64_asm_defines PRIVATE ${CTR64_CORE_DEFINES})
target_include_directories(ctr64_asm_defines PRIVATE ${CTR64_CORE_INCLUDES})
target_compile_options(ctr64_asm_defines PRIVATE -O2 -fsigned-char)

set(ASMDEF_DIR ${CMAKE_CURRENT_BINARY_DIR}/asm_defines)
find_program(CTR64_STRINGS NAMES arm-none-eabi-strings HINTS ${DEVKITARM}/bin)
add_custom_command(
  OUTPUT ${ASMDEF_DIR}/asm_defines_gas.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ASMDEF_DIR}
  COMMAND sh -c "${CTR64_STRINGS} '$<TARGET_OBJECTS:ctr64_asm_defines>' | tr -d '\\r' | awk -v dest_dir='${ASMDEF_DIR}' -f '${CORE}/tools/gen_asm_defines.awk'"
  DEPENDS ctr64_asm_defines $<TARGET_OBJECTS:ctr64_asm_defines>
  COMMENT "Generating asm_defines_gas.h"
  VERBATIM)
add_custom_target(ctr64_asm_defines_h DEPENDS ${ASMDEF_DIR}/asm_defines_gas.h)

# --- the core itself ---
add_library(ctr64_core STATIC ${CTR64_CORE_SOURCES})
add_dependencies(ctr64_core ctr64_asm_defines_h)
# PUBLIC: frontend code that includes core headers must see the same struct layouts
# (NEW_DYNAREC changes struct r4300_core and therefore every offset in g_dev).
target_compile_definitions(ctr64_core PUBLIC ${CTR64_CORE_DEFINES})
target_include_directories(ctr64_core PRIVATE ${CTR64_CORE_INCLUDES} ${ASMDEF_DIR})
target_include_directories(ctr64_core INTERFACE ${CORE}/src ${CORE}/src/api ${LRC}/include)
target_compile_options(ctr64_core PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -O2 -fsigned-char -ffast-math -fno-strict-aliasing -fomit-frame-pointer
    -Wno-discarded-qualifiers -Wno-incompatible-pointer-types -w>)
# arm-none-eabi defaults to 1-byte enums; mupen64plus-core assumes int-sized enums (every
# platform it ships on uses them). PUBLIC so frontend code agrees on core struct layouts.
target_compile_options(ctr64_core PUBLIC $<$<COMPILE_LANGUAGE:C>:-fno-short-enums>)
# The dynarec's compiler loops over the 13 host registers per MIPS instruction everywhere;
# unrolled, block compiles (the stutter when a game enters new code) are faster.
# Profile-guided: CTR64_PGO=gen builds it instrumented (profile written to sdmc:/3ds/ctr64/pgo by
# the dev switch pgodump.txt, main.c); "use" builds with ctr64/pgo/new_dynarec.gcda. Off by default:
# in Azahar it moved compile time by under 1% either way (it cannot model the I-cache or branch
# prediction), and a profile goes stale with every edit of new_dynarec.c.
set(CTR64_PGO "off" CACHE STRING "new_dynarec.c profile: gen, use or off")
# CTR64_ND_FLAGS: optimisation of new_dynarec.c (build variants for hardware comparisons)
set(CTR64_ND_FLAGS "-O3;-funroll-loops" CACHE STRING "new_dynarec.c optimisation flags")
set(CTR64_ND_OPTS "${CTR64_ND_FLAGS}")
if(CTR64_PGO STREQUAL "gen")
  # -fprofile-info-section: no file I/O in libgcov (the Windows-hosted compiler bakes a host-style
  # .gcda path); main.c serializes the section itself
  list(APPEND CTR64_ND_OPTS "-fprofile-generate" "-fprofile-info-section=gcov_info" "-fprofile-update=single")
elseif(CTR64_PGO STREQUAL "use" AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/pgo/new_dynarec.gcda)
  list(APPEND CTR64_ND_OPTS "-fprofile-use=${CMAKE_CURRENT_LIST_DIR}/pgo/new_dynarec.gcda" "-fprofile-correction"
       "-Wno-missing-profile" "-fprofile-partial-training")
endif()
set_source_files_properties(${CORE}/src/device/r4300/new_dynarec/new_dynarec.c PROPERTIES
  COMPILE_OPTIONS "${CTR64_ND_OPTS}")
target_compile_options(ctr64_asm_defines PRIVATE -fno-short-enums)
