# EventCompositor (EC) 통신 스펙

Core 개발자를 위한 기존 EventCompositor 동작 방식 문서.

---

## 1. Backend → EC: 이벤트 설정 전송

### NATS Subject
```
stream_id.{video_id}.app_id.{app_id}.update
```

예시: `stream_id.cam01.app_id.app001.update`

### 통신 방식
- **NATS Request-Response** (동기 요청)
- Timeout: 10초

### 요청 메시지 (InferenceSettings)
```json
{
  "version": "1.6.1",
  "configs": [
    {
      "eventType": "ROI",
      "eventSettingId": "evt001",
      "eventSettingName": "입구 감지",
      "points": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]],
      "target": {
        "label": "person",
        "classType": null,
        "resultLabel": null
      },
      "timeout": 5.0,
      "detectionPoint": "c:b"
    },
    {
      "eventType": "Line",
      "eventSettingId": "evt002",
      "eventSettingName": "라인 크로싱",
      "points": [[0.0, 0.5], [1.0, 0.5]],
      "direction": "A2B",
      "target": {
        "label": "person",
        "classType": null,
        "resultLabel": null
      }
    },
    {
      "eventType": "And",
      "eventSettingId": "evt003",
      "eventSettingName": "복합 이벤트",
      "inOrder": true,
      "ncond": ">=2"
    },
    {
      "eventType": "Filter",
      "eventSettingId": "evt004",
      "parentId": "evt001",
      "points": [[0.2, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]]
    }
  ]
}
```

### 응답 메시지 (ConfigResultMSG)
```json
{
  "status": 1,
  "result": "Success",
  "term_ev_list": ["evt001", "evt003"]
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| status | int | 0=실패, 1=성공 |
| result | string | 성공/에러 메시지 |
| term_ev_list | string[] | 터미널 이벤트 ID 목록 (이벤트 발생 시 이 ID들로 publish) |

---

## 2. 이벤트 타입 상세 설명

### ROI (Region of Interest) - 영역 감지
지정된 다각형 영역 안에 객체가 들어오면 이벤트 발생.

```
┌─────────────────┐
│    ┌───────┐    │
│    │  ROI  │ ← 사람이 이 영역에 들어오면 이벤트
│    │ 영역  │    │
│    └───────┘    │
└─────────────────┘
```

- **points**: 다각형 꼭짓점 좌표 (최소 3개)
- **target**: 감지할 객체 (person, car 등)
- **timeout**: 영역 내 체류 시간 조건 (초)
- **detectionPoint**: 객체의 어느 지점을 기준으로 판단할지 (c:b = 발 위치)

---

### Line - 라인 크로싱
지정된 선을 객체가 통과하면 이벤트 발생.

```
        A ──────────────── B
              ↑
        사람이 이 선을 넘으면 이벤트
```

- **points**: 선의 시작점(A)과 끝점(B) 좌표 (정확히 2개)
- **direction**:
  - `A2B`: A→B 방향으로 넘을 때만
  - `B2A`: B→A 방향으로 넘을 때만
  - `BOTH`: 양방향 모두
- **target**: 감지할 객체

---

### And - 논리 AND (복합 이벤트)
여러 자식 이벤트가 **모두** 발생해야 이벤트 발생.

```
And 이벤트
├── ROI 이벤트 (입구 진입) ← 먼저 발생
└── Line 이벤트 (라인 통과) ← 그 다음 발생

둘 다 발생해야 And 이벤트 트리거
```

- **inOrder**: true면 순서대로 발생해야 함
- **ncond**: 조건 (예: `>=2` = 2개 이상 만족)
- 자식 이벤트들은 **parentId**로 이 And 이벤트를 참조

---

### Or - 논리 OR (복합 이벤트)
여러 자식 이벤트 중 **하나라도** 발생하면 이벤트 발생.

```
Or 이벤트
├── ROI 이벤트 (영역 A)
└── ROI 이벤트 (영역 B)

둘 중 하나만 발생해도 Or 이벤트 트리거
```

- **ncond**: 조건 (예: `>=1` = 1개 이상)
- 자식 이벤트들은 **parentId**로 이 Or 이벤트를 참조

---

### Speed - 속도 감지
두 개의 Line을 순차적으로 통과하는 시간으로 속도 계산.

```
Line A ─────────────────
          ↓ 통과
          (시간 측정)
          ↓ 통과
Line B ─────────────────

두 라인 간 거리 / 통과 시간 = 속도
```

- 자식으로 **Line 이벤트 2개** 필요
- 각 Line에 **turn** 값 필요 (0 또는 1, 순서 구분)

---

### HM (Heatmap) - 히트맵
객체들의 이동 경로를 히트맵으로 시각화.

- **regenInterval**: 히트맵 재생성 주기 (초)

---

### Filter - 필터
부모 이벤트의 결과를 추가 영역으로 필터링.

```
ROI 이벤트 (넓은 영역)
    └── Filter (좁은 영역)

ROI에서 감지된 것 중 Filter 영역 안에 있는 것만 통과
```

- **parentId**: 필터링할 부모 이벤트 ID (필수)
- **points**: 필터 영역 좌표 (선택)

---

### EnEx (Enter-Exit) - 입출 카운팅
영역에 들어온 객체와 나간 객체 수를 카운팅.

```
┌─────────┐
│  EnEx   │  들어온 수: 10
│  영역   │  나간 수: 8
└─────────┘  현재 내부: 2
```

- **target**: 카운팅할 객체

---

### Alarm - 알람
이벤트 발생 시 외부 알람 트리거.

- **ext**: 알람 설정 문자열 (외부 시스템 연동 정보)

---

## 3. 이벤트 설정 필드 스펙

### 필수/선택 필드 정리

| Type | 필수 필드 | 선택 필드 |
|------|-----------|-----------|
| `ROI` | points (>=3), target | timeout, detectionPoint |
| `Line` | points (=2), direction, target | detectionPoint |
| `And` | inOrder | ncond, timeout |
| `Or` | - | ncond |
| `Speed` | Line 자식 2개 (각각 turn) | - |
| `HM` | regenInterval | - |
| `Filter` | parentId | points |
| `EnEx` | target | - |
| `Alarm` | ext | - |

### 공통 필드

```json
{
  "eventType": "ROI",
  "eventSettingId": "고유 ID",
  "eventSettingName": "표시 이름",
  "parentId": "부모 이벤트 ID (자식일 경우)",
  "points": [[x1, y1], [x2, y2], ...],
  "target": {
    "label": "person",
    "classType": "classifier",
    "resultLabel": ["red", "blue"]
  },
  "timeout": 5.0,
  "regenInterval": 60.0,
  "ncond": ">=2",
  "direction": "A2B",
  "inOrder": true,
  "turn": 0,
  "detectionPoint": "c:b",
  "ext": "alarm config string"
}
```

### detectionPoint 값
```
l:t (leftTop)      c:t (centerTop)      r:t (rightTop)
l:c (leftCenter)   c:c (center)         r:c (rightCenter)
l:b (leftBottom)   c:b (centerBottom)   r:b (rightBottom)
```

### direction 값 (Line 전용)
- `A2B`: 첫 번째 점 → 두 번째 점 방향
- `B2A`: 두 번째 점 → 첫 번째 점 방향
- `BOTH`: 양방향

---

## 3. EC → Backend: 이벤트 발생 알림

### NATS Subject
```
event.updated.{stream_id}
```

예시: `event.updated.cam01`

### 통신 방식
- **NATS Publish** (Fire-and-forget)

### 이벤트 메시지 (예상 구조)
```json
{
  "event_id": "uuid",
  "event_setting_id": "evt001",
  "stream_id": "cam01",
  "timestamp": 1736502000000,
  "objects": [
    {
      "track_id": 123,
      "label": "person",
      "confidence": 0.95,
      "bbox": {"x": 100, "y": 200, "width": 50, "height": 120}
    }
  ]
}
```

---

## 4. 전체 흐름 다이어그램

```
┌──────────┐                    ┌────────────────────┐
│ Backend  │                    │ EventCompositor(EC)│
└────┬─────┘                    └─────────┬──────────┘
     │                                    │
     │  1. NATS Request                   │
     │  Subject: stream_id.{vid}.app_id.{aid}.update
     │  Body: InferenceSettings           │
     │ ──────────────────────────────────>│
     │                                    │
     │                                    │ 2. EC가 설정 파싱/저장
     │                                    │    - 이벤트 트리 구성
     │                                    │    - 터미널 이벤트 식별
     │                                    │
     │  3. NATS Response                  │
     │  Body: ConfigResultMSG             │
     │ <──────────────────────────────────│
     │  {status, result, term_ev_list}    │
     │                                    │
     │                                    │ 4. EC가 프레임 분석 중
     │                                    │    이벤트 조건 만족 시
     │                                    │
     │  5. NATS Publish                   │
     │  Subject: event.updated.{stream_id}│
     │ <──────────────────────────────────│
     │  Body: 이벤트 정보                  │
     │                                    │
```

---

## 5. 핵심 개념: eventSettingId가 키

### 설정 → 감지 → 이벤트 발행 흐름

**1단계: Backend → EC (설정 전송)**
```json
{
  "version": "1.6.1",
  "configs": [
    {
      "eventSettingId": "evt001",      // ← 이 ID가 핵심!
      "eventSettingName": "입구 감지",
      "eventType": "ROI",
      "points": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]],
      "target": {"label": "person"}
    }
  ]
}
```

**2단계: EC 내부 동작**
```
EC가 설정을 메모리에 저장:
- evt001 = ROI 이벤트, 이 영역, person 감지

영상 프레임마다:
- 객체 감지 (YOLO 등)
- 각 이벤트 조건 체크
- evt001 조건 만족? → 이벤트 발행!
```

**3단계: EC → Backend (이벤트 발행)**
```json
{
  "metadata": {
    "streamId": "cam01",
    "appId": "app001",
    "timestamp": 1736502000000000000
  },
  "events": [
    {
      "eventSettingId": "evt001",      // ← 설정할 때 준 ID 그대로!
      "eventSettingName": "입구 감지",
      "objects": [
        {"trackId": 123, "label": "person", "bbox": {...}, "score": 0.95}
      ]
    }
  ]
}
```

### 왜 이렇게 동작하나?

1. **Backend는 모든 이벤트 설정을 알고 있음** (DB에 저장)
2. **EC는 설정 받아서 감지만 함**
3. **이벤트 발생 시 `eventSettingId`만 보내면**
4. **Backend가 "아 evt001이 발생했구나" 알 수 있음**

→ EC는 무거운 메타데이터 안 보내도 됨, ID만 보내면 OK

---

## 6. 새 Core 구현 가이드

### 기존 EC 방식 (NATS)
```
1. stream_id.*.app_id.*.update 패턴 구독
2. InferenceSettings 파싱 → 이벤트 트리 구성
3. ConfigResultMSG 응답 (term_ev_list 포함)
4. 이벤트 발생 시 event.updated.{stream_id}로 publish
```

### 새 Core 방식 (gRPC 권장)

**설정 수신** - gRPC RPC 추가
```protobuf
rpc UpdateEventSetting(EventSettingReq) returns (EventSettingRes);

message EventSettingReq {
  string app_id = 1;
  string video_id = 2;
  string settings_json = 3;  // InferenceSettings JSON 문자열
}

message EventSettingRes {
  bool result = 1;
  string message = 2;
  repeated string term_ev_list = 3;
}
```

**이벤트 발행** - NATS 유지 또는 gRPC 스트림
```
옵션 1: NATS 유지 (기존 방식)
- Subject: event.updated.{stream_id}
- Backend가 이미 구독하고 있음

옵션 2: gRPC 스트림 (새 방식)
- rpc SubscribeEvents(...) returns (stream EventMsg);
- Backend가 스트림으로 이벤트 수신
```

### Core가 구현해야 할 핵심 로직

```
1. 이벤트 설정 수신 (gRPC)
   - settings_json 파싱
   - eventSettingId별로 메모리에 저장
   - term_ev_list 계산해서 응답

2. 프레임별 이벤트 체크
   - 객체 감지 결과 받음
   - 각 eventSettingId의 조건 체크
     - ROI: 객체가 영역 안에 있나?
     - Line: 객체가 선을 넘었나?
     - And/Or: 자식 이벤트들 조합
     - 등등...

3. 이벤트 발생 시 발행
   - eventSettingId + 감지된 objects + timestamp
   - Backend로 전송 (NATS 또는 gRPC)
```

---

## 7. 참고: Backend 코드 위치

| 역할 | 파일 |
|------|------|
| NATS Publisher | `app/workers/nats_publisher.py` |
| NATS Subscriber | `app/workers/nats_subscriber.py` |
| 이벤트 스키마 | `app/schemas/inference.py` |
| 이벤트 저장 | `app/services/event_service.py` |
| 이벤트 모델 | `app/models/event.py` |

---

*작성일: 2025-01-10*
