# Stream Daemon 기술 문서

## 1. 개요

Stream Daemon은 RTSP 카메라 영상을 실시간으로 처리하여 Hailo NPU에서 객체 탐지를 수행하고, 결과를 NATS 메시지 큐를 통해 외부 시스템에 전달하는 C++ 기반 데몬입니다.

**주요 목적:**
- 세차장 카메라에서 차량 및 차량 부위(바퀴, 사이드미러) 검출
- 실시간 검출 결과를 웹 프론트엔드에 전달
- 여러 카메라를 하나의 프로세스에서 효율적으로 관리

---

## 2. 시스템 구성도

```
┌────────────────────────────────────────────────────────────────────────────┐
│                            Stream Daemon                                    │
│                                                                            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                         gRPC Server (:50052)                         │  │
│  │   - RegisterModel: HEF 모델 등록                                      │  │
│  │   - AddStream: RTSP 스트림 추가                                       │  │
│  │   - RemoveStream: 스트림 제거                                         │  │
│  │   - GetAllStatus: 전체 상태 조회                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                    │                                        │
│                                    ▼                                        │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                         StreamManager                                │  │
│  │                                                                      │  │
│  │   models_: 등록된 HEF 모델 관리 (model_id → HailoInference)          │  │
│  │   streams_: 활성 스트림 관리 (stream_id → StreamProcessor)           │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                    │                                        │
│           ┌────────────────────────┼────────────────────────┐              │
│           ▼                        ▼                        ▼              │
│  ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐        │
│  │ StreamProcessor │    │ StreamProcessor │    │ StreamProcessor │        │
│  │     (cam1)      │    │     (cam2)      │    │     (camN)      │        │
│  │                 │    │                 │    │                 │        │
│  │  GStreamer      │    │  GStreamer      │    │  GStreamer      │        │
│  │  Pipeline       │    │  Pipeline       │    │  Pipeline       │        │
│  └────────┬────────┘    └────────┬────────┘    └────────┬────────┘        │
│           │                      │                      │                  │
│           └──────────────────────┼──────────────────────┘                  │
│                                  ▼                                          │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                     HailoInference (Shared)                          │  │
│  │                                                                      │  │
│  │   VDevice: 단일 Hailo NPU 인스턴스                                    │  │
│  │   InferModel: 로드된 HEF 모델                                         │  │
│  │   ConfiguredInferModel: 입출력 바인딩 설정                            │  │
│  │                                                                      │  │
│  │   ※ 모든 스트림이 하나의 VDevice를 공유하여 NPU 자원 효율화           │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                  │                                          │
│                                  ▼                                          │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                        NatsPublisher                                 │  │
│  │                                                                      │  │
│  │   subject: "aiwash.detection.{stream_id}"                            │  │
│  │   format: JSON (timestamp, detections, keypoints)                    │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                  │                                          │
└──────────────────────────────────┼──────────────────────────────────────────┘
                                   │
                                   ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                           NATS Server                                        │
│                                                                              │
│   TCP (:4223)  ─────────────────────────────────────  내부 서비스 연결       │
│   WebSocket (:4224)  ───────────────────────────────  웹 프론트엔드 연결     │
└──────────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                         React Frontend                                       │
│                                                                              │
│   NATS WebSocket 구독 → 실시간 검출 결과 표시                                │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 컴포넌트 상세

### 3.1 gRPC Server

외부 시스템(FastAPI, CLI 등)에서 Stream Daemon을 제어하기 위한 API를 제공합니다.

| API | 설명 | 파라미터 |
|-----|------|----------|
| `RegisterModel` | HEF 모델 등록 | model_id, hef_path, config_json_path |
| `UnregisterModel` | 모델 등록 해제 | model_id |
| `AddStream` | RTSP 스트림 추가 | stream_id, rtsp_url, model_id |
| `RemoveStream` | 스트림 제거 | stream_id |
| `GetStreamStatus` | 개별 스트림 상태 | stream_id |
| `GetAllStatus` | 전체 상태 조회 | - |
| `HealthCheck` | 서버 상태 확인 | - |

**사용 예시:**
```bash
# 모델 등록
grpcurl -plaintext -d '{
  "model_id": "vehicle-pose",
  "hef_path": "/app/models/best12_ms.hef",
  "config_json_path": "/app/models/best12_config.json"
}' localhost:50052 stream_daemon.StreamDaemon/RegisterModel

# 스트림 추가
grpcurl -plaintext -d '{
  "stream_id": "cam1",
  "rtsp_url": "rtsp://192.168.0.100:554/stream",
  "model_id": "vehicle-pose"
}' localhost:50052 stream_daemon.StreamDaemon/AddStream
```

---

### 3.2 StreamManager

모든 스트림과 모델의 생명주기를 관리하는 중앙 컨트롤러입니다.

**주요 역할:**
- 모델 등록/해제 관리
- 스트림 추가/제거/상태 관리
- GLib MainLoop 실행 (GStreamer 이벤트 루프)
- 스트림 상태 모니터링

**내부 데이터 구조:**
```
models_: map<model_id, shared_ptr<HailoInference>>
  └─ "vehicle-pose" → HailoInference 인스턴스

streams_: map<stream_id, unique_ptr<StreamProcessor>>
  ├─ "cam1" → StreamProcessor 인스턴스
  ├─ "cam2" → StreamProcessor 인스턴스
  └─ "cam4" → StreamProcessor 인스턴스
```

---

### 3.3 StreamProcessor

개별 RTSP 스트림을 처리하는 GStreamer 파이프라인을 관리합니다.

**GStreamer 파이프라인 구조:**
```
rtspsrc (RTSP 수신)
   │
   ▼
rtph264depay (RTP 패킷 → H264)
   │
   ▼
h264parse (H264 파싱)
   │
   ▼
avdec_h264 (H264 디코딩 → Raw Frame)
   │
   ▼
videoconvert (포맷 변환)
   │
   ▼
videoscale (960x960 리사이즈)
   │
   ▼
appsink (프레임 추출)
   │
   ▼
[C++ 코드에서 HailoInference 호출]
```

**주요 기능:**
- RTSP 연결 관리
- 프레임 디코딩 및 전처리
- 연결 끊김 시 자동 재연결 (3초 간격)
- 프레임 드롭 처리 (버퍼 초과 시)

**상태 관리:**
```
IDLE → CONNECTING → RUNNING → ERROR → RECONNECTING → RUNNING
                      │                     ▲
                      └─────────────────────┘
```

---

### 3.4 HailoInference

Hailo NPU를 사용하여 실제 추론을 수행합니다.

**핵심 개념 - Shared VDevice:**
```
┌─────────────────────────────────────────────────────────┐
│                    Hailo NPU (Physical)                 │
│                                                         │
│  ┌─────────────────────────────────────────────────┐   │
│  │              VDevice (Shared Instance)          │   │
│  │                                                 │   │
│  │   ┌─────────────┐  ┌─────────────┐             │   │
│  │   │ InferModel  │  │ InferModel  │   ...       │   │
│  │   │ (model A)   │  │ (model B)   │             │   │
│  │   └─────────────┘  └─────────────┘             │   │
│  │                                                 │   │
│  │   cam1 ──┐                                      │   │
│  │   cam2 ──┼──▶ 추론 요청 큐 ──▶ NPU 실행        │   │
│  │   cam4 ──┘                                      │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

**추론 과정:**
1. StreamProcessor에서 프레임 수신 (RGB, 960x960)
2. HailoInference::RunInference() 호출
3. 입력 버퍼에 프레임 데이터 복사
4. NPU에서 추론 실행
5. 출력 버퍼에서 결과 추출 (9개 텐서)
6. 후처리: DFL 디코딩, NMS, Keypoint 추출
7. Detection 결과 반환

**출력 텐서 구조 (YOLOv8-pose 멀티스케일):**
| 스케일 | 크기 | DFL (bbox) | Class | Keypoint |
|--------|------|------------|-------|----------|
| P3 | 120x120 | 64ch | 13ch | 12ch |
| P4 | 60x60 | 64ch | 13ch | 12ch |
| P5 | 30x30 | 64ch | 13ch | 12ch |

---

### 3.5 NatsPublisher

검출 결과를 NATS 메시지 큐에 발행합니다.

**발행 Subject:**
```
aiwash.detection.{stream_id}

예: aiwash.detection.cam1
    aiwash.detection.cam2
```

**메시지 포맷 (JSON):**
```json
{
  "stream_id": "cam1",
  "timestamp": 1705312345678,
  "frame_number": 12345,
  "inference_time_ms": 15.2,
  "detections": [
    {
      "class_id": 3,
      "class_name": "Full-size",
      "confidence": 0.92,
      "bbox": {
        "x1": 120.5,
        "y1": 200.3,
        "x2": 450.8,
        "y2": 520.1
      },
      "keypoints": [
        {"x": 150.0, "y": 210.0, "confidence": 0.95},
        {"x": 180.0, "y": 480.0, "confidence": 0.88},
        {"x": 420.0, "y": 485.0, "confidence": 0.91},
        {"x": 445.0, "y": 215.0, "confidence": 0.87}
      ]
    }
  ]
}
```

**Keypoint 의미 (차량):**
- kp[0]: 앞범퍼
- kp[1]: 앞바퀴 하단
- kp[2]: 뒷바퀴 하단
- kp[3]: 뒷범퍼

---

## 4. 기술 스택

| 구분 | 기술 | 버전 | 용도 |
|------|------|------|------|
| 언어 | C++ | 17 | 메인 개발 언어 |
| 영상처리 | GStreamer | 1.20+ | RTSP 수신, 디코딩 |
| NPU | HailoRT | 4.23.0 | Hailo8 NPU 제어 |
| API | gRPC | 1.51+ | 외부 제어 인터페이스 |
| 직렬화 | Protobuf | 3.21+ | gRPC 메시지 정의 |
| 메시징 | NATS C | 3.6+ | 검출 결과 발행 |
| JSON | nlohmann/json | 3.11+ | 설정 파일, 메시지 포맷 |
| 빌드 | CMake | 3.20+ | 빌드 시스템 |

---

## 5. 파일 구조

```
stream-daemon-docs/
├── CMakeLists.txt          # 빌드 설정
├── Dockerfile              # Docker 이미지 빌드
├── docker-compose.yml      # Docker Compose 설정
├── nats-docker.conf        # NATS 서버 설정
│
├── src/                    # 소스 코드
│   ├── main.cpp            # 진입점
│   ├── stream_manager.cpp  # 스트림 관리자
│   ├── stream_processor.cpp # GStreamer 파이프라인
│   ├── hailo_inference.cpp # NPU 추론
│   ├── nats_publisher.cpp  # NATS 발행
│   └── grpc_server.cpp     # gRPC 서버
│
├── include/                # 헤더 파일
│   ├── stream_manager.h
│   ├── stream_processor.h
│   ├── hailo_inference.h
│   ├── nats_publisher.h
│   └── grpc_server.h
│
├── proto/                  # Protobuf 정의
│   └── stream_daemon.proto
│
├── config/                 # 설정 파일 (마운트용)
│   └── config.yaml
│
└── models/                 # HEF 모델 (마운트용)
    ├── best12_ms.hef
    └── best12_config.json
```

---

## 6. 배포

### 요구사항 (호스트)
- Docker 20.10+
- HailoRT 커널 드라이버 4.23.0
- /dev/hailo0 장치

### 실행
```bash
# 시작
docker compose up -d

# 로그 확인
docker compose logs -f stream-daemon

# 중지
docker compose down
```

### 포트
| 포트 | 서비스 | 프로토콜 |
|------|--------|----------|
| 50052 | stream-daemon | gRPC |
| 4223 | nats | TCP |
| 4224 | nats | WebSocket |

---

## 7. 성능

| 항목 | 값 |
|------|-----|
| 최대 스트림 수 | 4개 (Hailo8 기준) |
| 추론 속도 | ~15ms/frame |
| 전체 지연 | ~50-100ms (RTSP → NATS) |
| 메모리 사용 | ~200-300MB |
| NPU 활용률 | ~70-80% (4스트림) |

---

## 8. 클래스 목록 (13개)

| ID | 클래스명 | 설명 |
|----|----------|------|
| 0 | General | 일반 차량 |
| 1 | Compact | 경차 |
| 2 | Mid-size | 중형차 |
| 3 | Full-size | 대형차 |
| 4 | Small-truck | 소형 트럭 |
| 5 | Large-truck | 대형 트럭 |
| 6 | SUV | SUV |
| 7 | Sports-car | 스포츠카 |
| 8 | Van | 밴 |
| 9 | Bus | 버스 |
| 10 | Wheel-L | 왼쪽 바퀴 |
| 11 | Wheel-R | 오른쪽 바퀴 |
| 12 | Mirror | 사이드미러 |
