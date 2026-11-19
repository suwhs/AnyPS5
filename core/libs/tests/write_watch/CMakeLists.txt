cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(WriteWatchTests LANGUAGES CXX)
    enable_testing()
    set(ANYPS5_TEST_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

find_package(Threads REQUIRED)

if(APPLE)
    add_executable(write_watch_backing_tests ../WriteWatchBacking.cpp ../../prx/libc/src/GuestWriteWatch.cpp)
    target_include_directories(write_watch_backing_tests PRIVATE ../..)
    target_compile_features(write_watch_backing_tests PRIVATE cxx_std_20)
    target_link_libraries(write_watch_backing_tests PRIVATE Threads::Threads)
    set_target_properties(write_watch_backing_tests PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "$<1:${ANYPS5_TEST_OUTPUT_DIRECTORY}>")

    foreach(mode shared private writable late restore)
        add_test(NAME write_watch_backing_${mode} COMMAND write_watch_backing_tests ${mode})
        set_tests_properties(write_watch_backing_${mode} PROPERTIES TIMEOUT 30)
    endforeach()
endif()

add_executable(write_watch_tests ../WriteWatch.cpp ../../prx/libc/src/GuestWriteWatch.cpp)
target_include_directories(write_watch_tests PRIVATE ../..)
target_compile_features(write_watch_tests PRIVATE cxx_std_20)
target_link_libraries(write_watch_tests PRIVATE Threads::Threads)
set_target_properties(write_watch_tests PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "$<1:${ANYPS5_TEST_OUTPUT_DIRECTORY}>")

if(APPLE)
    foreach(mode collect fresh armed register mapped host empty protection overlaps threads raised fault-ahead read-trap read-trap-threads read-trap-holder read-trap-fault read-trap-late read-trap-fault-bound execute-fault read-fault)
        add_test(NAME write_watch_${mode} COMMAND write_watch_tests ${mode})
        set_tests_properties(write_watch_${mode} PROPERTIES TIMEOUT 30)
    endforeach()

    add_test(NAME write_watch_fault-ahead-off COMMAND write_watch_tests fault-ahead-off)
    set_tests_properties(write_watch_fault-ahead-off PROPERTIES TIMEOUT 30 ENVIRONMENT APS5_WRITE_WATCH_FAULT_AHEAD=0)
else()
    foreach(mode read-trap read-trap-threads read-trap-holder read-trap-fault)
        add_test(NAME write_watch_${mode} COMMAND write_watch_tests ${mode})
        set_tests_properties(write_watch_${mode} PROPERTIES TIMEOUT 30)
    endforeach()

    foreach(mode read-trap read-trap-late)
        add_test(NAME write_watch_${mode}-tiny-limit COMMAND write_watch_tests ${mode})
        set_tests_properties(write_watch_${mode}-tiny-limit PROPERTIES TIMEOUT 30 ENVIRONMENT APS5_WRITE_WATCH_PAGE_STATE_LIMIT=1)
    endforeach()

    add_test(NAME write_watch_read-trap-trim COMMAND write_watch_tests read-trap-trim)
    set_tests_properties(write_watch_read-trap-trim PROPERTIES TIMEOUT 120 ENVIRONMENT APS5_WRITE_WATCH_PAGE_STATE_LIMIT=1)

    add_test(NAME write_watch_read-trap-late-no-retry COMMAND write_watch_tests read-trap-late)
    set_tests_properties(write_watch_read-trap-late-no-retry PROPERTIES TIMEOUT 30 ENVIRONMENT APS5_WRITE_WATCH_UNTRAPPED_RETRIES=0 WILL_FAIL TRUE)
endif()
