cmake_minimum_required(VERSION 3.24)
project(payback-linux-host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release artifacts must not embed developer-machine library search paths;
# every linked library (GTK4, GIO, libsecret, curses, LZ4, zlib) is a system
# dependency resolved by the normal loader search on the target machine.
set(CMAKE_SKIP_RPATH TRUE)

# RIVET_ROOT points at the installed Rivet package. `raco rivet build` sets
# it; manual builds may export it themselves.
if(NOT DEFINED ENV{RIVET_ROOT})
  message(FATAL_ERROR "RIVET_ROOT is not set. Export RIVET_ROOT to a Rivet checkout.")
endif()
set(RIVET_ROOT "$ENV{RIVET_ROOT}")

# The Rivet platform is consumed as a package library target (project
# constraint, rivet#238): hosts never compile Rivet sources into their own
# executable lists — rivet adding a translation unit needs no downstream
# change.
add_subdirectory("${RIVET_ROOT}/platform/linux"
                 "${CMAKE_BINARY_DIR}/rivet-platform-build")

# shared/i18n single-source strings load through json-glib at runtime.
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

add_executable(RivetHost
  src/main.cpp
)

target_include_directories(RivetHost PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(RivetHost PRIVATE
  rivet::platform
  PkgConfig::JSON_GLIB
)

target_compile_options(RivetHost PRIVATE -Wall -Wextra -Wpedantic)

# Host unit tests ride the default build target so `raco rivet build` and the
# CI linux job compile them; ctest runs them (see ci.yml). Packaging stages
# only the RivetHost binary, so test binaries never reach a release.
include(CTest)
if(BUILD_TESTING)
  add_executable(normalize-date-test
    tests/normalize-date-test.cpp
  )

  target_include_directories(normalize-date-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
  )

  target_link_libraries(normalize-date-test PRIVATE
    rivet::platform
    PkgConfig::JSON_GLIB
  )

  target_compile_options(normalize-date-test PRIVATE -Wall -Wextra -Wpedantic)

  add_test(NAME normalize-date COMMAND normalize-date-test)
endif()
