# Architecture Design

## 시스템 구성 요소

### 1. StreamManager

**책임:**
- 모든 스트림의 생명주기 관리
- 스트림 추가/제거/업데이트
- GLib MainLoop 관리

**주요 메서드:**
```cpp
class StreamManager {
public:
    bool addStream(const std::string& streamId, const StreamConfig& config);
    bool removeStream(const std::string& streamId);
    bool updateStream(const std::string& streamId, const StreamConfig& config);
    StreamStatus getStatus(const std::string& streamId);
    std::vector<StreamStatus> getAllStatus();
    
    void start();  // GLib MainLoop 시작
    void stop();   // 모든 스트림 정리 및 종료
    
private:
    std::map<std::string, std::unique_ptr<StreamProcessor>> streams_;
    GMainLoop* mainLoop_;
    std::mutex streamsMutex_;
};
```

### 2. StreamProcessor

**책임:**
- 개별 스트림의 GStreamer 파이프라인 관리
- Hailo 추론 결과 처리
- 에러 핸들링 및 재연결

**GStreamer 파이프라인:**
```
rtspsrc location={rtsp_url} latency=0 timeout=10000000 retry=3
  ! rtph264depay
  ! h264parse
  ! avdec_h264
  ! videoconvert
  ! hailonet hef-path={hef_path}
  ! hailofilter function-name=yolov8
  ! appsink name=sink emit-signals=true max-buffers=1 drop=true
```

**주요 메서드:**
```cpp
class StreamProcessor {
public:
    StreamProcessor(const std::string& streamId, 
                   const std::string& rtspUrl,
                   const std::string& hefPath,
                   NatsPublisher* natsPublisher);
    
    ~StreamProcessor();
    
    bool start();
    void stop();
    
    StreamStatus getStatus() const;
    
private:
    // GStreamer callbacks
    static GstPadProbeReturn probeCallback(GstPad* pad, 
                                          GstPadProbeInfo* info,
                                          gpointer userData);
    
    static void onErrorMessage(GstBus* bus, GstMessage* msg, gpointer userData);
    static void onEosMessage(GstBus* bus, GstMessage* msg, gpointer userData);
    
    // 재연결 로직
    void scheduleReconnect();
    static gboolean reconnectCallback(gpointer userData);
    
    std::string streamId_;
    std::string rtspUrl_;
    std::string hefPath_;
    
    GstElement* pipeline_;
    GstBus* bus_;
    
    NatsPublisher* natsPublisher_;
    
    std::atomic<StreamState> state_;
    std::atomic<uint64_t> frameCount_;
    std::chrono::steady_clock::time_point startTime_;
};
```

### 3. NatsPublisher

**책임:**
- NATS 연결 관리
- 탐지 결과 JSON 직렬화
- 메시지 발행

**메시지 포맷:**
```json
{
  "stream_id": "cam1",
  "timestamp": 1704758400000,
  "frame_number": 12345,
  "detections": [
    {
      "class": "car",
      "confidence": 0.95,
      "bbox": {
        "x": 100,
        "y": 200,
        "width": 150,
        "height": 80
      }
    }
  ]
}
```

**주요 메서드:**
```cpp
class NatsPublisher {
public:
    NatsPublisher(const std::string& natsUrl);
    ~NatsPublisher();
    
    bool connect();
    void disconnect();
    
    bool publish(const std::string& subject, const nlohmann::json& data);
    
private:
    natsConnection* conn_;
    std::string natsUrl_;
    std::mutex publishMutex_;
};
```

### 4. GrpcServer

**책임:**
- gRPC 서비스 제공
- StreamManager 제어

**서비스 정의 (stream.proto):**
```protobuf
service StreamService {
  rpc AddStream(AddStreamRequest) returns (StreamResponse);
  rpc RemoveStream(RemoveStreamRequest) returns (StreamResponse);
  rpc UpdateStream(UpdateStreamRequest) returns (StreamResponse);
  rpc GetStatus(GetStatusRequest) returns (StatusResponse);
  rpc GetAllStatus(Empty) returns (AllStatusResponse);
}
```

**구현:**
```cpp
class StreamServiceImpl final : public stream::StreamService::Service {
public:
    StreamServiceImpl(StreamManager* manager);
    
    grpc::Status AddStream(grpc::ServerContext* context,
                          const stream::AddStreamRequest* request,
                          stream::StreamResponse* response) override;
    
    grpc::Status RemoveStream(grpc::ServerContext* context,
                             const stream::RemoveStreamRequest* request,
                             stream::StreamResponse* response) override;
    
    // ... 기타 메서드
    
private:
    StreamManager* manager_;
};
```

## 스레드 모델

### Main Thread
- GLib MainLoop 실행
- 모든 GStreamer 파이프라인 관리

### gRPC Thread Pool
- gRPC 요청 처리
- StreamManager API 호출

### GStreamer Internal Threads
- 각 파이프라인마다 여러 스레드 자동 생성
  - Demuxer thread
  - Decoder thread
  - Hailo inference thread
  - Sink thread

```
Main Thread (GLib MainLoop)
  ├─ Stream 1 Pipeline
  │   ├─ Demux thread
  │   ├─ Decode thread
  │   ├─ Hailo thread
  │   └─ Sink thread
  ├─ Stream 2 Pipeline
  │   └─ ...
  └─ Stream N Pipeline

gRPC Thread Pool (4 threads)
  ├─ Request handler 1
  ├─ Request handler 2
  ├─ Request handler 3
  └─ Request handler 4
```

## 동기화 전략

### 1. StreamManager 동기화

```cpp
class StreamManager {
private:
    std::map<std::string, std::unique_ptr<StreamProcessor>> streams_;
    std::mutex streamsMutex_;  // streams_ 보호
    
public:
    bool addStream(...) {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        // streams_ 접근
    }
};
```

### 2. GStreamer Callbacks

- GStreamer callbacks는 Main Thread에서 실행됨
- GLib MainLoop context에서 실행 보장
- 별도 동기화 불필요

### 3. NATS Publishing

```cpp
class NatsPublisher {
private:
    std::mutex publishMutex_;  // NATS 연결 보호
    
public:
    bool publish(...) {
        std::lock_guard<std::mutex> lock(publishMutex_);
        natsConnection_PublishString(...);
    }
};
```

## 에러 처리 및 복구

### 1. RTSP 연결 실패

```cpp
void StreamProcessor::onErrorMessage(GstBus* bus, GstMessage* msg, ...) {
    GError* err;
    gst_message_parse_error(msg, &err, nullptr);
    
    g_printerr("Stream %s error: %s\n", streamId_.c_str(), err->message);
    
    // 상태 업데이트
    state_ = StreamState::ERROR;
    
    // 3초 후 재연결 스케줄
    g_timeout_add_seconds(3, reconnectCallback, this);
    
    g_error_free(err);
}
```

### 2. Hailo 추론 실패

- Hailo plugin이 내부적으로 처리
- 실패 시 해당 프레임 스킵
- 계속 실패 시 파이프라인 재시작

### 3. NATS 연결 끊김

```cpp
void NatsPublisher::checkConnection() {
    if (!natsConnection_IsClosed(conn_)) {
        return;
    }
    
    // 재연결 시도
    disconnect();
    connect();
}
```

## 메모리 관리

### 1. GStreamer 객체

```cpp
StreamProcessor::~StreamProcessor() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    
    if (bus_) {
        gst_object_unref(bus_);
        bus_ = nullptr;
    }
}
```

### 2. Smart Pointers 사용

```cpp
class StreamManager {
private:
    // unique_ptr로 자동 메모리 관리
    std::map<std::string, std::unique_ptr<StreamProcessor>> streams_;
};
```

## 성능 최적화

### 1. GStreamer 버퍼 설정

```cpp
// appsink 설정
g_object_set(sink,
    "max-buffers", 1,        // 최소 버퍼링
    "drop", TRUE,            // 오래된 프레임 버림
    "emit-signals", TRUE,
    nullptr);
```

### 2. RTSP 지연 최소화

```cpp
// rtspsrc 설정
g_object_set(rtspsrc,
    "latency", 0,            // 지연 최소화
    "timeout", 10000000,     // 타임아웃 10초
    "retry", 3,              // 재시도 3회
    nullptr);
```

### 3. Zero-Copy 최대화

- Hailo plugin은 내부적으로 zero-copy 지원
- GStreamer 버퍼를 직접 NPU로 전달
- CPU 복사 최소화

## 모니터링

### 메트릭 수집

```cpp
struct StreamStatus {
    std::string streamId;
    std::string rtspUrl;
    StreamState state;
    uint64_t frameCount;
    double currentFps;
    uint64_t uptimeSeconds;
    std::string lastError;
};
```

### 로깅

```cpp
// GStreamer 로그 레벨
gst_debug_set_default_threshold(GST_LEVEL_WARNING);

// 커스텀 로깅
#define LOG_INFO(fmt, ...) \
    g_print("[INFO] " fmt "\n", ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    g_printerr("[ERROR] " fmt "\n", ##__VA_ARGS__)
```

## 확장 고려사항

### 1. 스트림 수 확장

현재: 4개 → 향후: 10+개
- CPU 사용률 모니터링 필요
- Hailo NPU 처리량 확인
- 네트워크 대역폭 체크

### 2. 다중 Hailo 디바이스

```cpp
class HailoDevicePool {
    std::vector<VDevice*> devices_;
    
    VDevice* acquireDevice();
    void releaseDevice(VDevice* device);
};
```

### 3. 클러스터링

- 여러 Daemon 인스턴스
- 로드 밸런싱
- NATS JetStream for persistence

## 보안 고려사항

### 1. gRPC TLS

```cpp
grpc::SslServerCredentialsOptions ssl_opts;
// 인증서 설정
auto creds = grpc::SslServerCredentials(ssl_opts);
builder.AddListeningPort(server_address, creds);
```

### 2. RTSP 인증

```cpp
// rtspsrc에 인증 정보 설정
g_object_set(rtspsrc,
    "user-id", username,
    "user-pw", password,
    nullptr);
```

### 3. NATS 인증

```cpp
natsOptions* opts = nullptr;
natsOptions_Create(&opts);
natsOptions_SetUserInfo(opts, username, password);
natsConnection_Connect(&conn, opts);
```
