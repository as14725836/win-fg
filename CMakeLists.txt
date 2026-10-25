cmake_minimum_required(VERSION 3.18)
project(win_fg CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# ---- self-identifying build stamp -------------------------------------------
# CI passes -DWINFG_GIT_DESCRIBE=<git describe>; fall back to a local git describe,
# then to the VERSION file, so every build (CI or local) embeds an identifiable
# string. Recover it from a shipped .so with:
#     strings libwin_fg.so | grep win-fg-build
#     nm -D libwin_fg.so   | grep win_fg_build_version
if(NOT WINFG_GIT_DESCRIBE)
  execute_process(
    COMMAND git describe --tags --always --dirty --long
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE WINFG_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT WINFG_GIT_DESCRIBE)
  file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION _winfg_ver LIMIT_COUNT 1)
  set(WINFG_GIT_DESCRIBE "v${_winfg_ver}-nogit")
endif()
string(TIMESTAMP WINFG_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
message(STATUS "win-fg build stamp: ${WINFG_GIT_DESCRIBE} (${WINFG_BUILD_DATE})")
configure_file(${CMAKE_SOURCE_DIR}/src/version_stamp.cpp.in
               ${CMAKE_BINARY_DIR}/version_stamp.cpp @ONLY)

# VULKAN_INCLUDE points at a Vulkan-Headers include/ (vk_layer.h + vulkan.h).
add_library(win_fg SHARED src/layer.cpp src/framegen.cpp
            ${CMAKE_BINARY_DIR}/version_stamp.cpp)
target_include_directories(win_fg PRIVATE src ${VULKAN_INCLUDE})
target_compile_definitions(win_fg PRIVATE VK_NO_PROTOTYPES=1)
if(ANDROID)
  # Android-specific Vulkan platform header (vulkan_android.h needs NDK types
  # like AHardwareBuffer). Only defined when actually cross-compiling with the
  # NDK; termux-glibc / desktop glibc builds stay platform-neutral and use the
  # software (non-GL) Vulkan path.
  target_compile_definitions(win_fg PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
endif()
set_target_properties(win_fg PROPERTIES OUTPUT_NAME win_fg)
if(ANDROID)
  # liblog for __android_log_print; libandroid for ASharedMemory_create()
  # (winfg_stats.hpp uses it in place of the missing Bionic shm_open).
  find_library(log-lib log)
  find_library(android-lib android)
  target_link_libraries(win_fg ${log-lib})
  if(android-lib)
    target_link_libraries(win_fg ${android-lib})
  endif()
else()
  # shm_open()/ftruncate() historically live in librt; glibc >= 2.34 folded them
  # into libc, but linking rt is harmless and keeps older toolchains working.
  find_library(rt-lib rt)
  if(rt-lib)
    target_link_libraries(win_fg ${rt-lib})
  endif()
endif()

# ---- offline harnesses ------------------------------------------------------
# record_apicount drives FrameGen::record() against a real (or software) Vulkan
# device and counts the vkCmd* calls plus the exact device memory the per-size
# scratch set asks for. It never submits anything, so it runs fine on lavapipe
# and needs no GPU — see the header comment in tests/record_apicount.cpp for why
# emit-side counts are the honest metric for the record()-path optimizations.
option(WINFG_BUILD_TESTS "Build the offline verification harnesses" OFF)
if(WINFG_BUILD_TESTS)
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    add_executable(record_apicount tests/record_apicount.cpp src/framegen.cpp)
    target_include_directories(record_apicount PRIVATE src ${VULKAN_INCLUDE})
    # framegen.cpp goes through the dispatch tables only, so it must NOT see the
    # loader prototypes; the harness itself does need them (it creates the device
    # and calls vkGetDeviceProcAddr to fill the tables), hence the per-source flag.
    set_source_files_properties(src/framegen.cpp PROPERTIES
                                COMPILE_DEFINITIONS VK_NO_PROTOTYPES=1)
    target_link_libraries(record_apicount Vulkan::Vulkan)
  else()
    message(WARNING "WINFG_BUILD_TESTS=ON but no Vulkan loader; skipping harnesses")
  endif()
endif()
