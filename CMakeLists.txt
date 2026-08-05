cmake_minimum_required(VERSION 3.14)
project(person_detector VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

set(PD_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wshadow
    -Wconversion
    -Wstrict-prototypes
    -Wmissing-prototypes
)

set(PD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(PD_COMMON_SOURCES
    src/core/aggregator.c
    src/core/estimator.c
    src/core/replay.c
    src/utils/logger.c
)

set(PD_SCANNER_SOURCES
    src/ble/ble_scanner.c
    src/wifi/wifi_scanner.c
)

find_package(Threads REQUIRED)

# find_library(... REQUIRED) needs CMake 3.18, above this project's floor, and
# libm is bundled into libc on some toolchains where the search legitimately
# fails. Fall back to the linker's own resolution rather than aborting.
find_library(MATH_LIBRARY m)
if(NOT MATH_LIBRARY)
    set(MATH_LIBRARY m)
endif()

find_library(BLUETOOTH_LIBRARY bluetooth)
find_library(NL3_LIBRARY nl-3)
find_library(NL_GENL3_LIBRARY nl-genl-3)

function(pd_apply_warnings target_name)
    target_compile_options(${target_name} PRIVATE ${PD_WARNING_FLAGS})
endfunction()

add_executable(person_detector
    src/main.c
    ${PD_COMMON_SOURCES}
    ${PD_SCANNER_SOURCES}
)

target_include_directories(person_detector PRIVATE ${PD_INCLUDE_DIR})
pd_apply_warnings(person_detector)

target_link_libraries(person_detector
    PRIVATE
        Threads::Threads
        ${MATH_LIBRARY}
)

if(BLUETOOTH_LIBRARY)
    target_link_libraries(person_detector PRIVATE ${BLUETOOTH_LIBRARY})
else()
    message(WARNING "libbluetooth not found; BLE scanner will fail to link on this system")
endif()

if(NL3_LIBRARY AND NL_GENL3_LIBRARY)
    target_link_libraries(person_detector PRIVATE ${NL3_LIBRARY} ${NL_GENL3_LIBRARY})
else()
    message(STATUS "libnl-3/libnl-genl-3 not found; continuing without explicit nl linkage")
endif()

add_executable(test_estimator
    tests/test_estimator.c
    ${PD_COMMON_SOURCES}
)

target_include_directories(test_estimator PRIVATE ${PD_INCLUDE_DIR})
pd_apply_warnings(test_estimator)
target_link_libraries(test_estimator PRIVATE Threads::Threads ${MATH_LIBRARY})

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(person_detector PRIVATE -g -O0 -fsanitize=address,undefined)
    target_link_options(person_detector PRIVATE -fsanitize=address,undefined)
    target_compile_options(test_estimator PRIVATE -g -O0 -fsanitize=address,undefined)
    target_link_options(test_estimator PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME estimator_unit COMMAND test_estimator)

install(TARGETS person_detector RUNTIME DESTINATION bin)
install(FILES scripts/person-detector.service DESTINATION lib/systemd/system)
