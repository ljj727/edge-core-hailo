# Testing Strategy

## 테스트 레벨

### 1. Unit Tests (단위 테스트)

GStreamer/Hailo 없이 순수 로직 테스트

```bash
# 빌드 및 실행
cmake -DENABLE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
./unit_tests
```

**테스트 대상:**
- `common.h` - 데이터 구조, Result 타입, 유틸리티
- `mock_components.h` - Mock 객체 동작
- 순수 비즈니스 로직

### 2. Integration Tests (통합 테스트)

GStreamer 초기화 필요, Hailo 없이 가능

```bash
./integration_tests
```

**테스트 대상:**
- GStreamer 파이프라인 생성/검증
- 테스트 파이프라인 실행
- 디버그 유틸리티

### 3. System Tests (시스템 테스트)

전체 스택 필요 (NATS, RTSP 소스, Hailo)

```bash
# 환경 설정
docker run -d -p 4222:4222 nats
./mediamtx &

# 데몬 실행
./stream_daemon --check-plugins
./stream_daemon
```

## Mock 기반 테스트

### IMessagePublisher 인터페이스

```cpp
#include "mock_components.h"

// NATS 없이 메시지 발행 테스트
auto mock = std::make_shared<MockMessagePublisher>();
mock->Connect();

DetectionEvent event;
event.stream_id = "test";
mock->Publish(event);

auto events = mock->GetPublishedEvents();
EXPECT_EQ(events.size(), 1);
```

### IStreamProcessor 인터페이스

```cpp
// GStreamer 없이 스트림 처리 테스트
StreamInfo info{"test", "rtsp://mock", "/model.hef"};
MockStreamProcessor processor(info);

processor.Start();
EXPECT_TRUE(processor.IsRunning());

// 탐지 시뮬레이션
DetectionEvent event;
event.detections.push_back(Detection{"person", 0, 0.95f, {0,0,100,200}});
processor.SimulateDetection(event);
```

### MockStreamProcessorFactory

```cpp
// 팩토리 패턴으로 의존성 주입
MockStreamProcessorFactory factory;
auto processor = factory.Create(info, mock_publisher);

// 생성된 프로세서 추적
auto created = factory.GetCreatedProcessors();
```

## 디버깅 도구

### GStreamer 디버그 레벨

```cpp
#include "debug_utils.h"

// 전역 디버그 레벨 설정
debug::GStreamerDebug::SetDebugLevel(3);

// 특정 플러그인만
debug::GStreamerDebug::SetDebugLevel(5, "hailonet:5,rtspsrc:4");
```

### 파이프라인 DOT 그래프

```cpp
// DOT 파일 생성 활성화
debug::GStreamerDebug::EnableDotFileGeneration("/tmp/gst-dots");

// 수동 덤프
debug::GStreamerDebug::DumpPipelineToDot(pipeline, "my_pipeline");

// 이미지로 변환
// dot -Tpng /tmp/gst-dots/my_pipeline.dot > pipeline.png
```

### 파이프라인 상태 확인

```cpp
auto state = debug::GStreamerDebug::GetPipelineState(pipeline);
auto elements = debug::GStreamerDebug::ListPipelineElements(pipeline);
```

### 플러그인 확인

```cpp
// Hailo 플러그인 확인
bool available = debug::GStreamerDebug::CheckHailoPlugins();

// CLI에서
./stream_daemon --check-plugins
```

## 테스트 파이프라인

### 1. 기본 테스트 (GStreamer만)

```cpp
auto pipeline = debug::TestPipelineBuilder::BuildTestPipeline(true);
// videotestsrc ! videoconvert ! fakesink
```

### 2. RTSP 테스트 (Hailo 없이)

```cpp
auto pipeline = debug::TestPipelineBuilder::BuildRtspTestPipeline(
    "rtsp://localhost:8554/test", true);
```

### 3. Hailo 테스트

```cpp
auto pipeline = debug::TestPipelineBuilder::BuildHailoTestPipeline(
    "/path/to/model.hef");
```

### 4. 파이프라인 검증

```cpp
auto result = debug::TestPipelineBuilder::ValidatePipeline(pipeline_str);
if (IsError(result)) {
    std::cerr << GetError(result) << std::endl;
}
```

## 성능 프로파일링

```cpp
debug::PerformanceProfiler profiler;

// 프레임마다 기록
profiler.RecordFrame(fps, latency_ms, is_dropped);

// 통계 확인
auto stats = profiler.GetStats();
std::cout << "Avg FPS: " << stats.avg_fps << std::endl;
std::cout << "Dropped: " << stats.dropped_frames << std::endl;

// 리포트 출력
std::cout << profiler.GetReport();
```

## CI/CD 테스트 전략

### GitHub Actions 예시

```yaml
jobs:
  unit-tests:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v3
      - name: Install deps
        run: |
          sudo apt-get update
          sudo apt-get install -y libgstreamer1.0-dev \
            libgstreamer-plugins-base1.0-dev \
            libgrpc++-dev libprotobuf-dev \
            nlohmann-json3-dev libgtest-dev
      - name: Build
        run: |
          mkdir build && cd build
          cmake -DENABLE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug ..
          make -j$(nproc)
      - name: Run tests
        run: cd build && ctest --output-on-failure

  integration-tests:
    runs-on: ubuntu-22.04
    services:
      nats:
        image: nats:latest
        ports:
          - 4222:4222
    steps:
      - uses: actions/checkout@v3
      - name: Install deps
        run: # ... GStreamer + NATS 설치
      - name: Run integration tests
        run: cd build && ./integration_tests
```

## 수동 테스트 체크리스트

### 기본 기능

- [ ] 데몬 시작/종료
- [ ] gRPC 연결
- [ ] NATS 연결
- [ ] 스트림 추가/제거/업데이트

### 스트림 처리

- [ ] RTSP 연결
- [ ] Hailo 추론
- [ ] 탐지 결과 발행
- [ ] FPS 유지

### 장애 복구

- [ ] RTSP 연결 끊김 → 자동 재연결
- [ ] NATS 연결 끊김 → 재연결
- [ ] 잘못된 HEF → 에러 처리

### 동시성

- [ ] 4개 스트림 동시 처리
- [ ] gRPC 동시 요청
- [ ] 스트림 추가 중 제거

## Sanitizer 사용

```bash
# Address Sanitizer로 빌드
cmake -DENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)

# 실행 (메모리 오류 감지)
./stream_daemon

# Valgrind (더 느리지만 상세함)
valgrind --leak-check=full ./stream_daemon
```

## 부하 테스트

```bash
# 다수의 gRPC 요청
for i in {1..100}; do
  grpcurl -plaintext localhost:50051 stream.StreamService/GetAllStatus &
done
wait

# 스트림 빠른 추가/제거
for i in {1..10}; do
  grpcurl -plaintext -d "{\"stream_id\": \"test_$i\", \"rtsp_url\": \"rtsp://test\", \"hef_path\": \"/model.hef\"}" \
    localhost:50051 stream.StreamService/AddStream
  grpcurl -plaintext -d "{\"stream_id\": \"test_$i\"}" \
    localhost:50051 stream.StreamService/RemoveStream
done
```
