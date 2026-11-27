# proJV unit tests (doctest, vendored at external/doctest/doctest.h)

set(TEST_SOURCES
    unit/test_main.cpp
    unit/test_agent_request.cpp
    unit/test_bubble_model.cpp
    unit/test_speaker_colors.cpp
    unit/test_markdown_text.cpp
    unit/test_deepseek_sse.cpp
    unit/test_status_line.cpp
    unit/test_approval_logic.cpp
    unit/test_curl_error_text.cpp
    unit/test_cli_args.cpp
    unit/test_delete_guard.cpp
    unit/test_config.cpp
    unit/test_approval_policy.cpp
    unit/test_log_files.cpp
    unit/test_snapshot.cpp
    unit/test_theme_map.cpp
    unit/test_theme_colors.cpp
    unit/test_skill_runner.cpp
    unit/test_skill_swarm.cpp
    unit/test_skill_engine.cpp
    unit/test_skill_monica.cpp
    unit/test_skill_rounds.cpp
    unit/test_shared_context.cpp
    unit/test_skill_sop.cpp
    unit/test_skill_trigger.cpp
    unit/test_web_api.cpp
    unit/test_image_input.cpp
    unit/test_prompt_upgrade.cpp
    unit/test_prompt_style.cpp
    unit/test_prompt_seed.cpp
    unit/test_turn_end.cpp
    unit/test_reasoning_content.cpp
    unit/test_file_tools.cpp
    unit/test_shell_output.cpp
    unit/test_wish.cpp
    unit/test_task_registry.cpp
    unit/test_slash_menu.cpp
    unit/test_help_text.cpp
    unit/test_ctrlc_guard.cpp
    unit/test_modal_focus.cpp
    unit/test_paste_flatten.cpp
    unit/test_tool_paths.cpp
    unit/test_capabilities.cpp
    unit/test_diff_capture.cpp
    unit/test_storage_diff.cpp
    unit/test_diff_parse.cpp
    unit/test_workspace_paths.cpp
    unit/test_skill_thinking.cpp
    unit/test_skill_parallel.cpp
    unit/test_skill_budget.cpp
    unit/test_token_estimate.cpp
    unit/test_agents_md.cpp
    unit/test_view_image.cpp
    unit/test_osc7501.cpp
    unit/test_install_shim.cpp
    unit/test_mcp.cpp
    unit/test_session_runtime.cpp
    unit/test_session_init.cpp
    unit/test_skill_deliverable.cpp
    # The whole-screen view model (design 08.5, D4): derivation + TUI parity.
    unit/test_view_model.cpp
    golden/test_tui_smoke.cpp
    golden/test_chat_render.cpp
    golden/test_todo_render.cpp
    golden/test_markdown_render.cpp
    golden/test_theme_editor.cpp
    # The studio's design system (tokens / layout / text fitting / the header's
    # button table) is plain C++ with no ImGui include, so the geometry the window
    # is built from is pinned here without a GPU, a window or a frame.
    unit/test_studio_design.cpp
    # What a drag covers: the ordering rule the surface, the highlight and the
    # clipboard all obey. It is pure arithmetic with no ImGui include, which is the
    # only reason it can be tested at all.
    unit/test_studio_select.cpp
    # Selection on RENDERED lines: per-character pixel offsets -> characters, and the
    # copy rule (soft-wrap join, no leak from an undrawn line). Also ImGui-free.
    unit/test_studio_copy.cpp
    # --screenshot writes a PNG with no image library; the bytes are pinned here.
    unit/test_png_writer.cpp
    unit/test_worker_wait.cpp
    ${CMAKE_SOURCE_DIR}/studio/png_writer.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/tokens.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/layout.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/fit.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/actions.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/file_tree.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/path_text.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/gauges.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/work_clock.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/text_select.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/text_runs.cpp
    ${SRC_DIR}/tui/speaker_colors.cpp
    ${SRC_DIR}/tui/chat_view.cpp
    ${SRC_DIR}/tui/diff_view.cpp
    ${SRC_DIR}/tui/markdown_view.cpp
    ${SRC_DIR}/tui/todo_view.cpp
    ${SRC_DIR}/tui/theme_map.cpp
    ${SRC_DIR}/tui/theme_editor_view.cpp
)

# -- MCP test fixture -----------------------------------------------------
# MCP is a process protocol, so the test spawns a real executable. It must be its
# own target: linking the code into proJV_tests would produce a "server" that is
# already inside the test process and never exercises a pipe.
add_executable(fake_mcp_server fakes/fake_mcp_server.cpp)
target_include_directories(fake_mcp_server PRIVATE ${EXTERNAL_DIR})
if(WIN32)
    target_compile_definitions(fake_mcp_server PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(MSVC)
    target_compile_options(fake_mcp_server PRIVATE /utf-8)
endif()

add_executable(proJV_tests ${TEST_SOURCES})

# test_mcp.cpp spawns the fixture. It resolves the executable next to the test
# binary at run time (fakeServerPath()) instead of baking an absolute path in:
# a Windows path inside a generated string literal turns its backslashes into
# escape sequences (\w, \p ...), and the two binaries always land side by side.
add_dependencies(proJV_tests fake_mcp_server)

target_include_directories(proJV_tests PRIVATE
    ${EXTERNAL_DIR}
    ${EXTERNAL_DIR}/doctest
    ${EXTERNAL_DIR}/FTXUI-7.0.3/include
    ${SRC_DIR}
    ${SRC_DIR}/client
    ${CMAKE_SOURCE_DIR}/studio
)

target_link_libraries(proJV_tests PRIVATE proJV_lib ftxui::dom ftxui::component)

# proJV_lib links SQLiteCpp PRIVATE, so its headers and symbols do not reach the
# tests. test_storage_diff opens a database directly to build a pre-migration
# schema, so it needs both. Scoped to the test target on purpose: making the lib
# link SQLiteCpp PUBLIC would leak the dependency into every consumer.
target_include_directories(proJV_tests PRIVATE ${EXTERNAL_DIR}/SQLiteCpp-3.3.3/include)
target_link_libraries(proJV_tests PRIVATE SQLiteCpp)

# Example skill repos live in <repo>/skills/ (e.g. monica).
target_compile_definitions(proJV_tests PRIVATE PROJV_EXAMPLE_SKILLS_DIR="${CMAKE_SOURCE_DIR}/skills")

if(WIN32)
    target_compile_definitions(proJV_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        # target-scoped: add_compile_options() after add_executable() would not
        # reach proJV_tests, and the test suite contains non-ASCII literals.
        target_compile_options(proJV_tests PRIVATE /utf-8)
    endif()
endif()

add_test(NAME proJV_tests COMMAND proJV_tests)

# --- Headless copy probe -------------------------------------------------
# No API key, no model, no GPU, no window: it drives the real studio::TextSurface
# through a headless ImGui context with real mouse/keyboard events and checks that a
# drag's clipboard text equals the glyphs it covered (the "copy does not match the
# selection" bug, especially over Chinese). The unit test above pins the arithmetic;
# this one pins the font: only a real face can show that a CJK advance is not two
# Latin cells.
add_executable(proJV_copy_probe
    ${CMAKE_SOURCE_DIR}/tests/copy_selftest.cpp
    ${CMAKE_SOURCE_DIR}/studio/studio_text.cpp
    ${CMAKE_SOURCE_DIR}/studio/studio_markdown.cpp
    ${CMAKE_SOURCE_DIR}/studio/studio_theme.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/tokens.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/fit.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/text_select.cpp
    ${CMAKE_SOURCE_DIR}/studio/design/text_runs.cpp
)
target_include_directories(proJV_copy_probe PRIVATE
    ${EXTERNAL_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/client
    ${CMAKE_SOURCE_DIR}/studio
)
target_link_libraries(proJV_copy_probe PRIVATE proJV_lib projv_imgui)
if(WIN32)
    target_compile_definitions(proJV_copy_probe PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(MSVC)
    target_compile_options(proJV_copy_probe PRIVATE /utf-8)
endif()
add_test(NAME studio_copy_probe COMMAND proJV_copy_probe)
