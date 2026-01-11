# API Specification

## gRPC API

### Endpoint

```
Host: localhost:50051
Protocol: gRPC (HTTP/2)
```

### Service Definition

```protobuf
// proto/stream.proto
syntax = "proto3";

package stream;

// 스트림 관리 서비스
service StreamService {
  // 스트림 추가
  rpc AddStream(AddStreamRequest) returns (StreamResponse);
  
  // 스트림 제거
  rpc RemoveStream(RemoveStreamRequest) returns (StreamResponse);
  
  // 스트림 업데이트 (RTSP URL 변경)
  rpc UpdateStream(UpdateStreamRequest) returns (StreamResponse);
  
  // 특정 스트림 상태 조회
  rpc GetStatus(GetStatusRequest) returns (StatusResponse);
  
  // 모든 스트림 상태 조회
  rpc GetAllStatus(Empty) returns (AllStatusResponse);
}

// 메시지 정의

message StreamConfig {
  int32 width = 1;           // 비디오 width (기본: 1920)
  int32 height = 2;          // 비디오 height (기본: 1080)
  int32 fps = 3;             // 목표 FPS (기본: 30)
  float confidence_threshold = 4;  // 탐지 신뢰도 임계값 (기본: 0.5)
}

message AddStreamRequest {
  string stream_id = 1;      // 고유 스트림 ID
  string rtsp_url = 2;       // RTSP URL
  string hef_path = 3;       // Hailo HEF 모델 경로
  StreamConfig config = 4;   // 선택적 설정
}

message RemoveStreamRequest {
  string stream_id = 1;      // 제거할 스트림 ID
}

message UpdateStreamRequest {
  string stream_id = 1;      // 업데이트할 스트림 ID
  string rtsp_url = 2;       // 새 RTSP URL
  StreamConfig config = 3;   // 선택적 설정
}

message GetStatusRequest {
  string stream_id = 1;      // 조회할 스트림 ID
}

message Empty {}

// 응답 메시지

message StreamResponse {
  bool success = 1;          // 성공 여부
  string message = 2;        // 응답 메시지
  string stream_id = 3;      // 스트림 ID
}

message StreamStatus {
  string stream_id = 1;      // 스트림 ID
  string rtsp_url = 2;       // RTSP URL
  string state = 3;          // 상태: STARTING, RUNNING, STOPPED, ERROR
  uint64 frame_count = 4;    // 처리된 프레임 수
  double current_fps = 5;    // 현재 FPS
  uint64 uptime_seconds = 6; // 가동 시간 (초)
  string last_error = 7;     // 마지막 에러 메시지
  int64 last_detection_time = 8;  // 마지막 탐지 시간 (Unix timestamp)
}

message StatusResponse {
  bool success = 1;
  StreamStatus status = 2;
  string message = 3;
}

message AllStatusResponse {
  repeated StreamStatus streams = 1;
}
```

## API 사용 예시

### 1. AddStream - 스트림 추가

**Request:**
```bash
grpcurl -plaintext -d '{
  "stream_id": "camera_01",
  "rtsp_url": "rtsp://192.168.1.100:554/stream",
  "hef_path": "/models/yolov8n.hef",
  "config": {
    "width": 1920,
    "height": 1080,
    "fps": 30,
    "confidence_threshold": 0.6
  }
}' localhost:50051 stream.StreamService/AddStream
```

**Response:**
```json
{
  "success": true,
  "message": "Stream camera_01 added successfully",
  "stream_id": "camera_01"
}
```

**Error Response:**
```json
{
  "success": false,
  "message": "Stream camera_01 already exists",
  "stream_id": "camera_01"
}
```

### 2. RemoveStream - 스트림 제거

**Request:**
```bash
grpcurl -plaintext -d '{
  "stream_id": "camera_01"
}' localhost:50051 stream.StreamService/RemoveStream
```

**Response:**
```json
{
  "success": true,
  "message": "Stream camera_01 removed successfully",
  "stream_id": "camera_01"
}
```

### 3. UpdateStream - 스트림 업데이트

**Request:**
```bash
grpcurl -plaintext -d '{
  "stream_id": "camera_01",
  "rtsp_url": "rtsp://192.168.1.101:554/stream",
  "config": {
    "confidence_threshold": 0.7
  }
}' localhost:50051 stream.StreamService/UpdateStream
```

**Response:**
```json
{
  "success": true,
  "message": "Stream camera_01 updated successfully",
  "stream_id": "camera_01"
}
```

### 4. GetStatus - 특정 스트림 상태 조회

**Request:**
```bash
grpcurl -plaintext -d '{
  "stream_id": "camera_01"
}' localhost:50051 stream.StreamService/GetStatus
```

**Response:**
```json
{
  "success": true,
  "status": {
    "stream_id": "camera_01",
    "rtsp_url": "rtsp://192.168.1.100:554/stream",
    "state": "RUNNING",
    "frame_count": 12345,
    "current_fps": 29.8,
    "uptime_seconds": 3600,
    "last_error": "",
    "last_detection_time": 1704758400
  },
  "message": "Status retrieved successfully"
}
```

### 5. GetAllStatus - 모든 스트림 상태 조회

**Request:**
```bash
grpcurl -plaintext localhost:50051 stream.StreamService/GetAllStatus
```

**Response:**
```json
{
  "streams": [
    {
      "stream_id": "camera_01",
      "rtsp_url": "rtsp://192.168.1.100:554/stream",
      "state": "RUNNING",
      "frame_count": 12345,
      "current_fps": 29.8,
      "uptime_seconds": 3600,
      "last_error": ""
    },
    {
      "stream_id": "camera_02",
      "rtsp_url": "rtsp://192.168.1.101:554/stream",
      "state": "ERROR",
      "frame_count": 5000,
      "current_fps": 0.0,
      "uptime_seconds": 1800,
      "last_error": "RTSP connection timeout"
    }
  ]
}
```

## NATS 메시지 포맷

스트림에서 탐지 결과는 NATS로 발행됩니다.

### Subject 패턴

```
stream.{stream_id}.detections
```

예시:
- `stream.camera_01.detections`
- `stream.camera_02.detections`

### 메시지 페이로드 (JSON)

```json
{
  "stream_id": "camera_01",
  "timestamp": 1704758400123,
  "frame_number": 12345,
  "fps": 29.8,
  "detections": [
    {
      "class": "car",
      "class_id": 2,
      "confidence": 0.95,
      "bbox": {
        "x": 100,
        "y": 200,
        "width": 150,
        "height": 80
      }
    },
    {
      "class": "person",
      "class_id": 0,
      "confidence": 0.87,
      "bbox": {
        "x": 300,
        "y": 150,
        "width": 60,
        "height": 180
      }
    }
  ]
}
```

### 구독 예시 (Python)

```python
import asyncio
import nats
import json

async def main():
    nc = await nats.connect("nats://localhost:4222")
    
    async def message_handler(msg):
        data = json.loads(msg.data.decode())
        print(f"Stream: {data['stream_id']}")
        print(f"Detections: {len(data['detections'])}")
        for det in data['detections']:
            print(f"  - {det['class']}: {det['confidence']:.2f}")
    
    # 특정 스트림 구독
    await nc.subscribe("stream.camera_01.detections", cb=message_handler)
    
    # 모든 스트림 구독 (wildcard)
    await nc.subscribe("stream.*.detections", cb=message_handler)
    
    # 계속 실행
    while True:
        await asyncio.sleep(1)

if __name__ == '__main__':
    asyncio.run(main())
```

## 상태 코드 (StreamState)

```cpp
enum class StreamState {
    STARTING,    // 초기화 중
    RUNNING,     // 정상 실행 중
    STOPPED,     // 정지됨
    ERROR,       // 에러 발생
    RECONNECTING // 재연결 시도 중
};
```

## 에러 코드

| 코드 | 메시지 | 설명 |
|------|--------|------|
| `ALREADY_EXISTS` | Stream already exists | 동일 ID의 스트림이 이미 존재 |
| `NOT_FOUND` | Stream not found | 해당 ID의 스트림이 없음 |
| `INVALID_URL` | Invalid RTSP URL | RTSP URL 형식 오류 |
| `CONNECTION_FAILED` | Failed to connect to RTSP | RTSP 연결 실패 |
| `HEF_NOT_FOUND` | HEF file not found | HEF 파일 경로 오류 |
| `PIPELINE_ERROR` | GStreamer pipeline error | 파이프라인 생성 실패 |
| `INTERNAL_ERROR` | Internal server error | 내부 서버 오류 |

## 클라이언트 라이브러리 예시

### Python (grpcio)

```python
import grpc
import stream_pb2
import stream_pb2_grpc

# 연결
channel = grpc.insecure_channel('localhost:50051')
stub = stream_pb2_grpc.StreamServiceStub(channel)

# 스트림 추가
request = stream_pb2.AddStreamRequest(
    stream_id="camera_01",
    rtsp_url="rtsp://192.168.1.100:554/stream",
    hef_path="/models/yolov8n.hef",
    config=stream_pb2.StreamConfig(
        confidence_threshold=0.6
    )
)

response = stub.AddStream(request)
print(f"Success: {response.success}")
print(f"Message: {response.message}")
```

### Java (Spring Boot)

```java
@Service
public class StreamService {
    private final StreamServiceGrpc.StreamServiceBlockingStub stub;
    
    public StreamService() {
        ManagedChannel channel = ManagedChannelBuilder
            .forAddress("localhost", 50051)
            .usePlaintext()
            .build();
        
        this.stub = StreamServiceGrpc.newBlockingStub(channel);
    }
    
    public boolean addStream(String streamId, String rtspUrl) {
        AddStreamRequest request = AddStreamRequest.newBuilder()
            .setStreamId(streamId)
            .setRtspUrl(rtspUrl)
            .setHefPath("/models/yolov8n.hef")
            .build();
        
        StreamResponse response = stub.addStream(request);
        return response.getSuccess();
    }
}
```

### curl (REST proxy 필요)

gRPC-Gateway를 사용하면 REST API로도 접근 가능:

```bash
# 스트림 추가
curl -X POST http://localhost:8080/v1/streams \
  -H "Content-Type: application/json" \
  -d '{
    "stream_id": "camera_01",
    "rtsp_url": "rtsp://192.168.1.100:554/stream",
    "hef_path": "/models/yolov8n.hef"
  }'
```

## 인증 (향후 구현)

```protobuf
// API 키 기반 인증
message AuthMetadata {
  string api_key = 1;
}

// 각 요청에 메타데이터 추가
```

## Rate Limiting

- 기본: 100 requests/minute per client
- Burst: 최대 10 requests/second

## Health Check

```bash
grpcurl -plaintext localhost:50051 grpc.health.v1.Health/Check
```

## 버전 관리

API 버전: v1 (current)
- 하위 호환성 유지
- Breaking changes 시 v2로 업그레이드
