cmake_minimum_required(VERSION 3.16)
project(proJV VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Paths ────────────────────────────────────────────────────────────────
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(SRC_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ── SQLiteCpp (both platforms, from source) ──────────────────────────────
add_subdirectory(
    "${EXTERNAL_DIR}/SQLiteCpp-3.3.3"
    "${CMAKE_BINARY_DIR}/SQLiteCpp"
)

# ── Common sources (platform-agnostic business logic) ────────────────────
set(COMMON_SOURCES
    ${SRC_DIR}/client/models.cpp
    ${SRC_DIR}/client/deepseek.cpp
    ${SRC_DIR}/core/config.cpp
    ${SRC_DIR}/core/agent.cpp
    ${SRC_DIR}/core/session.cpp
    ${SRC_DIR}/tools/registry.cpp
    ${SRC_DIR}/tools/file_tool.cpp
    ${SRC_DIR}/tools/md_file_tool.cpp
    ${SRC_DIR}/tools/edit_file_tool.cpp
    ${SRC_DIR}/tools/file_search_tool.cpp
    ${SRC_DIR}/tools/web_tools.cpp
    ${SRC_DIR}/tools/todo_tool.cpp
    ${SRC_DIR}/platform/isystem_util.cpp
    ${SRC_DIR}/core/storage.cpp
    ${SRC_DIR}/core/tool_list_parser.cpp
    ${SRC_DIR}/core/prompts_loader.cpp
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

# ── libcurl: Windows from source, Linux from system ──────────────────────
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
    add_subdirectory(${EXTERNAL_DIR}/curl-8.21.0)
    set(CURL_INCLUDE_DIR "${EXTERNAL_DIR}/curl-8.21.0/include")
    set(CURL_LIBRARY libcurl)
else()
    find_package(CURL REQUIRED)
    set(CURL_INCLUDE_DIR ${CURL_INCLUDE_DIRS})
    set(CURL_LIBRARY ${CURL_LIBRARIES})
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
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/wd4005>")
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

# ── FTXUI TUI backend (frontend migration) ───────────────────────────────
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXTERNAL_DIR}/FTXUI-7.0.3 ${CMAKE_BINARY_DIR}/ftxui)

add_executable(proJV_tui
    ${SRC_DIR}/tui/main_tui.cpp
    ${SRC_DIR}/tui/terminal.cpp
    ${SRC_DIR}/tui/app_tui.cpp
    ${SRC_DIR}/tui/chat_view.cpp
    ${SRC_DIR}/tui/bubble_model.cpp
    ${SRC_DIR}/tui/markdown_text.cpp
    ${SRC_DIR}/tui/markdown_view.cpp
    ${SRC_DIR}/tui/status_line.cpp
    ${SRC_DIR}/tui/status_bar.cpp
    ${SRC_DIR}/tui/todo_view.cpp
    ${SRC_DIR}/tui/approval_logic.cpp
    ${SRC_DIR}/tui/config_view.cpp
    ${SRC_DIR}/tui/theme_map.cpp
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

# ── Tests ────────────────────────────────────────────────────────────────
option(PROJV_BUILD_TESTS "Build proJV unit tests" ON)
if(PROJV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()


