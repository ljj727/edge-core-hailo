# Build Guide

## 시스템 요구사항

### 운영체제
- Ubuntu 22.04 LTS (추천)
- Ubuntu 20.04 LTS
- Debian 11+

### 하드웨어
- CPU: x86_64, 4코어 이상
- RAM: 8GB 이상
- Storage: 10GB 여유 공간
- Hailo-8 NPU (필수)

### 소프트웨어
- GCC 9+ 또는 Clang 10+
- CMake 3.20+
- GStreamer 1.20+
- Hailo SDK & HailoRT

## 의존성 설치

### Ubuntu 22.04

```bash
#!/bin/bash
# scripts/install_deps.sh

# 시스템 업데이트
sudo apt update && sudo apt upgrade -y

# 빌드 도구
sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    git

# GStreamer
sudo apt install -y \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-tools

# gRPC 및 Protocol Buffers
sudo apt install -y \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc

# NATS C Client
# GitHub에서 빌드 필요 (아래 참조)

# JSON 라이브러리
sudo apt install -y nlohmann-json3-dev

# Hailo SDK 설치 (별도 다운로드 필요)
# https://hailo.ai/developer-zone/
```

### NATS C Client 빌드

```bash
# NATS C Client 설치
cd /tmp
git clone https://github.com/nats-io/nats.c.git
cd nats.c
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
sudo ldconfig
```

### Hailo SDK 설치

```bash
# Hailo SDK 다운로드 (HailoRT)
# https://hailo.ai/developer-zone/software-downloads/

# 예시: HailoRT 4.17.0
wget https://hailo.ai/.../hailort-4.17.0-Linux.deb
sudo dpkg -i hailort-4.17.0-Linux.deb

# GStreamer Hailo 플러그인
sudo apt install -y gstreamer1.0-hailort

# 설치 확인
gst-inspect-1.0 hailonet
gst-inspect-1.0 hailofilter
```

## 프로젝트 빌드

### 1. 소스 코드 클론

```bash
cd /Users/ijongjin/snuailab/project
git clone <repository-url> asdf
cd asdf
```

### 2. 빌드 디렉토리 생성

```bash
mkdir build
cd build
```

### 3. CMake 구성

```bash
# Debug 빌드
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Release 빌드 (프로덕션)
cmake -DCMAKE_BUILD_TYPE=Release ..

# 특정 옵션 지정
cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_TESTS=ON \
  -DENABLE_LOGGING=ON \
  ..
```

### 4. 컴파일

```bash
# 병렬 빌드
make -j$(nproc)

# 또는 단일 스레드 (디버깅 시)
make
```

### 5. 빌드 결과 확인

```bash
# 실행 파일 확인
ls -lh stream_daemon

# 의존성 확인
ldd stream_daemon
```

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(stream_daemon VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 옵션
option(ENABLE_TESTS "Build tests" OFF)
option(ENABLE_LOGGING "Enable verbose logging" ON)

# GStreamer 찾기
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GSTREAMER_APP REQUIRED gstreamer-app-1.0)

# gRPC 및 Protobuf
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

# NATS
find_library(NATS_LIB nats)
if(NOT NATS_LIB)
    message(FATAL_ERROR "NATS C library not found")
endif()

# JSON
find_package(nlohmann_json 3.2.0 REQUIRED)

# 소스 파일
set(SOURCES
    src/main.cpp
    src/stream_manager.cpp
    src/stream_processor.cpp
    src/grpc_server.cpp
    src/nats_publisher.cpp
)

# Protobuf 생성
set(PROTO_FILES proto/stream.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
grpc_generate_cpp(GRPC_SRCS GRPC_HDRS ${PROTO_FILES})

# 실행 파일
add_executable(stream_daemon
    ${SOURCES}
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

# Include 디렉토리
target_include_directories(stream_daemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}  # Protobuf 생성 파일
    ${GSTREAMER_INCLUDE_DIRS}
)

# 링크 라이브러리
target_link_libraries(stream_daemon PRIVATE
    ${GSTREAMER_LIBRARIES}
    ${GSTREAMER_APP_LIBRARIES}
    gRPC::grpc++
    protobuf::libprotobuf
    ${NATS_LIB}
    nlohmann_json::nlohmann_json
    pthread
)

# 컴파일 옵션
target_compile_options(stream_daemon PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)

# 설치
install(TARGETS stream_daemon DESTINATION bin)
```

## 빌드 문제 해결

### GStreamer 못 찾음

```bash
# pkg-config 경로 확인
export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH

# GStreamer 설치 확인
pkg-config --modversion gstreamer-1.0
```

### gRPC 못 찾음

```bash
# Ubuntu 22.04에서 gRPC 수동 설치
sudo apt install -y \
    libabsl-dev \
    libre2-dev

git clone --recurse-submodules -b v1.58.0 \
    https://github.com/grpc/grpc
cd grpc
mkdir -p cmake/build
cd cmake/build
cmake -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      ../..
make -j$(nproc)
sudo make install
```

### NATS 라이브러리 못 찾음

```bash
# 설치 위치 확인
sudo find / -name "libnats.*"

# ldconfig 갱신
sudo ldconfig

# 또는 LD_LIBRARY_PATH 설정
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

### Hailo 플러그인 못 찾음

```bash
# GStreamer 플러그인 경로 확인
export GST_PLUGIN_PATH=/opt/hailo/tappas/plugins:$GST_PLUGIN_PATH

# 플러그인 등록 확인
gst-inspect-1.0 | grep hailo
```

## 개발 환경 설정

### VSCode

```json
// .vscode/settings.json
{
    "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools",
    "cmake.buildDirectory": "${workspaceFolder}/build",
    "cmake.configureOnOpen": true
}
```

### CLion

1. Open Project → CMakeLists.txt 선택
2. Settings → Build → CMake 확인
3. Build → Build Project

## 테스트 빌드

```bash
# 테스트 활성화
cmake -DENABLE_TESTS=ON ..
make -j$(nproc)

# 테스트 실행
ctest --output-on-failure
```

## Docker 빌드

```bash
# Dockerfile로 빌드
cd docker
docker build -t stream-daemon:latest .

# 실행
docker run --device /dev/video0 \
    -p 50051:50051 \
    stream-daemon:latest
```

## 배포 빌드

```bash
# Release 모드 + 최적화
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-march=native" \
      ..
make -j$(nproc)

# Strip symbols (바이너리 크기 감소)
strip stream_daemon

# 크기 확인
ls -lh stream_daemon

# 배포 패키지 생성
cpack
```

## 크로스 컴파일 (Jetson 등)

```bash
# ARM64용 크로스 컴파일
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm64-toolchain.cmake ..
make -j$(nproc)
```

## 빌드 캐시 정리

```bash
# 빌드 디렉토리 삭제
rm -rf build

# CMake 캐시만 삭제
cd build
rm CMakeCache.txt
cmake ..
```
