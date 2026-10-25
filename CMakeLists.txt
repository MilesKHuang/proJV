cmake_minimum_required(VERSION 3.16)
project(proJV VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single source of truth for the version string shown in the UI. To bump the
# version, change ONLY the project() VERSION above; the code reads this macro.
add_compile_definitions(PROJV_VERSION="${PROJECT_VERSION}")

# ── Paths ────────────────────────────────────────────────────────────────
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(SRC_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ── Third-party dependencies ─────────────────────────────────────────────
# The bundled deps (FTXUI / SQLiteCpp / libcurl) each ship install() rules
# that would leak their headers and static libs into our release package.
# Those artifacts are pruned from the CPack staging tree before zipping -- see
# cmake/prune_package.cmake and CPACK_PRE_BUILD_SCRIPTS at the bottom.

# libcurl: Windows builds from source, Linux uses the system library.
if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Windows native SSL" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_FILE ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMB ON CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(PROJV_BUILD_CURL ON)
else()
    set(PROJV_BUILD_CURL OFF)
endif()

# FTXUI options: library only, no examples / tests / docs / install rules.
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

# SQLiteCpp (both platforms, built from source; also builds sqlite3).
# Its CMakeLists defaults SQLITECPP_RUN_CPPCHECK and SQLITECPP_RUN_CPPLINT to ON
# and attaches both as ALL custom targets. That is a lint gate for SQLiteCpp
# itself, not for us, and on CI it aborted the whole Windows build with
#   error MSB8066: Custom build for '...SQLiteCpp_cppcheck.rule' exited with code 1
# Turn them off before add_subdirectory so the option is already set.
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "SQLiteCpp cppcheck target" FORCE)
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "SQLiteCpp cpplint target" FORCE)
add_subdirectory("${EXTERNAL_DIR}/SQLiteCpp-3.3.3"
                 "${CMAKE_BINARY_DIR}/SQLiteCpp")

# libcurl (Windows only; on Linux the system library is used instead).
if(PROJV_BUILD_CURL)
    add_subdirectory("${EXTERNAL_DIR}/curl-8.21.0"
                     "${CMAKE_BINARY_DIR}/external/curl-8.21.0")
endif()

# FTXUI (terminal UI toolkit).
add_subdirectory("${EXTERNAL_DIR}/FTXUI-7.0.3"
                 "${CMAKE_BINARY_DIR}/ftxui")

# libcurl include dir and link target for proJV_lib.
if(WIN32)
    set(CURL_INCLUDE_DIR "${EXTERNAL_DIR}/curl-8.21.0/include")
    set(CURL_LIBRARY libcurl)
else()
    find_package(CURL REQUIRED)
    set(CURL_INCLUDE_DIR ${CURL_INCLUDE_DIRS})
    set(CURL_LIBRARY ${CURL_LIBRARIES})
endif()

# ── Common sources (platform-agnostic business logic) ────────────────────
set(COMMON_SOURCES
    ${SRC_DIR}/client/models.cpp
    ${SRC_DIR}/client/deepseek.cpp
    ${SRC_DIR}/core/config.cpp
    ${SRC_DIR}/core/agent.cpp
    ${SRC_DIR}/core/session.cpp
    ${SRC_DIR}/tools/registry.cpp
    ${SRC_DIR}/tools/file_tool.cpp
    ${SRC_DIR}/tools/format_tool.cpp
    ${SRC_DIR}/tools/md_file_tool.cpp
    ${SRC_DIR}/tools/edit_file_tool.cpp
    ${SRC_DIR}/tools/file_search_tool.cpp
    ${SRC_DIR}/tools/web_tools.cpp
    ${SRC_DIR}/tools/todo_tool.cpp
    ${SRC_DIR}/platform/isystem_util.cpp
    ${SRC_DIR}/core/storage.cpp
    ${SRC_DIR}/core/tool_list_parser.cpp
    ${SRC_DIR}/core/prompts_loader.cpp
    ${SRC_DIR}/core/project_context.cpp
    ${SRC_DIR}/core/sub_agent.cpp
    ${SRC_DIR}/core/image_input.cpp
    ${SRC_DIR}/core/skill_trigger.cpp
    ${SRC_DIR}/core/skill_runner.cpp
    ${SRC_DIR}/core/skills_builtin.cpp
    ${EXTERNAL_DIR}/loguru.cpp
)

set(HEADERS
    ${SRC_DIR}/client/models.h
    ${SRC_DIR}/client/deepseek.h
    ${SRC_DIR}/core/config.h
    ${SRC_DIR}/core/agent.h
    ${SRC_DIR}/core/session.h
    ${SRC_DIR}/core/storage.h
    ${SRC_DIR}/tools/registry.h
    ${SRC_DIR}/core/skill_config.h
    ${SRC_DIR}/core/image_input.h
    ${SRC_DIR}/core/skill_trigger.h
    ${SRC_DIR}/tools/base64.h
    ${SRC_DIR}/core/sub_agent.h
    ${SRC_DIR}/core/skill_runner.h
    ${SRC_DIR}/core/skills_builtin.h
    ${SRC_DIR}/tools/file_tool.h
    ${SRC_DIR}/tools/python_tool_manager.h
    ${SRC_DIR}/tools/json_utils.h
)

# ── Platform-specific sources ────────────────────────────────────────────
if(WIN32)
    set(PLATFORM_SOURCES
        ${SRC_DIR}/tools/shell_tool.cpp
        ${SRC_DIR}/tools/python_tool_manager.cpp
        ${SRC_DIR}/platform/process_runner_win.cpp
        ${SRC_DIR}/platform/system_util_win.cpp
    )
else()
    set(PLATFORM_SOURCES
        ${SRC_DIR}/tools/shell_tool.cpp
        ${SRC_DIR}/tools/python_tool_manager.cpp
        ${SRC_DIR}/platform/process_runner_linux.cpp
        ${SRC_DIR}/platform/system_util_linux.cpp
    )
endif()

# ── Release build: strip debug logs ──────────────────────────────────────
# Use generator expression for multi-config generators (MSVC/Xcode).
# $<$<CONFIG:Release>:PROJV_RELEASE> adds the define only for Release config.
add_compile_definitions($<$<CONFIG:Release>:PROJV_RELEASE>)

# ── Static library (proJV_lib) ───────────────────────────────────────────
add_library(proJV_lib STATIC ${COMMON_SOURCES} ${PLATFORM_SOURCES} ${HEADERS})

# Common includes
target_include_directories(proJV_lib PRIVATE
    ${EXTERNAL_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/client
    ${CURL_INCLUDE_DIR}
)

# ── Platform: Windows ────────────────────────────────────────────────────
if(WIN32)
    target_link_libraries(proJV_lib PRIVATE
        crypt32 ole32 shell32 comctl32 comdlg32 gdi32 user32 dbghelp advapi32
        ${CURL_LIBRARY}
        SQLiteCpp
    )
    target_compile_definitions(proJV_lib PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        # Sources are UTF-8; compile the lib as UTF-8 too so Chinese string
        # literals are encoded correctly. NOTE: must be target_compile_options
        # (add_compile_options after add_library would NOT apply to proJV_lib).
        target_compile_options(proJV_lib PRIVATE /utf-8 /wd4005)
    endif()

# ── Platform: Linux ──────────────────────────────────────────────────────
else()
    target_link_libraries(proJV_lib PRIVATE
        ${CURL_LIBRARY}
        SQLiteCpp
        pthread
        dl
    )
endif()

# ── TUI frontend ─────────────────────────────────────────────────────────
add_executable(proJV_tui
    ${SRC_DIR}/tui/main_tui.cpp
    ${SRC_DIR}/tui/terminal.cpp
    ${SRC_DIR}/tui/app_tui.cpp
    ${SRC_DIR}/tui/chat_view.cpp
    ${SRC_DIR}/tui/bubble_model.cpp
    ${SRC_DIR}/tui/speaker_colors.cpp
    ${SRC_DIR}/tui/markdown_text.cpp
    ${SRC_DIR}/tui/markdown_view.cpp
    ${SRC_DIR}/tui/status_line.cpp
    ${SRC_DIR}/tui/status_bar.cpp
    ${SRC_DIR}/tui/todo_view.cpp
    ${SRC_DIR}/tui/approval_logic.cpp
    ${SRC_DIR}/tui/config_view.cpp
    ${SRC_DIR}/tui/theme_map.cpp
    ${SRC_DIR}/tui/theme_colors.cpp
    ${SRC_DIR}/tui/theme_manager.cpp
    ${SRC_DIR}/tui/theme_editor_view.cpp
    ${SRC_DIR}/tui/clipboard.cpp
)
target_include_directories(proJV_tui PRIVATE
    ${EXTERNAL_DIR}/FTXUI-7.0.3/include
    ${EXTERNAL_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/client
    ${SRC_DIR}/tui
)
target_link_libraries(proJV_tui PRIVATE proJV_lib ftxui::component)

if(WIN32)
    target_compile_definitions(proJV_tui PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()
if(MSVC)
    target_compile_options(proJV_tui PRIVATE /utf-8)
endif()

# ── Deploy example skills next to the runtime exe ────────────────────────
# The engine reads skills from {exeDir}/projv_files/skills/. Repo example
# skills live in <repo>/skills/ (bigbang, monica). Copy them next to the built
# exe after linking, so a plain run finds them and any release that ships
# projv_files/ carries them too. Installing a skill is just copying its folder
# into projv_files/skills/; uninstalling is deleting it. No special-casing.
add_custom_command(TARGET proJV_tui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/skills"
            "$<TARGET_FILE_DIR:proJV_tui>/projv_files/skills"
    COMMENT "Deploying example skills -> projv_files/skills/"
)

# ── Tests ────────────────────────────────────────────────────────────────
option(PROJV_BUILD_TESTS "Build proJV unit tests" ON)
if(PROJV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Release payload: install rules + CPack ───────────────────────────────
# Layout produced by `cpack` (and `cmake --install`):
#   proJV_v<VER>_win64/
#     proJV_tui.exe
#     projv_files/{fonts,pytool,theme,skills}   <- copy this next to the exe
#     README.md, README_zh.md, LICENSE
# A user only needs to keep proJV_tui.exe + projv_files/ together and run.
# The dependencies contribute no install() rules (see cmake/third_party/), so
# this install set is exactly what ships.
# NOTE: projv_files/prompts/ and projv_files/sessions/ are created and seeded
# by the exe on first launch, so they are intentionally not shipped.
install(TARGETS proJV_tui RUNTIME DESTINATION .)

install(DIRECTORY "${CMAKE_SOURCE_DIR}/projv_files/"
        DESTINATION projv_files
        PATTERN "__pycache__" EXCLUDE
        PATTERN "*.pyc" EXCLUDE
        # Never ship secrets or history: these are generated on first launch.
        # CI builds from a clean checkout, but a local `cpack` would otherwise
        # pack the developer's API key and chat sessions.
        PATTERN "config.toml" EXCLUDE
        PATTERN "sessions" EXCLUDE
        PATTERN "prompts" EXCLUDE)

# Example skills (bigbang, monica). The built-in skill_maker is compiled into
# the exe and self-seeds on first launch, so it is NOT packaged here.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/skills/"
        DESTINATION projv_files/skills
        PATTERN "__pycache__" EXCLUDE
        PATTERN "*.pyc" EXCLUDE)

install(FILES
        "${CMAKE_SOURCE_DIR}/README.md"
        "${CMAKE_SOURCE_DIR}/README_zh.md"
        "${CMAKE_SOURCE_DIR}/LICENSE"
        DESTINATION .)

# Prune dependency install artifacts. The bundled dep subprojects (FTXUI /
# SQLiteCpp / libcurl) register install() rules that drop their headers and
# static libs under include/ lib/ bin/ share/ in the staging tree. Our own
# payload lives only at the root (proJV_tui.exe, README*, LICENSE) and under
# projv_files/, so removing those four directories leaves exactly the runtime.
# Runs at install time, in order AFTER the subdirectory install scripts.
install(CODE [[
  foreach(_dep_dir include lib bin share)
    if(EXISTS "${CMAKE_INSTALL_PREFIX}/${_dep_dir}")
      message(STATUS "prune: removing staging dir '${_dep_dir}/' (dependency artifacts)")
      file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/${_dep_dir}")
    endif()
  endforeach()
]])

# ── CPack: one-click package (ZIP on Windows, TGZ on Linux) ──────────────
# Releases are produced by .github/workflows/release.yml on a `v*` tag; the
# same commands work locally:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   cpack --config build/CPackConfig.cmake -B dist
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_SYSTEM_NAME "windows-x64")
else()
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_SYSTEM_NAME "linux-x64")
endif()
set(CPACK_PACKAGE_NAME "proJV")
set(CPACK_PACKAGE_VENDOR "Humanity")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "proJV -- native C++ DeepSeek AI agent (TUI)")
set(CPACK_PACKAGE_FILE_NAME "proJV-${PROJECT_VERSION}-${CPACK_SYSTEM_NAME}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)



