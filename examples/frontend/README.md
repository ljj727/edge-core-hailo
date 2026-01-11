# Stream Daemon Frontend Example

NATS WebSocket을 통해 실시간 스트림과 디텍션 결과를 표시하는 프론트엔드 예제입니다.

## Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  Stream Daemon  │────>│   NATS Server    │<────│    Frontend     │
│  (GStreamer +   │     │  (WebSocket)     │     │  (Browser)      │
│   Hailo NPU)    │     │                  │     │                 │
└─────────────────┘     └──────────────────┘     └─────────────────┘
        │                        │                       │
        │  Publish to            │  Subscribe to         │
        │  stream.<id>           │  stream.>             │
        └────────────────────────┴───────────────────────┘
```

## NATS Server Setup

### 1. WebSocket 활성화된 NATS 서버 설치

```bash
# NATS 서버 설치
wget https://github.com/nats-io/nats-server/releases/download/v2.10.7/nats-server-v2.10.7-linux-amd64.tar.gz
tar -xzf nats-server-v2.10.7-linux-amd64.tar.gz
sudo mv nats-server-v2.10.7-linux-amd64/nats-server /usr/local/bin/
```

### 2. NATS 설정 파일 생성

`/etc/nats/nats.conf`:

```conf
# 기본 포트 (stream_daemon용)
port: 4222

# WebSocket 포트 (프론트엔드용)
websocket {
    port: 8080
    no_tls: true
}

# 로깅
debug: false
trace: false
logtime: true
log_file: "/var/log/nats/nats.log"

# 메모리 제한 (4ch 30fps 기준 약 200MB 버퍼)
max_payload: 8MB
max_pending: 256MB
```

### 3. NATS 서버 실행

```bash
# 설정 파일로 실행
nats-server -c /etc/nats/nats.conf

# 또는 직접 옵션으로 실행
nats-server --port 4222 --websocket_port 8080
```

### 4. Systemd 서비스 (선택사항)

`/etc/systemd/system/nats.service`:

```ini
[Unit]
Description=NATS Server
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/nats-server -c /etc/nats/nats.conf
Restart=on-failure
User=nats
Group=nats

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable nats
sudo systemctl start nats
```

## Frontend Usage

### 1. 로컬에서 실행

```bash
# Python 간이 HTTP 서버
cd examples/frontend
python3 -m http.server 3000

# 브라우저에서 열기
xdg-open http://localhost:3000
```

### 2. 연결 설정

- **NATS URL**: `ws://localhost:8080` (WebSocket 포트)
- **Subject Pattern**: `stream.>` (모든 스트림 구독)

### 3. Subject 패턴 예시

| Pattern | Description |
|---------|-------------|
| `stream.>` | 모든 스트림 |
| `stream.cam1` | cam1 스트림만 |
| `stream.cam*` | cam으로 시작하는 스트림 |

## Message Format

Stream Daemon이 NATS로 발행하는 메시지 형식:

```json
{
  "stream_id": "cam1",
  "timestamp": 1704844800000,
  "frame_number": 12345,
  "fps": 29.97,
  "width": 1920,
  "height": 1080,
  "detections": [
    {
      "class": "person",
      "class_id": 0,
      "confidence": 0.95,
      "bbox": {
        "x": 100,
        "y": 200,
        "width": 150,
        "height": 300
      }
    }
  ],
  "image": "<base64_encoded_jpeg>"
}
```

## Bandwidth Estimation

| Channels | FPS | Resolution | Est. JPEG Size | Bandwidth |
|----------|-----|------------|----------------|-----------|
| 1 | 30 | 1080p | ~200KB | ~48 Mbps |
| 4 | 30 | 1080p | ~200KB | ~192 Mbps |
| 4 | 15 | 720p | ~100KB | ~48 Mbps |

Gigabit LAN 환경에서 4채널 1080p 30fps 충분히 처리 가능.

## Troubleshooting

### CORS 문제

NATS WebSocket은 기본적으로 CORS를 허용하지만, 문제가 있을 경우:

```conf
websocket {
    port: 8080
    no_tls: true
    # 모든 origin 허용
    same_origin: false
}
```

### 연결 실패

1. NATS 서버가 WebSocket 포트(8080)에서 실행 중인지 확인
2. 방화벽에서 포트가 열려 있는지 확인
3. 브라우저 콘솔에서 오류 메시지 확인

### 프레임 지연

1. JPEG 품질 낮추기 (stream_daemon 설정)
2. FPS 낮추기
3. 해상도 낮추기
4. 네트워크 대역폭 확인

## Browser Compatibility

| Browser | WebSocket | Tested |
|---------|-----------|--------|
| Chrome 90+ | Yes | Yes |
| Firefox 88+ | Yes | Yes |
| Safari 14+ | Yes | No |
| Edge 90+ | Yes | Yes |

## Development

### Dependencies

- [nats.ws](https://github.com/nats-io/nats.ws) - NATS WebSocket client (CDN)

### Customization

- `BBOX_COLORS`: Bounding box 색상 배열
- Canvas 크기: 자동으로 스트림 해상도에 맞춤
- 로그 개수: 최대 100개 유지

## Integration with Backend API

카메라 목록은 Backend API에서 가져오고, 실시간 스트림은 NATS로 직접 수신:

```javascript
// 1. Backend에서 카메라 목록 조회
const cameras = await fetch('/api/cameras').then(r => r.json());

// 2. 선택한 카메라의 스트림 구독
const subject = `stream.${cameras[0].stream_id}`;
const sub = nc.subscribe(subject);

// 3. 실시간 프레임 수신
for await (const msg of sub) {
    const data = JSON.parse(msg.data);
    // 화면에 표시
}
```
