# best12.hef Pose Model Optimization Plan

## Overview
- **Model**: best12.hef (YOLOv8s-pose)
- **Input**: 960x960 RGB
- **Classes**: 12-13 (vehicle types)
- **Keypoints**: 4 per detection
- **Issue**: NPU timeout after 2-3 frames in current implementation

## Current Status
- Model works for first 2-3 frames, then D2H transfer times out
- NMS output: 60 classes, 60 max bboxes/class, 48 params/detection
- Detection parsing works (found 2 detections on first frame)
- Other company successfully uses this model

## Tasks

### 1. Inference Speed Test
- [ ] Test with hailortcli to verify model works standalone
- [ ] Measure actual inference time per frame
- [ ] Check if model meets real-time requirements (30fps = 33ms/frame)

### 2. Model Structure Analysis
- [ ] Verify output layer format
- [ ] Check NMS configuration in HEF
- [ ] Compare with working implementation

### 3. VStream Synchronization Fix
- [ ] Current issue: synchronous write/read blocking
- [ ] Try async vstreams
- [ ] Implement proper buffer management
- [ ] Add frame rate limiting for heavy models

### 4. Pipeline Optimization
- [ ] Separate inference thread from video processing
- [ ] Implement inference queue
- [ ] Add frame skipping when inference can't keep up

### 5. Testing & Validation
- [ ] Verify detection accuracy
- [ ] Measure end-to-end latency
- [ ] Test with real RTSP streams

## Technical Details

### Model Output Format (48 params per detection)
```
Slot: [y_min, x_min, y_max, x_max, score, kp1_x, kp1_y, kp1_conf, ...]
Total: 60 classes × 60 bboxes × 48 params = 172,800 floats
```

### Expected Performance
- Model input: 960×960 = 921,600 pixels
- Output size: 172,800 × 4 bytes = 691KB
- Target: < 100ms inference for stable operation

## Progress Log
- 2026-01-11: Initial analysis, identified timeout issue
- 2026-01-11: NMS parsing fixed, detections found on first frames
