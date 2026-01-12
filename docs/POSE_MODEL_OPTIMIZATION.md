# best12.hef Pose Model Optimization Plan

## Overview
- **Model**: best12.hef (YOLOv8s-pose)
- **Input**: 960x960 RGB
- **Classes**: 12-13 (vehicle types)
- **Keypoints**: 4 per detection
- **Status**: RESOLVED

## Root Cause
The model has **5 output layers** but the code was only reading from **1 output vstream**, causing the other 4 output buffers to fill up and trigger HAILO_TIMEOUT errors.

### Model Output Layers
```
Output[0] 'best12/conv39': 691,200 bytes (120x120x12)
Output[1] 'best12/conv47': 172,800 bytes (60x60x12)
Output[2] 'best12/conv58': 230,400 bytes (30x30x64)
Output[3] 'best12/conv59':  50,400 bytes (30x30x14)
Output[4] 'best12/conv60':  43,200 bytes (30x30x12)
```

## Tasks - COMPLETED

### 1. Inference Speed Test
- [x] Test with hailortcli to verify model works standalone
- [x] Measure actual inference time per frame: **8.35ms HW latency**
- [x] Check if model meets real-time requirements: **~120 FPS capable**

### 2. Model Structure Analysis
- [x] Verify output layer format: **5 output layers (not single NMS)**
- [x] Check NMS configuration in HEF: **60 classes, 60 bboxes/class**
- [x] Compare with working implementation

### 3. VStream Synchronization Fix
- [x] Identified issue: only reading from 1 of 5 output vstreams
- [x] Fixed: read from ALL output vstreams in RunInference()
- [x] Modified hailo_inference.cpp to handle multiple outputs

### 4. Pipeline Performance
- [x] Verified real-time performance: **~54 FPS achieved**
- [x] No frame skipping needed
- [x] Continuous operation with no timeouts

### 5. Testing & Validation
- [x] Verify detection accuracy: 1-3 detections per frame
- [x] Test with real RTSP streams: Working with cam2 (608x1080)

## Technical Details

### Model Output Format (48 params per detection)
```
Slot: [y_min, x_min, y_max, x_max, score, kp1_x, kp1_y, kp1_conf, ...]
Total: 60 classes x 60 bboxes x 48 params = 172,800 floats
```

### Performance Results
- Model input: 960x960 = 921,600 pixels
- HW inference latency: 8.35ms
- End-to-end FPS: ~54 FPS (with letterbox resize)
- Real-time capable: YES

## Code Changes

### hailo_inference.h
- Changed `output_buffer_` to `output_buffers_` (vector of vectors)
- Added `output_frame_sizes_` vector

### hailo_inference.cpp
- Modified Initialize() to create buffers for ALL output vstreams
- Modified RunInference() to read from ALL output vstreams
- Added logging for multi-output model detection

## Progress Log
- 2026-01-11: Initial analysis, identified timeout issue
- 2026-01-11: NMS parsing fixed, detections found on first frames
- 2026-01-11: Discovered model has 5 outputs via hailortcli
- 2026-01-11: **FIXED** - reading all 5 output vstreams, ~54 FPS achieved
