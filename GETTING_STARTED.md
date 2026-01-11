# Getting Started

## 프로젝트 구조 이해

이 프로젝트는 다음 단계로 개발됩니다:

### Phase 1: 기본 구조 (현재)
- ✅ 프로젝트 구조 생성
- ✅ 문서 작성
- ⏳ CMake 빌드 시스템 (proto만 컴파일 가능)

### Phase 2: GStreamer 기본
- StreamProcessor 클래스 구현
- 단일 RTSP 스트림 처리
- Hailo 추론 연동

### Phase 3: 멀티스트림
- StreamManager 구현
- 여러 스트림 동시 처리
- 에러 핸들링

### Phase 4: gRPC API
- gRPC 서버 구현
- API 테스트

### Phase 5: NATS 통합
- NATS 메시지 발행
- 전체 파이프라인 테스트

## 빠른 시작

### 1. 의존성 설치

```bash
cd /Users/ijongjin/snuailab/project/asdf
./scripts/install_deps.sh
```

**주의:** Hailo SDK는 수동으로 설치해야 합니다.
- https://hailo.ai/developer-zone/ 에서 다운로드
- `sudo dpkg -i hailort-*.deb` 로 설치

### 2. 프로토 컴파일 테스트

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

성공하면 다음과 같은 메시지가 표시됩니다:
```
Stream Daemon - Proto compiled successfully
```

### 3. GStreamer 테스트

```bash
# GStreamer 설치 확인
gst-launch-1.0 --version

# Hailo 플러그인 확인
gst-inspect-1.0 hailonet
gst-inspect-1.0 hailofilter

# 간단한 파이프라인 테스트
gst-launch-1.0 videotestsrc ! autovideosink
```

### 4. RTSP 테스트 환경 구축

MediaMTX로 로컬 RTSP 서버 생성:

```bash
# MediaMTX 다운로드
wget https://github.com/bluenviron/mediamtx/releases/latest/download/mediamtx_v1.5.1_linux_amd64.tar.gz
tar -xzf mediamtx_*.tar.gz

# 실행
./mediamtx &

# Webcam을 RTSP로 스트리밍
ffmpeg -f v4l2 -i /dev/video0 \
  -c:v libx264 -preset ultrafast \
  -f rtsp rtsp://localhost:8554/webcam
```

이제 `rtsp://localhost:8554/webcam`으로 테스트 가능합니다.

## 다음 단계: 코드 구현

### StreamProcessor 구현 시작

다음 순서로 개발을 진행하세요:

1. **include/stream_processor.h** 작성
2. **src/stream_processor.cpp** 작성
3. **테스트 메인 함수** 작성
4. **빌드 및 테스트**

### 예시 개발 흐름

```bash
# 1. 헤더 작성
vim include/stream_processor.h

# 2. 구현 작성
vim src/stream_processor.cpp

# 3. 간단한 main 작성
vim src/main.cpp

# 4. CMakeLists.txt 수정 (SOURCES에 추가)
vim CMakeLists.txt

# 5. 빌드
cd build
cmake ..
make -j$(nproc)

# 6. 실행
./stream_daemon
```

## 개발 도구 설정

### VSCode

```bash
# C++ 확장 설치
code --install-extension ms-vscode.cpptools
code --install-extension ms-vscode.cmake-tools

# 프로젝트 열기
code /Users/ijongjin/snuailab/project/asdf
```

**설정 파일 (.vscode/settings.json):**
```json
{
    "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools",
    "cmake.buildDirectory": "${workspaceFolder}/build",
    "files.associations": {
        "*.h": "cpp",
        "*.cpp": "cpp"
    }
}
```

### 디버깅

```bash
# Debug 빌드
mkdir build-debug
cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)

# GDB로 디버깅
gdb ./stream_daemon

# 또는 VSCode 디버거 사용
```

## 코딩 스타일

### Google C++ Style Guide 준수

```cpp
// 네이밍
class StreamProcessor {};  // PascalCase for classes
void processFrame() {}     // camelCase for methods
std::string rtsp_url_;     // snake_case_ for members

// 들여쓰기: 2 spaces
if (condition) {
  doSomething();
}

// 포인터/참조
int* ptr;      // *와 타입 붙임
int& ref;      // &와 타입 붙임
```

### 헤더 가드

```cpp
#ifndef STREAM_PROCESSOR_H_
#define STREAM_PROCESSOR_H_

// ... code ...

#endif  // STREAM_PROCESSOR_H_
```

## 테스트

### 단위 테스트 (향후)

```bash
# Google Test 사용
cmake -DENABLE_TESTS=ON ..
make -j$(nproc)
ctest --output-on-failure
```

### 통합 테스트

```bash
# RTSP 서버 시작
./mediamtx &

# Daemon 시작
./stream_daemon &

# 스트림 추가
grpcurl -plaintext -d '{
  "stream_id": "test",
  "rtsp_url": "rtsp://localhost:8554/webcam",
  "hef_path": "/path/to/model.hef"
}' localhost:50051 stream.StreamService/AddStream

# NATS 메시지 확인
nats sub "stream.test.detections"
```

## 문제 해결

### GStreamer 파이프라인 디버깅

```bash
# 로그 레벨 설정
export GST_DEBUG=3

# 특정 플러그인 디버깅
export GST_DEBUG=hailonet:5

# 파이프라인 그래프 생성
export GST_DEBUG_DUMP_DOT_DIR=/tmp
# → .dot 파일 생성됨
```

### Memory Leak 체크

```bash
# Valgrind 사용
valgrind --leak-check=full ./stream_daemon
```

### 성능 프로파일링

```bash
# perf 사용
perf record -g ./stream_daemon
perf report
```

## 참고 자료

- [GStreamer 문서](https://gstreamer.freedesktop.org/documentation/)
- [Hailo 문서](https://hailo.ai/developer-zone/documentation/)
- [gRPC C++ 튜토리얼](https://grpc.io/docs/languages/cpp/quickstart/)
- [NATS C Client](https://github.com/nats-io/nats.c)

## 커뮤니티

- GitHub Issues: 버그 리포트 및 기능 요청
- Discussions: 질문 및 아이디어 공유

---

**Happy Coding! 🚀**
