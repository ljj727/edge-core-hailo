cmake_minimum_required(VERSION 3.16)
project(stream_daemon VERSION 1.0.0 LANGUAGES CXX)

# ============================================================================
# C++ 표준 설정
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# 빌드 옵션
# ============================================================================
option(ENABLE_TESTS "Build tests" OFF)
option(ENABLE_DEBUG_LOGGING "Enable debug logging" ON)
option(ENABLE_SANITIZERS "Enable address/undefined sanitizers" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# 빌드 타입 기본값
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ============================================================================
# Sanitizers (for debugging)
# ============================================================================
if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${SANITIZER_FLAGS}")
    set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} ${SANITIZER_FLAGS}")
    message(STATUS "Sanitizers enabled")
endif()

# ============================================================================
# 의존성 찾기
# ============================================================================

# PkgConfig 필수
find_package(PkgConfig REQUIRED)

# GStreamer
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0>=1.16)
pkg_check_modules(GSTREAMER_APP REQUIRED gstreamer-app-1.0>=1.16)
pkg_check_modules(GSTREAMER_VIDEO REQUIRED gstreamer-video-1.0>=1.16)
message(STATUS "GStreamer version: ${GSTREAMER_VERSION}")

# gRPC 및 Protobuf
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG QUIET)

if(NOT gRPC_FOUND)
    message(STATUS "gRPC not found via CONFIG, trying PkgConfig...")
    pkg_check_modules(GRPC REQUIRED grpc++)
endif()

message(STATUS "Protobuf version: ${Protobuf_VERSION}")

# NATS C 라이브러리
find_library(NATS_LIB nats PATHS /usr/local/lib /usr/lib)
find_path(NATS_INCLUDE_DIR nats/nats.h PATHS /usr/local/include /usr/include)

if(NOT NATS_LIB)
    message(FATAL_ERROR "NATS C library not found. Install from https://github.com/nats-io/nats.c")
endif()
message(STATUS "NATS library: ${NATS_LIB}")

# JSON 라이브러리
find_package(nlohmann_json 3.2.0 REQUIRED)

# YAML 설정 파일 라이브러리
find_package(yaml-cpp REQUIRED)
message(STATUS "yaml-cpp version: ${yaml-cpp_VERSION}")

# ZIP 라이브러리 (모델 업로드용)
pkg_check_modules(LIBZIP REQUIRED libzip)
message(STATUS "libzip version: ${LIBZIP_VERSION}")

# JPEG 라이브러리 (프레임 인코딩용)
find_package(JPEG REQUIRED)
message(STATUS "JPEG library: ${JPEG_LIBRARIES}")

# ============================================================================
# Protobuf/gRPC 코드 생성
# ============================================================================
set(PROTO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILES "${PROTO_PATH}/detector.proto")

set(PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/detector.pb.cc")
set(PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/detector.pb.h")
set(GRPC_SRCS "${CMAKE_CURRENT_BINARY_DIR}/detector.grpc.pb.cc")
set(GRPC_HDRS "${CMAKE_CURRENT_BINARY_DIR}/detector.grpc.pb.h")

find_program(PROTOC protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

# Protobuf 생성
add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
    COMMAND ${PROTOC}
    ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
         -I${PROTO_PATH}
         ${PROTO_FILES}
    DEPENDS ${PROTO_FILES}
    COMMENT "Generating protobuf C++ files"
)

# gRPC 생성
add_custom_command(
    OUTPUT ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${PROTOC}
    ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
         --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
         -I${PROTO_PATH}
         ${PROTO_FILES}
    DEPENDS ${PROTO_FILES}
    COMMENT "Generating gRPC C++ files"
)

# Proto 라이브러리 (재사용을 위해 분리)
add_library(stream_proto STATIC
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

target_include_directories(stream_proto PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(stream_proto PUBLIC
    protobuf::libprotobuf
)

if(gRPC_FOUND)
    target_link_libraries(stream_proto PUBLIC gRPC::grpc++)
else()
    target_link_libraries(stream_proto PUBLIC ${GRPC_LIBRARIES})
    target_include_directories(stream_proto PUBLIC ${GRPC_INCLUDE_DIRS})
endif()

# ============================================================================
# 코어 라이브러리
# ============================================================================
set(CORE_SOURCES
    src/common.cpp
    src/config.cpp
    src/model_registry.cpp
    src/nats_publisher.cpp
    src/hailo_inference.cpp
    src/batch_inference_manager.cpp
    src/event_compositor.cpp
    src/stream_processor.cpp
    src/stream_manager.cpp
    src/grpc_server.cpp
)

add_library(stream_daemon_core STATIC ${CORE_SOURCES})

target_include_directories(stream_daemon_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${GSTREAMER_INCLUDE_DIRS}
    ${NATS_INCLUDE_DIR}
    ${LIBZIP_INCLUDE_DIRS}
    ${JPEG_INCLUDE_DIR}
    /usr/include/hailo
    /usr/include/gstreamer-1.0/gst/hailo
)

target_link_libraries(stream_daemon_core PUBLIC
    stream_proto
    ${GSTREAMER_LIBRARIES}
    ${GSTREAMER_APP_LIBRARIES}
    ${GSTREAMER_VIDEO_LIBRARIES}
    /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgsthailo.so
    /usr/lib/libhailort.so
    ${NATS_LIB}
    nlohmann_json::nlohmann_json
    yaml-cpp
    ${LIBZIP_LIBRARIES}
    ${JPEG_LIBRARIES}
    pthread
)

# 컴파일 정의
if(ENABLE_DEBUG_LOGGING)
    target_compile_definitions(stream_daemon_core PUBLIC DEBUG_LOGGING)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(stream_daemon_core PUBLIC DEBUG)
endif()

# 컴파일 옵션
target_compile_options(stream_daemon_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)

# ============================================================================
# 실행 파일
# ============================================================================
add_executable(stream_daemon src/main.cpp)

target_link_libraries(stream_daemon PRIVATE stream_daemon_core)

target_compile_options(stream_daemon PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)

# ============================================================================
# 테스트 (선택적)
# ============================================================================
if(ENABLE_TESTS)
    enable_testing()

    # Google Test 찾기
    find_package(GTest QUIET)

    if(GTest_FOUND)
        message(STATUS "Google Test found, building tests")

        # 단위 테스트
        add_executable(unit_tests
            tests/test_common.cpp
            tests/test_nats_publisher.cpp
            tests/test_stream_manager.cpp
            tests/test_mock_components.cpp
        )

        target_link_libraries(unit_tests PRIVATE
            stream_daemon_core
            GTest::gtest
            GTest::gtest_main
        )

        target_include_directories(unit_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        )

        add_test(NAME unit_tests COMMAND unit_tests)

        # 통합 테스트 (GStreamer 없이)
        add_executable(integration_tests
            tests/test_integration.cpp
        )

        target_link_libraries(integration_tests PRIVATE
            stream_daemon_core
            GTest::gtest
            GTest::gtest_main
        )

        add_test(NAME integration_tests COMMAND integration_tests)

    else()
        message(STATUS "Google Test not found, tests disabled")
    endif()
endif()

# ============================================================================
# 설치
# ============================================================================
install(TARGETS stream_daemon
    RUNTIME DESTINATION bin
)

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/detector.proto
    DESTINATION share/stream_daemon/proto
)

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/config.yaml
    DESTINATION etc/stream_daemon
    RENAME config.yaml.example
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION include/stream_daemon
    FILES_MATCHING PATTERN "*.h"
)

# ============================================================================
# 요약
# ============================================================================
message(STATUS "")
message(STATUS "╔═══════════════════════════════════════════════════════════╗")
message(STATUS "║         Stream Daemon Configuration Summary               ║")
message(STATUS "╠═══════════════════════════════════════════════════════════╣")
message(STATUS "║  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "║  C++ compiler:      ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "║  C++ standard:      ${CMAKE_CXX_STANDARD}")
message(STATUS "║  GStreamer:         ${GSTREAMER_VERSION}")
message(STATUS "║  Protobuf:          ${Protobuf_VERSION}")
message(STATUS "║  NATS:              ${NATS_LIB}")
message(STATUS "║  yaml-cpp:          ${yaml-cpp_VERSION}")
message(STATUS "║  libzip:            ${LIBZIP_VERSION}")
message(STATUS "║  libjpeg:           ${JPEG_LIBRARIES}")
message(STATUS "║  Tests:             ${ENABLE_TESTS}")
message(STATUS "║  Debug logging:     ${ENABLE_DEBUG_LOGGING}")
message(STATUS "║  Sanitizers:        ${ENABLE_SANITIZERS}")
message(STATUS "║  Install prefix:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "╚═══════════════════════════════════════════════════════════╝")
message(STATUS "")
