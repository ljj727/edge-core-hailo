# Stream Daemon 배포 가이드

## 시스템 요구사항

### 하드웨어
| 항목 | 최소 사양 | 권장 사양 |
|------|----------|----------|
| CPU | 4 Core | 4 Core 이상 |
| RAM | 8GB | 16GB |
| NPU | Hailo8 (PCIe) | Hailo8 (PCIe) |

### 소프트웨어 (호스트 PC에 필요)
| 항목 | 버전 | 설치 방법 |
|------|------|----------|
| Docker | 20.10+ | `curl -fsSL https://get.docker.com \| sh` |
| Docker Compose | v2+ | Docker에 포함됨 |
| HailoRT 드라이버 | 4.23.0 | `sudo dpkg -i hailort_4.23.0_amd64.deb` |

> **중요**: HailoRT 런타임 라이브러리는 Docker 이미지에 포함되어 있지만, **커널 드라이버**는 호스트에 설치되어야 합니다.

### Hailo 드라이버 확인
```bash
# Hailo 장치 확인
ls -la /dev/hailo*

# 드라이버 버전 확인
hailortcli fw-control identify
```

---

## 배포 파일 구조

```
stream-daemon/
├── docker-compose.yml    # Docker Compose 설정
├── nats-docker.conf      # NATS 서버 설정
├── .env                  # 환경 변수 (포트 설정)
├── config/
│   └── config.yaml       # Stream Daemon 설정
└── models/
    └── *.hef             # Hailo 모델 파일
```

---

## 설정 파일

### .env
```env
GRPC_PORT=50052
NATS_PORT=4223
NATS_WS_PORT=4224
```

### config/config.yaml
```yaml
nats:
  url: "nats://nats:4222"
  detection_subject: "aiwash.detection"

models: []
streams: []
```

### nats-docker.conf
```conf
port: 4222
http_port: 8222
max_payload: 8MB

websocket {
    port: 8080
    no_tls: true
}
```

---

## 실행 방법

### 1. 이미지 다운로드
```bash
docker compose pull
```

### 2. 서비스 시작
```bash
docker compose up -d
```

### 3. 상태 확인
```bash
docker compose ps
docker compose logs -f stream-daemon
```

### 4. 서비스 중지
```bash
docker compose down
```

---

## 모델 등록 (gRPC API)

```bash
# 모델 등록
grpcurl -plaintext -d '{
  "model_id": "yolov8n",
  "hef_path": "/app/models/yolov8n.hef"
}' localhost:50052 stream_daemon.StreamDaemon/RegisterModel

# 스트림 추가
grpcurl -plaintext -d '{
  "stream_id": "cam1",
  "rtsp_url": "rtsp://192.168.0.100:554/stream",
  "model_id": "yolov8n"
}' localhost:50052 stream_daemon.StreamDaemon/AddStream
```

---

## 포트 정보

| 서비스 | 컨테이너 포트 | 기본 호스트 포트 | 용도 |
|--------|-------------|-----------------|------|
| stream-daemon | 50052 | 50052 | gRPC API |
| nats | 4222 | 4223 | NATS TCP |
| nats | 8080 | 4224 | NATS WebSocket |

---

## 트러블슈팅

### Hailo 장치를 찾을 수 없음
```bash
# 장치 확인
ls /dev/hailo*

# 드라이버 재로드
sudo modprobe hailo_pci
```

### 권한 오류
```bash
# 현재 사용자를 docker 그룹에 추가
sudo usermod -aG docker $USER
# 재로그인 필요
```

### 컨테이너 로그 확인
```bash
docker compose logs stream-daemon
docker compose logs nats
```
