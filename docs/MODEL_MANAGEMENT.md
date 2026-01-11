# Model Management Guide

Stream Daemon의 모델 관리 시스템 문서입니다.

## 개요

- 모델은 gRPC API를 통해 ZIP 패키지로 업로드
- 기본 모델 개념 없음 - 모든 스트림은 반드시 등록된 모델 ID 지정 필요
- 파일 기반 저장 (DB 불필요)

---

## 모델 패키지 구조

### ZIP 파일 구성

```
model_package.zip
├── model.hef              # Hailo 모델 파일 (필수)
└── model_config.json      # 모델 메타데이터 (필수)
```

### model_config.json 스키마

```json
{
  "model_id": "yolov8n-coco",
  "name": "YOLOv8 Nano COCO",
  "function_name": "yolov8",
  "post_process_so": "/usr/lib/hailo-post-processes/libyolo_hailortpp_post.so",
  "labels": ["person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck"],
  "description": "YOLOv8n trained on COCO dataset (80 classes)"
}
```

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `model_id` | string | O | 고유 식별자 (영문, 숫자, 하이픈, 언더스코어) |
| `name` | string | X | 표시용 이름 (기본값: model_id) |
| `function_name` | string | X | 후처리 함수명 (기본값: "yolov8") |
| `post_process_so` | string | X | 후처리 라이브러리 경로 (기본값: libyolo_hailortpp_post.so) |
| `labels` | string[] | X | 클래스 레이블 목록 |
| `description` | string | X | 모델 설명 |

---

## 서버 저장 구조

```
/var/lib/stream-daemon/models/          # config.yaml의 models.models_dir
├── yolov8n-coco/
│   ├── model.hef
│   └── model_config.json
├── yolov8s-custom/
│   ├── model.hef
│   └── model_config.json
└── my-detection-model/
    ├── model.hef
    └── model_config.json
```

---

## gRPC API

### 서비스 정의

```protobuf
service StreamService {
  // 모델 업로드 (Client Streaming)
  rpc UploadModel(stream UploadModelChunk) returns (ModelResponse);

  // 모델 삭제
  rpc DeleteModel(DeleteModelRequest) returns (ModelResponse);

  // 모델 목록 조회
  rpc ListModels(Empty) returns (ListModelsResponse);

  // 모델 상세 정보 조회
  rpc GetModelInfo(GetModelInfoRequest) returns (ModelInfoResponse);
}
```

### 메시지 정의

```protobuf
// 업로드 청크 (스트리밍)
message UploadModelChunk {
  oneof data {
    UploadModelMetadata metadata = 1;   // 첫 번째 청크: 메타데이터
    bytes chunk = 2;                    // 이후 청크: ZIP 바이너리
  }
}

message UploadModelMetadata {
  string filename = 1;                  // 원본 파일명
  int64 total_size = 2;                 // 전체 크기 (bytes)
  bool overwrite = 3;                   // 덮어쓰기 여부
}

// 응답
message ModelResponse {
  bool success = 1;
  string message = 2;
  string model_id = 3;
}

// 모델 정보
message ModelInfo {
  string model_id = 1;
  string name = 2;
  string hef_path = 3;
  string post_process_so = 4;
  string function_name = 5;
  repeated string labels = 6;
  string description = 7;
  int64 registered_at = 8;
  int32 usage_count = 9;                // 사용 중인 스트림 수
}
```

---

## 업로드 흐름

```
Client                                  Server
  |                                        |
  |--- UploadModelChunk{metadata} -------->|  (filename, total_size, overwrite)
  |--- UploadModelChunk{chunk_1} --------->|
  |--- UploadModelChunk{chunk_2} --------->|  (bytes 누적)
  |              ...                       |
  |--- UploadModelChunk{chunk_n} --------->|
  |                                        |  ZIP 추출 → 검증 → 저장
  |<-- ModelResponse{success, model_id} ---|
```

### 청크 크기 권장

- 청크 크기: 64KB ~ 1MB
- gRPC 최대 메시지 크기: 100MB (서버 설정)

---

## 에러 응답

| 상황 | success | message |
|------|---------|---------|
| 성공 | `true` | `"Model 'xxx' uploaded successfully"` |
| 메타데이터 누락 | `false` | `"First chunk must contain metadata"` |
| 데이터 없음 | `false` | `"No data received"` |
| ZIP 오류 | `false` | `"Failed to open ZIP: ..."` |
| model.hef 없음 | `false` | `"ZIP must contain 'model.hef'"` |
| config.json 없음 | `false` | `"ZIP must contain 'model_config.json'"` |
| model_id 누락 | `false` | `"model_config.json must contain 'model_id' string"` |
| 중복 등록 | `false` | `"Model 'xxx' already exists. Use overwrite=true"` |
| 사용 중 덮어쓰기 | `false` | `"Model 'xxx' is in use by N stream(s)"` |

---

## 스트림에서 모델 사용

```protobuf
message AddStreamRequest {
  string stream_id = 1;
  string rtsp_url = 2;
  string model_id = 3;      // 등록된 모델 ID (필수!)
  StreamConfig config = 4;
}
```

**주의**: `model_id`는 필수 필드입니다. 등록되지 않은 모델 ID 사용 시 에러 반환.

---

## HEF 모델 다운로드

### Hailo Model Zoo

Hailo에서 사전 컴파일된 HEF 파일을 제공합니다.

**GitHub Repository**: https://github.com/hailo-ai/hailo_model_zoo

### 다운로드 방법

#### 1. AWS S3 직접 다운로드

```bash
# YOLOv8 모델 다운로드 (예시)
wget https://hailo-model-zoo.s3.eu-west-2.amazonaws.com/ModelZoo/Compiled/v2.17.0/hailo8/yolov8n.hef
wget https://hailo-model-zoo.s3.eu-west-2.amazonaws.com/ModelZoo/Compiled/v2.17.0/hailo8/yolov8s.hef
wget https://hailo-model-zoo.s3.eu-west-2.amazonaws.com/ModelZoo/Compiled/v2.17.0/hailo8/yolov8m.hef
```

#### 2. Hailo Model Zoo CLI 사용

```bash
# hailo_model_zoo 설치
pip install hailo_model_zoo

# 모델 다운로드 및 컴파일
hailomz compile yolov8n
hailomz compile yolov8s --hw-arch hailo8
```

#### 3. TAPPAS 패키지 (사전 설치된 모델)

```bash
# TAPPAS 설치 시 기본 모델 포함
ls /opt/hailo/tappas/models/
```

### 사용 가능한 Object Detection 모델

| 모델 | 입력 크기 | FPS (Hailo-8) | 용도 |
|------|----------|---------------|------|
| yolov8n | 640x640 | ~200 | 경량/빠른 추론 |
| yolov8s | 640x640 | ~150 | 균형 |
| yolov8m | 640x640 | ~100 | 정확도 우선 |
| yolov8l | 640x640 | ~70 | 고정확도 |
| yolov5m | 640x640 | ~120 | 레거시 호환 |
| ssd_mobilenet_v2 | 300x300 | ~300 | 초경량 |

---

## 모델 패키지 생성 예시

### 1. 파일 준비

```bash
# HEF 파일 다운로드
wget https://hailo-model-zoo.s3.eu-west-2.amazonaws.com/ModelZoo/Compiled/v2.17.0/hailo8/yolov8n.hef -O model.hef

# model_config.json 생성
cat > model_config.json << 'EOF'
{
  "model_id": "yolov8n-coco",
  "name": "YOLOv8 Nano COCO 80 Classes",
  "function_name": "yolov8",
  "labels": [
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
    "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
    "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
    "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
    "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake",
    "chair", "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop",
    "mouse", "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
  ],
  "description": "YOLOv8 Nano model trained on COCO dataset with 80 object classes"
}
EOF
```

### 2. ZIP 패키징

```bash
zip yolov8n-coco.zip model.hef model_config.json
```

### 3. 업로드 (Python 예시)

```python
import grpc
from stream_pb2 import UploadModelChunk, UploadModelMetadata
from stream_pb2_grpc import StreamServiceStub

def upload_model(stub, zip_path, overwrite=False):
    def chunk_generator():
        with open(zip_path, 'rb') as f:
            data = f.read()

        # 첫 번째 청크: 메타데이터
        yield UploadModelChunk(
            metadata=UploadModelMetadata(
                filename=zip_path,
                total_size=len(data),
                overwrite=overwrite
            )
        )

        # 이후 청크: 바이너리 데이터 (64KB씩)
        chunk_size = 64 * 1024
        for i in range(0, len(data), chunk_size):
            yield UploadModelChunk(chunk=data[i:i+chunk_size])

    response = stub.UploadModel(chunk_generator())
    return response

# 사용
channel = grpc.insecure_channel('localhost:50051')
stub = StreamServiceStub(channel)

response = upload_model(stub, 'yolov8n-coco.zip')
print(f"Success: {response.success}")
print(f"Model ID: {response.model_id}")
print(f"Message: {response.message}")
```

---

## 관련 파일

| 파일 | 설명 |
|------|------|
| `proto/stream.proto` | gRPC API 정의 |
| `include/model_registry.h` | ModelRegistry 클래스 |
| `src/model_registry.cpp` | ZIP 처리, 저장/로드 구현 |
| `src/grpc_server.cpp` | gRPC 핸들러 |
| `config.yaml` | `models.models_dir` 설정 |

---

## References

- [Hailo Model Zoo (GitHub)](https://github.com/hailo-ai/hailo_model_zoo)
- [Hailo Model Explorer](https://hailo.ai/products/software/model-explorer/)
- [Hailo TAPPAS](https://github.com/hailo-ai/tappas)
