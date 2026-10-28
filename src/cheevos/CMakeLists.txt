# SPDX-License-Identifier: GPL-3.0-or-later
#
# RetroAchievements support, as its own library: keeps dsperate_core and the
# headless frontend free of the network transport, and lets tests link
# frontend translation units one at a time.
#
# Not built: rc_client_raintegration.c (Windows-only authoring toolkit),
# rc_libretro.c (libretro integration).
set(RC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rcheevos)

# Whole-directory: these translation units reference each other freely
# (trigger -> condset -> condition -> operand -> memref), so a subset would be
# guesswork that breaks on an update.
file(GLOB RC_RUNTIME_SOURCES ${RC_DIR}/src/rcheevos/*.c)

file(GLOB RC_API_SOURCES ${RC_DIR}/src/rapi/*.c)

add_library(dsperate_cheevos STATIC
  cheevos_hash.cpp
  cheevos_memory.cpp
  cheevos_http.cpp
  cheevos_client.cpp
  cheevos_credentials.cpp
  cheevos_badges.cpp
  cheevos_outbox.cpp
  cheevos_png.cpp
  cheevos_identity.cpp
  ${RC_DIR}/src/rc_client.c
  ${RC_API_SOURCES}
  ${RC_RUNTIME_SOURCES}
  ${RC_DIR}/src/rhash/hash.c
  ${RC_DIR}/src/rhash/hash_rom.c
  ${RC_DIR}/src/rhash/md5.c
  ${RC_DIR}/src/rc_util.c
  ${RC_DIR}/src/rc_compat.c
  ${RC_DIR}/src/rc_version.c
)

target_include_directories(dsperate_cheevos
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
  PRIVATE ${RC_DIR}/include ${RC_DIR}/src ${RC_DIR}/src/rhash)

# Disc/encrypted(3DS)/zip hash paths are for consoles we never ask about,
# dropping cdreader.c, aes.c, hash_disc.c, hash_encrypted.c, hash_zip.c.
# RC_HASH_NO_ZIP costs nothing even for zipped ROMs: those reach rcheevos
# already unpacked/mapped through our own RomSource (see cheevos_hash.cpp).
target_compile_definitions(dsperate_cheevos PRIVATE
  RC_HASH_NO_DISC RC_HASH_NO_ENCRYPTED RC_HASH_NO_ZIP RC_DISABLE_LUA)
target_include_directories(dsperate_cheevos PRIVATE ${RC_DIR}/src/rapi)

# The version in DSperate's User-Agent (cheevos_identity.h): "0.<n>", n the fork's commit
# count since upstream 7c1fb67, the number in the package's pkgver (packaging/arch/PKGBUILD).
# RetroAchievements wants a numeric version that increases; upstream's tags do not follow
# this fork. Taken at configure time; a tree without git history reports 0.0.
set(DSPERATE_CHEEVOS_VERSION "" CACHE STRING "Version DSperate reports to RetroAchievements (default: 0.<commits since 7c1fb67>)")
set(_ds_ra_version "${DSPERATE_CHEEVOS_VERSION}")
if(NOT _ds_ra_version)
  execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} rev-list --count 7c1fb67..HEAD
                  OUTPUT_VARIABLE _ds_count OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _ds_count_rc)
  if(_ds_count_rc EQUAL 0 AND _ds_count MATCHES "^[0-9]+$")
    set(_ds_ra_version "0.${_ds_count}")
  else()
    set(_ds_ra_version "0.0")
    message(WARNING "DSperate: no git history to count; RetroAchievements will see version 0.0")
  endif()
endif()
message(STATUS "DSperate: RetroAchievements client version ${_ds_ra_version}")
target_compile_definitions(dsperate_cheevos PRIVATE
  DSPERATE_CHEEVOS_VERSION="${_ds_ra_version}")

# dsperate_core knows nothing about achievements; the dependency runs this way.
target_link_libraries(dsperate_cheevos PUBLIC dsperate_core ${CMAKE_DL_LIBS})
