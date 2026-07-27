cmake_minimum_required(VERSION 3.16)
project(proJV VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC: treat source files as UTF-8 (fixes C4819 warnings and C2001 with Unicode literals)
# Also suppress C4005 (macro redefinition) — harmless for external libs like loguru
if(MSVC)
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/wd4005>")
endif()

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(IMGUI_DIR "${EXTERNAL_DIR}/imgui")
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ── SQLiteCpp ──
add_subdirectory(
    "${EXTERNAL_DIR}/SQLiteCpp-3.3.3"
    "${CMAKE_BINARY_DIR}/SQLiteCpp"
)

# ── GLFW (statically linked, from source) ──
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXTERNAL_DIR}/glfw)

# ── Common sources ──
set(COMMON_SOURCES
    ${SRC_DIR}/gui_backend.cpp
    ${SRC_DIR}/client/models.cpp
    ${SRC_DIR}/client/deepseek.cpp
    ${SRC_DIR}/core/config.cpp
    ${SRC_DIR}/core/agent.cpp
    ${SRC_DIR}/core/session.cpp
    ${SRC_DIR}/tools/registry.cpp
    ${SRC_DIR}/tools/shell_tool.cpp
    ${SRC_DIR}/tools/file_tool.cpp
    ${SRC_DIR}/tools/md_file_tool.cpp
    ${SRC_DIR}/tools/edit_file_tool.cpp
    ${SRC_DIR}/tools/file_search_tool.cpp
    ${SRC_DIR}/tools/web_tools.cpp
    ${SRC_DIR}/tools/python_tool_manager.cpp
    ${SRC_DIR}/tools/todo_tool.cpp
    ${SRC_DIR}/core/storage.cpp
    ${SRC_DIR}/core/tool_list_parser.cpp
    ${SRC_DIR}/core/prompts_loader.cpp
    ${SRC_DIR}/ui/app.cpp
    ${SRC_DIR}/ui/render_chat.cpp
    ${SRC_DIR}/ui/render_settings.cpp
    ${SRC_DIR}/ui/markdown_render.cpp
    ${SRC_DIR}/ui/theme.cpp
    ${SRC_DIR}/ui/theme_popup.cpp
    ${EXTERNAL_DIR}/loguru.cpp
)

set(HEADERS
    ${SRC_DIR}/gui_backend.h
    ${SRC_DIR}/client/models.h
    ${SRC_DIR}/client/deepseek.h
    ${SRC_DIR}/core/config.h
    ${SRC_DIR}/core/agent.h
    ${SRC_DIR}/core/session.h
    ${SRC_DIR}/core/storage.h
    ${SRC_DIR}/tools/registry.h
    ${SRC_DIR}/tools/shell_tool.h
    ${SRC_DIR}/tools/file_tool.h
    ${SRC_DIR}/tools/python_tool_manager.h
    ${SRC_DIR}/tools/json_utils.h
)

# ── libcurl (from source) ──
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

# ── Release build: strip debug logs ──
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_definitions(PROJV_RELEASE)
endif()

# ── Static library ──
add_library(proJV_lib STATIC ${COMMON_SOURCES} ${HEADERS})
target_include_directories(proJV_lib PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${EXTERNAL_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/client
    ${CURL_INCLUDE_DIR}
    ${EXTERNAL_DIR}/glfw/include   # for GLFW/glfw3.h
)
target_link_libraries(proJV_lib PRIVATE
    opengl32
    crypt32 ole32 shell32 comctl32 comdlg32 gdi32 user32 dbghelp advapi32
    libcurl
    SQLiteCpp
)
target_compile_definitions(proJV_lib PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

# ── GUI executable ──
# WIN32 = /SUBSYSTEM:WINDOWS (no console window)
# /ENTRY:mainCRTStartup tells MSVC to call main() instead of WinMain()
add_executable(proJV WIN32 ${SRC_DIR}/main.cpp)
target_include_directories(proJV PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${EXTERNAL_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/client
    ${EXTERNAL_DIR}/glfw/include
)
target_link_libraries(proJV PRIVATE
    proJV_lib
    glfw
)
target_compile_definitions(proJV PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
# Override entry point: WIN32 subsystem defaults to WinMain, but we use main()
if(MSVC)
    set_target_properties(proJV PROPERTIES
        LINK_FLAGS "/ENTRY:mainCRTStartup"
    )
endif()

# ── imgui sources (S1: OpenGL3 + GLFW backends) ──
target_sources(proJV PRIVATE
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(proJV PRIVATE ${IMGUI_DIR}/backends)

# ── Copy assets + projv_files to build directory ──
add_custom_command(TARGET proJV POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:proJV>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/projv_files"
        "$<TARGET_FILE_DIR:proJV>/projv_files"
    COMMENT "Copying assets and projv_files to build output"
)
