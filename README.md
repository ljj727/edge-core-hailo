# Stream Processing Daemon

GStreamer + Hailo 기반 멀티 RTSP 스트림 처리 데몬

## 프로젝트 개요

실시간 RTSP 스트림을 처리하여 Hailo NPU로 객체 탐지를 수행하고, 결과를 NATS 메시지 큐를 통해 전달하는 고성능 C++ 데몬입니다.

## 주요 기능

- **멀티 스트림 처리**: 최대 4개의 RTSP 스트림 동시 처리
- **Hailo NPU 가속**: Hailo-8 NPU를 통한 고속 추론
- **동적 스트림 관리**: 런타임 중 스트림 추가/제거/변경
- **자동 재연결**: 네트워크 장애 시 자동 복구
- **gRPC API**: 외부에서 스트림 제어 가능
- **실시간 이벤트**: NATS를 통한 탐지 결과 발행

## 시스템 아키텍처

```
┌─────────────────────────────────────────────────┐
│         C++ GStreamer Daemon                    │
│  ┌──────────────────────────────────────────┐   │
│  │  StreamManager                           │   │
│  │  ├─ Stream 1 (GStreamer Pipeline)        │   │
│  │  ├─ Stream 2 (GStreamer Pipeline)        │   │
│  │  ├─ Stream 3 (GStreamer Pipeline)        │   │
│  │  └─ Stream 4 (GStreamer Pipeline)        │   │
│  └──────────────────────────────────────────┘   │
│           ↓ (NATS publish)                      │
│  ┌──────────────────────────────────────────┐   │
│  │  gRPC Server (port 50051)                │   │
│  │  - AddStream                             │   │
│  │  - RemoveStream                          │   │
│  │  - UpdateStream                          │   │
│  │  - GetStatus                             │   │
│  └──────────────────────────────────────────┘   │
└─────────────────────────────────────────────────┘
         ↓ NATS                    ↓ gRPC
┌─────────────────────┐   ┌─────────────────────┐
│  NATS Server        │   │  API Server         │
│  localhost:4222     │   │  (FastAPI/Spring)   │
└─────────────────────┘   └─────────────────────┘
                                   ↓ WebSocket
                          ┌─────────────────────┐
                          │  Web Frontend       │
                          └─────────────────────┘
```

## 기술 스택

- **언어**: C++17
- **미디어 프레임워크**: GStreamer 1.20+
- **AI 가속**: Hailo SDK, HailoRT
- **메시징**: NATS C Client
- **RPC**: gRPC
- **빌드**: CMake 3.20+
- **JSON**: nlohmann/json

## 디렉토리 구조

```
asdf/
├── README.md                 # 이 파일
├── ARCHITECTURE.md           # 상세 아키텍처 설계
├── BUILD.md                  # 빌드 가이드
├── API.md                    # gRPC API 명세
├── CMakeLists.txt           # CMake 빌드 설정
├── proto/                   # Protocol Buffers 정의
│   └── stream.proto
├── src/                     # 소스 코드
│   ├── main.cpp
│   ├── stream_manager.h
│   ├── stream_manager.cpp
│   ├── stream_processor.h
│   ├── stream_processor.cpp
│   ├── grpc_server.h
│   ├── grpc_server.cpp
│   ├── nats_publisher.h
│   └── nats_publisher.cpp
├── include/                 # 헤더 파일
├── scripts/                 # 유틸리티 스크립트
│   ├── install_deps.sh
│   └── run_daemon.sh
└── docker/                  # Docker 설정
    ├── Dockerfile
    └── docker-compose.yml
```

## 빠른 시작

### 의존성 설치

```bash
cd /Users/ijongjin/snuailab/project/asdf
./scripts/install_deps.sh
```

### 빌드

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 실행

```bash
# NATS 서버 실행
docker run -d -p 4222:4222 nats

# Daemon 실행
./build/stream_daemon
```

### 스트림 추가 (테스트)

```bash
# grpcurl 사용
grpcurl -plaintext -d '{
  "stream_id": "cam1",
  "rtsp_url": "rtsp://localhost:8554/webcam",
  "hef_path": "/path/to/model.hef"
}' localhost:50051 stream.StreamService/AddStream
```

## 개발 로드맵

### Phase 1: 기본 구조 (1주)
- [x] 프로젝트 구조 설계
- [ ] CMake 설정
- [ ] GStreamer 파이프라인 기본 구현
- [ ] 단일 스트림 테스트

### Phase 2: 멀티스트림 (1주)
- [ ] StreamManager 구현
- [ ] 멀티스레드 처리
- [ ] 에러 핸들링 및 자동 재연결
- [ ] 4개 스트림 동시 테스트

### Phase 3: gRPC 통합 (3일)
- [ ] Protocol Buffers 정의
- [ ] gRPC 서버 구현
- [ ] API 테스트

### Phase 4: NATS 통합 (3일)
- [ ] NATS 클라이언트 통합
- [ ] 탐지 결과 발행
- [ ] 메시지 포맷 정의

### Phase 5: 최적화 및 배포 (1주)
- [ ] 성능 튜닝
- [ ] 메모리 프로파일링
- [ ] Docker 이미지 작성
- [ ] 문서 완성

## 성능 목표

- **처리량**: 4개 스트림 @ 30 FPS
- **지연시간**: < 100ms (RTSP → 탐지 결과)
- **메모리**: < 150MB (4개 스트림)
- **CPU**: < 50% (Hailo NPU 사용 시)
- **가동시간**: 99.9%+ (자동 복구)

## 라이선스

MIT License

## 기여

이슈 및 풀 리퀘스트 환영합니다.

## 문의

- 개발자: Jin
- 프로젝트: autocare-dx20 MLOps Platform
