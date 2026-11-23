cmake_minimum_required(VERSION 3.21)
project(SoundSourceDirectivity VERSION 1.0.0 LANGUAGES CXX)

set(PLUGIN_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
if(PLUGIN_VERSION_PATCH STREQUAL "")
    set(PLUGIN_VERSION_PATCH 0)
endif()

set(PLUGIN_AUTHOR "Draugrath")
set(PLUGIN_COPYRIGHT "Copyright © 2026 ${PLUGIN_AUTHOR}.")
set(PLUGIN_DESCRIPTION "Sound Source Directivity for Skyrim SE and AE: NPC voices are quieter when the speaker faces away, with Steam Audio's dipole directivity.")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Keep local paths out of the binary.
set(EXTERN_DIR "${CMAKE_SOURCE_DIR}/extern")
set(LIB_DIR "${EXTERN_DIR}")
if(NOT EXISTS "${LIB_DIR}/CommonLibSSE-NG/CMakeLists.txt" AND EXISTS "${CMAKE_SOURCE_DIR}/../extern/CommonLibSSE-NG/CMakeLists.txt")
    set(LIB_DIR "${CMAKE_SOURCE_DIR}/../extern")
endif()
get_filename_component(LIB_DIR "${LIB_DIR}" REALPATH)

set(WOS_PATH_MAPS)
foreach(dir IN ITEMS "${LIB_DIR}" "${EXTERN_DIR}")
    file(TO_NATIVE_PATH "${dir}" native)
    list(APPEND WOS_PATH_MAPS "${native}=extern" "${dir}=extern")
endforeach()
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" WOS_NATIVE_SOURCE_DIR)
list(APPEND WOS_PATH_MAPS "${WOS_NATIVE_SOURCE_DIR}=${PROJECT_NAME}" "${CMAKE_SOURCE_DIR}=${PROJECT_NAME}")
if(DEFINED ENV{USERPROFILE})
    file(TO_NATIVE_PATH "$ENV{USERPROFILE}" WOS_NATIVE_USERPROFILE)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" WOS_CMAKE_USERPROFILE)
    list(APPEND WOS_PATH_MAPS "${WOS_NATIVE_USERPROFILE}=user" "${WOS_CMAKE_USERPROFILE}=user")
endif()
list(REMOVE_DUPLICATES WOS_PATH_MAPS)

if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/experimental:deterministic)
    foreach(map IN LISTS WOS_PATH_MAPS)
        add_compile_options("/pathmap:${map}")
    endforeach()
endif()
if(MSVC)
    add_link_options("$<$<CONFIG:Release>:/DEBUG:NONE>" /PDBALTPATH:%_PDB%)
endif()

set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "Enable Skyrim SE" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "Enable Skyrim AE" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "Disable Skyrim VR" FORCE)
set(COMMONLIB_PREBUILT OFF CACHE BOOL "Disable all-runtime CommonLib prebuilt" FORCE)
set(COMMONLIB_PREBUILT_DIR "" CACHE PATH "Disable all-runtime CommonLib prebuilt" FORCE)
set(COMMONLIB_PREBUILT_MULTICONFIG OFF CACHE BOOL "Disable all-runtime CommonLib prebuilt" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)

include(FetchContent)
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

function(declare_dependency name folder repository tag)
    set(source_dir "${LIB_DIR}/${folder}")
    set(binary_dir "${CMAKE_BINARY_DIR}/${folder}")
    if(EXISTS "${source_dir}/CMakeLists.txt")
        message(STATUS "SoundSourceDirectivity: using local ${name} from ${source_dir}")
        FetchContent_Declare(${name} SOURCE_DIR "${source_dir}" BINARY_DIR "${binary_dir}" ${ARGN})
    else()
        message(STATUS "SoundSourceDirectivity: downloading ${name} ${tag}")
        FetchContent_Declare(${name}
            GIT_REPOSITORY "${repository}" GIT_TAG "${tag}" GIT_SHALLOW TRUE GIT_PROGRESS TRUE
            SOURCE_DIR "${source_dir}" BINARY_DIR "${binary_dir}" ${ARGN})
    endif()
endfunction()

declare_dependency(spdlog spdlog https://github.com/gabime/spdlog.git v1.16.0 OVERRIDE_FIND_PACKAGE)
declare_dependency(CommonLibSSE CommonLibSSE-NG https://github.com/alandtse/CommonLibSSE-NG.git v13.1.0)

find_package(fmt CONFIG REQUIRED)
FetchContent_MakeAvailable(spdlog)
set(_saved_skip "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES TRUE)
FetchContent_MakeAvailable(CommonLibSSE)
set(CMAKE_SKIP_INSTALL_RULES "${_saved_skip}")
unset(_saved_skip)
if(NOT COMMAND add_commonlibsse_plugin)
    include("${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake")
endif()

set(SOURCES
    "${CMAKE_SOURCE_DIR}/src/Plugin.cpp"
    "${CMAKE_SOURCE_DIR}/src/Settings.h"
    "${CMAKE_SOURCE_DIR}/src/Directivity.cpp"
    "${CMAKE_SOURCE_DIR}/src/Directivity.h"
    "${CMAKE_SOURCE_DIR}/include/PCH.h")

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "SoundSourceDirectivity"
    AUTHOR "${PLUGIN_AUTHOR}"
    VERSION ${PROJECT_VERSION}
    USE_ADDRESS_LIBRARY
    SOURCES ${SOURCES})

configure_file("${CMAKE_SOURCE_DIR}/cmake/version.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/cmake/Version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/include/Version.h" @ONLY)
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc" "${CMAKE_CURRENT_BINARY_DIR}/include/Version.h")
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/include" "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/src")
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog fmt::fmt ole32)

# Steam Audio: header only at build time; phonon.dll is loaded at runtime from SKSE/Plugins/SoundSourceDirectivity.
set(STEAMAUDIO_DIR "${LIB_DIR}/steamaudio-4.8.1/steamaudio")
if(NOT EXISTS "${STEAMAUDIO_DIR}/include/phonon.h")
    message(FATAL_ERROR "Steam Audio SDK not found at ${STEAMAUDIO_DIR}")
endif()
target_include_directories(${PROJECT_NAME} PRIVATE "${STEAMAUDIO_DIR}/include")

set(HR_DIST_PLUGIN_DIR "${CMAKE_SOURCE_DIR}/dist/SoundSourceDirectivity/SKSE/Plugins")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${HR_DIST_PLUGIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${HR_DIST_PLUGIN_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/packed/SKSE/Plugins/SoundSourceDirectivity.ini" "${HR_DIST_PLUGIN_DIR}/SoundSourceDirectivity.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/LICENSE" "${HR_DIST_PLUGIN_DIR}/SoundSourceDirectivity-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/EXCEPTIONS.md" "${HR_DIST_PLUGIN_DIR}/SoundSourceDirectivity-EXCEPTIONS.txt"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${HR_DIST_PLUGIN_DIR}/SoundSourceDirectivity"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" "${HR_DIST_PLUGIN_DIR}/SoundSourceDirectivity/phonon.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIB_DIR}/steamaudio-4.8.1/LICENSE-SteamAudio.txt" "${HR_DIST_PLUGIN_DIR}/SoundSourceDirectivity/SteamAudio-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STEAMAUDIO_DIR}/THIRDPARTY.md" "${HR_DIST_PLUGIN_DIR}/SoundSourceDirectivity/SteamAudio-THIRDPARTY.txt"
    VERBATIM)
