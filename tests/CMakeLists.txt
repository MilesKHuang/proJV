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
    unit/test_theme_map.cpp
    unit/test_theme_colors.cpp
    unit/test_skill_runner.cpp
    unit/test_skill_swarm.cpp
    unit/test_skill_engine.cpp
    unit/test_skill_maker.cpp
    unit/test_skill_monica.cpp
    unit/test_skill_trigger.cpp
    unit/test_web_api.cpp
    unit/test_image_input.cpp
    unit/test_prompt_upgrade.cpp
    unit/test_prompt_style.cpp
    unit/test_turn_end.cpp
    unit/test_reasoning_content.cpp
    unit/test_file_tools.cpp
    unit/test_skill_thinking.cpp
    golden/test_tui_smoke.cpp
    golden/test_chat_render.cpp
    golden/test_todo_render.cpp
    golden/test_markdown_render.cpp
    golden/test_theme_editor.cpp
    ${SRC_DIR}/tui/bubble_model.cpp
    ${SRC_DIR}/tui/speaker_colors.cpp
    ${SRC_DIR}/tui/chat_view.cpp
    ${SRC_DIR}/tui/markdown_text.cpp
    ${SRC_DIR}/tui/markdown_view.cpp
    ${SRC_DIR}/tui/status_line.cpp
    ${SRC_DIR}/tui/status_bar.cpp
    ${SRC_DIR}/tui/todo_view.cpp
    ${SRC_DIR}/tui/approval_logic.cpp
    ${SRC_DIR}/tui/theme_map.cpp
    ${SRC_DIR}/tui/theme_colors.cpp
    ${SRC_DIR}/tui/theme_manager.cpp
    ${SRC_DIR}/tui/theme_editor_view.cpp
)

add_executable(proJV_tests ${TEST_SOURCES})

target_include_directories(proJV_tests PRIVATE
    ${EXTERNAL_DIR}
    ${EXTERNAL_DIR}/doctest
    ${EXTERNAL_DIR}/FTXUI-7.0.3/include
    ${SRC_DIR}
    ${SRC_DIR}/client
)

target_link_libraries(proJV_tests PRIVATE proJV_lib ftxui::dom ftxui::component)

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
