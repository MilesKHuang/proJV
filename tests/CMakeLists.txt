# proJV unit tests (doctest, vendored at external/doctest/doctest.h)

set(TEST_SOURCES
    unit/test_main.cpp
    unit/test_agent_request.cpp
)

add_executable(proJV_tests ${TEST_SOURCES})

target_include_directories(proJV_tests PRIVATE
    ${EXTERNAL_DIR}
    ${EXTERNAL_DIR}/doctest
    ${SRC_DIR}
    ${SRC_DIR}/client
)

target_link_libraries(proJV_tests PRIVATE proJV_lib)

if(WIN32)
    target_compile_definitions(proJV_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    endif()
endif()

add_test(NAME proJV_tests COMMAND proJV_tests)
