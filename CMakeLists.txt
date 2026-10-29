# ChronoDrive — tray client that keeps a folder in sync with a drive on the Miniserver (ChronoUI window for setup).
# Compiles against the header-only ChronoUI at M:\ChronoScan\v3\root\ChronoUI (include/). No vcpkg.
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(ChronoDrive LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHRONOUI_ROOT "M:/ChronoScan/v3/root/ChronoUI" CACHE PATH "ChronoUI framework root (has include/)")
if(NOT EXISTS "${CHRONOUI_ROOT}/include/VirtualWidget.hpp")
  message(FATAL_ERROR "ChronoUI headers not found under ${CHRONOUI_ROOT}/include. Pass -DCHRONOUI_ROOT=<path>.")
endif()

# The version lives in installer/ChronoDrive.iss (AppVersion) and nowhere else.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS installer/ChronoDrive.iss)
file(STRINGS installer/ChronoDrive.iss _ver_line REGEX "^#define AppVersion \"[0-9.]+\"")
string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" CD_VERSION "${_ver_line}")
if(NOT CD_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "AppVersion not found in installer/ChronoDrive.iss (got '${CD_VERSION}')")
endif()
string(REPLACE "." ";" _ver_parts "${CD_VERSION}")
list(GET _ver_parts 0 CD_VER_MAJOR)
list(GET _ver_parts 1 CD_VER_MINOR)
list(GET _ver_parts 2 CD_VER_PATCH)
configure_file(version.h.in ${CMAKE_BINARY_DIR}/version.h @ONLY)
message(STATUS "ChronoDrive version ${CD_VERSION}")

set(CD_INC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_BINARY_DIR})

# The ChronoUI app (tray + window). NOMINMAX is not defined (the framework wraps std::max itself).
add_executable(ChronoDrive WIN32 main.cpp msclient.hpp sync.hpp hubpipe.hpp app.rc ${CMAKE_BINARY_DIR}/version.h)
target_include_directories(ChronoDrive PRIVATE ${CHRONOUI_ROOT}/include ${CD_INC})
target_compile_definitions(ChronoDrive PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
target_link_libraries(ChronoDrive PRIVATE d2d1 dwrite windowscodecs shlwapi shell32 ole32 uuid winhttp crypt32 advapi32)
if(MSVC)
  target_compile_options(ChronoDrive PRIVATE /W4 /permissive- /utf-8 /bigobj /MP /EHsc)
  set_property(TARGET ChronoDrive APPEND_STRING PROPERTY LINK_FLAGS
    " /MANIFESTDEPENDENCY:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
endif()

# cdrive.exe: the console twin (status, pair, sync once, log), for scripts and for testing the engine without the window.
add_executable(cdrive cli.cpp msclient.hpp sync.hpp app.rc ${CMAKE_BINARY_DIR}/version.h)
target_include_directories(cdrive PRIVATE ${CD_INC})
target_compile_definitions(cdrive PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(cdrive PRIVATE winhttp crypt32 shlwapi shell32 ole32 advapi32)
if(MSVC)
  target_compile_options(cdrive PRIVATE /W4 /permissive- /utf-8 /bigobj /MP /EHsc)
endif()
