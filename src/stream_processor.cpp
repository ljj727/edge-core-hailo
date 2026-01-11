#include "stream_processor.h"

#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/hailo/tensor_meta.hpp>

// JPEG encoding
#include <jpeglib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace stream_daemon {

namespace {

// JPEG 인코딩 (libjpeg 사용)
std::vector<uint8_t> EncodeJpeg(const uint8_t* rgb_data, int width, int height, int quality) {
    std::vector<uint8_t> jpeg_data;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    // 메모리 출력 설정
    unsigned char* outbuffer = nullptr;
    unsigned long outsize = 0;
    jpeg_mem_dest(&cinfo, &outbuffer, &outsize);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    // 라인 단위로 인코딩
    int row_stride = width * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row_pointer = const_cast<JSAMPROW>(
            rgb_data + cinfo.next_scanline * row_stride);
        jpeg_write_scanlines(&cinfo, &row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    // 결과 복사
    if (outbuffer && outsize > 0) {
        jpeg_data.assign(outbuffer, outbuffer + outsize);
        free(outbuffer);
    }

    return jpeg_data;
}

// COCO 80 클래스 이름
static const char* COCO_LABELS[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
    "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
    "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
    "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
    "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator",
    "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
};
static const int NUM_COCO_LABELS = 80;

// NMS (Non-Maximum Suppression)
float ComputeIoU(const BoundingBox& a, const BoundingBox& b) {
    int x1 = std::max(a.x, b.x);
    int y1 = std::max(a.y, b.y);
    int x2 = std::min(a.x + a.width, b.x + b.width);
    int y2 = std::min(a.y + a.height, b.y + b.height);

    int inter_w = std::max(0, x2 - x1);
    int inter_h = std::max(0, y2 - y1);
    int inter_area = inter_w * inter_h;

    int area_a = a.width * a.height;
    int area_b = b.width * b.height;
    int union_area = area_a + area_b - inter_area;

    return union_area > 0 ? static_cast<float>(inter_area) / union_area : 0.0f;
}

void ApplyNMS(std::vector<Detection>& detections, float iou_threshold = 0.45f) {
    std::sort(detections.begin(), detections.end(),
              [](const Detection& a, const Detection& b) {
                  return a.confidence > b.confidence;
              });

    std::vector<Detection> result;
    std::vector<bool> suppressed(detections.size(), false);

    for (size_t i = 0; i < detections.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(detections[i]);

        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (suppressed[j]) continue;
            if (detections[i].class_id == detections[j].class_id) {
                if (ComputeIoU(detections[i].bbox, detections[j].bbox) > iou_threshold) {
                    suppressed[j] = true;
                }
            }
        }
    }
    detections = std::move(result);
}

// Helper to extract Hailo detections from buffer metadata
std::vector<Detection> ExtractHailoDetections(
    GstBuffer* buffer,
    float confidence_threshold,
    int frame_width = 640,
    int frame_height = 640) {

    std::vector<Detection> detections;
    if (!buffer) return detections;

    // Iterate through all tensor metadata
    gpointer state = nullptr;
    GstHailoTensorMeta* tensor_meta;

    while ((tensor_meta = GST_TENSOR_META_ITERATE(buffer, &state)) != nullptr) {
        const auto& info = tensor_meta->info;

        // Check if this is NMS output (already processed by Hailo)
        if (info.format.is_nms) {
            // NMS format: Hailo has already done post-processing
            // Parse NMS output format
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                const float* data = reinterpret_cast<const float*>(map.data);

                // NMS output format: [num_detections, class_id, score, x1, y1, x2, y2, ...]
                int num_classes = info.nms_shape.number_of_classes;
                int max_bboxes = info.nms_shape.max_bboxes_per_class;

                for (int cls = 0; cls < num_classes && cls < NUM_COCO_LABELS; ++cls) {
                    for (int i = 0; i < max_bboxes; ++i) {
                        int offset = (cls * max_bboxes + i) * 5;  // 5 = score + 4 bbox coords
                        float score = data[offset];

                        if (score < confidence_threshold) continue;

                        Detection det;
                        det.class_id = cls;
                        det.class_name = COCO_LABELS[cls];
                        det.confidence = score;

                        // bbox: x1, y1, x2, y2 (normalized 0-1)
                        float x1 = data[offset + 1];
                        float y1 = data[offset + 2];
                        float x2 = data[offset + 3];
                        float y2 = data[offset + 4];

                        det.bbox.x = static_cast<int>(x1 * frame_width);
                        det.bbox.y = static_cast<int>(y1 * frame_height);
                        det.bbox.width = static_cast<int>((x2 - x1) * frame_width);
                        det.bbox.height = static_cast<int>((y2 - y1) * frame_height);

                        if (det.bbox.width > 0 && det.bbox.height > 0) {
                            detections.push_back(det);
                        }
                    }
                }

                gst_buffer_unmap(buffer, &map);
            }
        } else {
            // Raw tensor output - need YOLOv8 post-processing
            // YOLOv8 output: [1, 84, 8400] where 84 = 4 (bbox) + 80 (classes)
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                const auto& shape = info.shape;
                int num_predictions = shape.width;   // 8400
                int num_outputs = shape.features;    // 84 (4 + 80)

                // Dequantize if needed
                float scale = info.quant_info.qp_scale;
                float zp = info.quant_info.qp_zp;

                const uint8_t* raw_data = map.data;

                for (int i = 0; i < num_predictions; ++i) {
                    // Find best class
                    int best_class = -1;
                    float best_score = 0;

                    for (int c = 4; c < num_outputs && c - 4 < NUM_COCO_LABELS; ++c) {
                        float score;
                        if (info.format.type == HailoTensorFormatType::HAILO_FORMAT_TYPE_FLOAT32) {
                            score = reinterpret_cast<const float*>(raw_data)[i * num_outputs + c];
                        } else {
                            score = (raw_data[i * num_outputs + c] - zp) * scale;
                        }

                        if (score > best_score) {
                            best_score = score;
                            best_class = c - 4;
                        }
                    }

                    if (best_score < confidence_threshold || best_class < 0) continue;

                    // Get bbox (cx, cy, w, h)
                    float cx, cy, w, h;
                    if (info.format.type == HailoTensorFormatType::HAILO_FORMAT_TYPE_FLOAT32) {
                        const float* fdata = reinterpret_cast<const float*>(raw_data);
                        cx = fdata[i * num_outputs + 0];
                        cy = fdata[i * num_outputs + 1];
                        w = fdata[i * num_outputs + 2];
                        h = fdata[i * num_outputs + 3];
                    } else {
                        cx = (raw_data[i * num_outputs + 0] - zp) * scale;
                        cy = (raw_data[i * num_outputs + 1] - zp) * scale;
                        w = (raw_data[i * num_outputs + 2] - zp) * scale;
                        h = (raw_data[i * num_outputs + 3] - zp) * scale;
                    }

                    Detection det;
                    det.class_id = best_class;
                    det.class_name = COCO_LABELS[best_class];
                    det.confidence = best_score;

                    // Convert center format to corner format
                    det.bbox.x = static_cast<int>((cx - w / 2) * frame_width);
                    det.bbox.y = static_cast<int>((cy - h / 2) * frame_height);
                    det.bbox.width = static_cast<int>(w * frame_width);
                    det.bbox.height = static_cast<int>(h * frame_height);

                    // Clamp to frame bounds
                    det.bbox.x = std::max(0, det.bbox.x);
                    det.bbox.y = std::max(0, det.bbox.y);
                    det.bbox.width = std::min(det.bbox.width, frame_width - det.bbox.x);
                    det.bbox.height = std::min(det.bbox.height, frame_height - det.bbox.y);

                    if (det.bbox.width > 0 && det.bbox.height > 0) {
                        detections.push_back(det);
                    }
                }

                gst_buffer_unmap(buffer, &map);
            }
        }

        // Only process first tensor (main output)
        break;
    }

    // Apply NMS
    if (!detections.empty()) {
        ApplyNMS(detections);
    }

    return detections;
}

}  // namespace

// ============================================================================
// Factory Method
// ============================================================================

Result<std::unique_ptr<StreamProcessor>> StreamProcessor::Create(
    const StreamInfo& info,
    std::shared_ptr<NatsPublisher> nats_publisher) {

    if (info.stream_id.empty()) {
        return std::string("Stream ID cannot be empty");
    }

    if (info.rtsp_url.empty()) {
        return std::string("RTSP URL cannot be empty");
    }

    // hef_path는 선택: 비어있으면 영상만 스트림 (추론 없음)

    auto processor = std::unique_ptr<StreamProcessor>(
        new StreamProcessor(info, std::move(nats_publisher)));

    return processor;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

StreamProcessor::StreamProcessor(
    const StreamInfo& info,
    std::shared_ptr<NatsPublisher> nats_publisher)
    : stream_id_(info.stream_id)
    , rtsp_url_(info.rtsp_url)
    , hef_path_(info.hef_path)
    , model_id_(info.model_id)
    , config_(info.config)
    , task_(info.task.empty() ? "det" : info.task)
    , num_keypoints_(info.num_keypoints)
    , labels_(info.labels)
    , nats_publisher_(std::move(nats_publisher))
    , frame_width_(0)   // Auto-detect from RTSP stream
    , frame_height_(0)  // Auto-detect from RTSP stream
    , event_compositor_(std::make_unique<EventCompositor>()) {
}

StreamProcessor::~StreamProcessor() {
    Stop();
}

// ============================================================================
// Lifecycle Management
// ============================================================================

VoidResult StreamProcessor::Start() {
    if (state_ == StreamState::kRunning || state_ == StreamState::kStarting) {
        return MakeOk();
    }

    SetState(StreamState::kStarting);
    LogInfo("Starting stream: " + stream_id_);

    if (auto result = CreatePipeline(); IsError(result)) {
        SetError(GetError(result));
        SetState(StreamState::kError);
        return result;
    }

    // Set pipeline to playing
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        DestroyPipeline();
        SetError("Failed to start pipeline");
        SetState(StreamState::kError);
        return MakeError("Failed to start GStreamer pipeline");
    }

    start_time_ = std::chrono::steady_clock::now();
    last_fps_update_ = start_time_;
    frame_count_ = 0;
    frames_since_last_update_ = 0;
    reconnect_attempts_ = 0;

    SetState(StreamState::kRunning);
    LogInfo("Stream started: " + stream_id_);

    return MakeOk();
}

void StreamProcessor::Stop() {
    if (state_ == StreamState::kStopped) {
        return;
    }

    LogInfo("Stopping stream: " + stream_id_);

    CancelReconnect();
    DestroyPipeline();

    SetState(StreamState::kStopped);
    LogInfo("Stream stopped: " + stream_id_);
}

VoidResult StreamProcessor::Update(const StreamInfo& new_info) {
    LogInfo("Updating stream: " + stream_id_);

    // Stop current pipeline
    Stop();

    // Update configuration
    rtsp_url_ = new_info.rtsp_url;
    if (!new_info.hef_path.empty()) {
        hef_path_ = new_info.hef_path;
    }
    if (!new_info.model_id.empty()) {
        model_id_ = new_info.model_id;
    }
    config_ = new_info.config;

    // Update model info
    if (!new_info.task.empty()) {
        task_ = new_info.task;
    }
    num_keypoints_ = new_info.num_keypoints;
    if (!new_info.labels.empty()) {
        labels_ = new_info.labels;
    }

    // Restart with new configuration
    return Start();
}

VoidResult StreamProcessor::ClearInference() {
    LogInfo("Clearing inference from stream: " + stream_id_);

    // Stop current pipeline
    Stop();

    // Clear inference-related state
    hef_path_.clear();
    model_id_.clear();
    hailo_inference_.reset();

    // Restart in video-only mode
    return Start();
}

Result<std::vector<std::string>> StreamProcessor::UpdateEventSettings(
    const std::string& settings_json) {
    if (!event_compositor_) {
        return MakeErrorT<std::vector<std::string>>("EventCompositor not initialized");
    }
    return event_compositor_->UpdateSettings(settings_json);
}

void StreamProcessor::ClearEventSettings() {
    if (event_compositor_) {
        event_compositor_->ClearSettings();
    }
}

// ============================================================================
// Status & Snapshot
// ============================================================================

StreamStatus StreamProcessor::GetStatus() const {
    StreamStatus status;
    status.stream_id = stream_id_;
    status.rtsp_url = rtsp_url_;
    status.model_id = model_id_;
    status.state = state_.load();
    status.frame_count = frame_count_.load();
    status.current_fps = current_fps_.load();
    status.last_detection_time = last_detection_time_.load();

    // Calculate uptime
    if (state_ == StreamState::kRunning || state_ == StreamState::kReconnecting) {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
        status.uptime_seconds = static_cast<uint64_t>(duration.count());
    }

    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        status.last_error = last_error_;
    }

    return status;
}

std::optional<std::vector<uint8_t>> StreamProcessor::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (last_snapshot_.empty()) {
        return std::nullopt;
    }
    return last_snapshot_;
}

// ============================================================================
// Callback Setters
// ============================================================================

void StreamProcessor::SetDetectionCallback(DetectionCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    detection_callback_ = std::move(callback);
}

void StreamProcessor::SetStateChangeCallback(StateChangeCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_change_callback_ = std::move(callback);
}

void StreamProcessor::SetErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    error_callback_ = std::move(callback);
}

// ============================================================================
// Pipeline Management
// ============================================================================

VoidResult StreamProcessor::CreatePipeline() {
    // Initialize HailoRT inference if HEF path is specified
    if (!hef_path_.empty()) {
        auto inference_result = HailoInference::GetInstance(hef_path_);
        if (IsError(inference_result)) {
            return MakeError("Failed to initialize Hailo inference: " + GetError(inference_result));
        }
        hailo_inference_ = GetValue(inference_result);

        // Set model configuration for proper output parsing
        hailo_inference_->SetModelConfig(task_, num_keypoints_, labels_);

        LogInfo("HailoRT inference initialized (shared instance)");
    }

    const std::string pipeline_str = BuildPipelineString();
    LogInfo("Creating pipeline: " + pipeline_str);

    GError* error = nullptr;
    pipeline_ = gst_parse_launch(pipeline_str.c_str(), &error);

    if (error) {
        std::string error_msg = "Failed to create pipeline: " + std::string(error->message);
        g_error_free(error);
        return MakeError(error_msg);
    }

    if (!pipeline_) {
        return MakeError("Failed to create pipeline: unknown error");
    }

    // Get appsink element
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    if (!appsink_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return MakeError("Failed to get appsink element");
    }

    // Configure appsink
    g_object_set(appsink_,
        "emit-signals", TRUE,
        "max-buffers", 1,
        "drop", TRUE,
        nullptr);

    // Connect new-sample signal
    g_signal_connect(appsink_, "new-sample", G_CALLBACK(OnNewSample), this);

    // Setup bus watch for messages
    bus_ = gst_element_get_bus(pipeline_);
    bus_watch_id_ = gst_bus_add_watch(bus_, OnBusMessage, this);

    return MakeOk();
}

void StreamProcessor::DestroyPipeline() {
    if (reconnect_source_id_ > 0) {
        g_source_remove(reconnect_source_id_);
        reconnect_source_id_ = 0;
    }

    if (bus_watch_id_ > 0) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);

        if (appsink_) {
            gst_object_unref(appsink_);
            appsink_ = nullptr;
        }

        if (bus_) {
            gst_object_unref(bus_);
            bus_ = nullptr;
        }

        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }

    hailo_probe_id_ = 0;
}

std::string StreamProcessor::BuildPipelineString() const {
    std::ostringstream oss;

    // RTSP source with reconnection settings
    oss << "rtspsrc location=\"" << rtsp_url_ << "\" "
        << "latency=0 "
        << "timeout=10000000 "
        << "retry=3 "
        << "protocols=tcp "
        << "name=src "
        << "! rtph264depay "
        << "! h264parse "
        << "! avdec_h264 ";

    // Auto-detect RTSP stream resolution (no forced scaling)
    // HailoInference handles resize internally for model input
    if (hailo_inference_ && hailo_inference_->IsReady()) {
        LogInfo("Inference enabled (model input: " +
                std::to_string(hailo_inference_->GetInputWidth()) + "x" +
                std::to_string(hailo_inference_->GetInputHeight()) +
                ", video: auto-detect)");
    } else if (!hef_path_.empty()) {
        LogInfo("HEF specified, will initialize inference on first frame");
    } else {
        LogInfo("Running in video-only mode (no inference)");
    }

    // Output to appsink (RGB format for JPEG encoding and inference)
    // No videoscale - preserve original RTSP stream resolution
    oss << "! videoconvert "
        << "! video/x-raw,format=RGB "
        << "! appsink name=sink emit-signals=true max-buffers=1 drop=true sync=false";

    return oss.str();
}

// ============================================================================
// Reconnection
// ============================================================================

void StreamProcessor::ScheduleReconnect() {
    if (state_ == StreamState::kStopped) {
        return;
    }

    if (reconnect_attempts_ >= kMaxReconnectAttempts) {
        SetError("Max reconnection attempts reached");
        SetState(StreamState::kError);
        return;
    }

    SetState(StreamState::kReconnecting);
    ++reconnect_attempts_;

    int delay = kReconnectDelaySeconds * reconnect_attempts_;
    LogWarning("Scheduling reconnect for " + stream_id_ +
               " in " + std::to_string(delay) + " seconds (attempt " +
               std::to_string(reconnect_attempts_) + "/" +
               std::to_string(kMaxReconnectAttempts) + ")");

    reconnect_source_id_ = g_timeout_add_seconds(
        static_cast<guint>(delay),
        OnReconnectTimeout,
        this);
}

void StreamProcessor::CancelReconnect() {
    if (reconnect_source_id_ > 0) {
        g_source_remove(reconnect_source_id_);
        reconnect_source_id_ = 0;
    }
}

// ============================================================================
// Detection Processing (with JPEG encoding)
// ============================================================================

void StreamProcessor::ProcessDetections(GstBuffer* buffer) {
    ++frame_count_;
    UpdateFps();

    // Get video frame info
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        LogWarning("Failed to map buffer");
        return;
    }

    // Get frame dimensions from caps (auto-detect from RTSP stream)
    GstCaps* caps = gst_pad_get_current_caps(
        gst_element_get_static_pad(appsink_, "sink"));

    int width = 0;
    int height = 0;

    if (caps) {
        GstStructure* structure = gst_caps_get_structure(caps, 0);
        gst_structure_get_int(structure, "width", &width);
        gst_structure_get_int(structure, "height", &height);
        gst_caps_unref(caps);
    }

    // Log resolution detection on first frame or resolution change
    if (width > 0 && height > 0 &&
        (frame_width_ != width || frame_height_ != height)) {
        LogInfo("Stream " + stream_id_ + " resolution: " +
                std::to_string(width) + "x" + std::to_string(height));
    }

    // Fallback to config if caps not available
    if (width <= 0 || height <= 0) {
        width = config_.width;
        height = config_.height;
    }

    frame_width_ = width;
    frame_height_ = height;

    // Run inference via HailoRT API if available
    std::vector<Detection> detections;
    if (hailo_inference_ && hailo_inference_->IsReady()) {
        detections = hailo_inference_->RunInference(
            map.data, width, height, config_.confidence_threshold);
    }

    // JPEG 인코딩
    auto jpeg_data = EncodeJpeg(map.data, width, height, jpeg_quality_);

    // 스냅샷 저장
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        last_snapshot_ = jpeg_data;
    }

    gst_buffer_unmap(buffer, &map);

    // DetectionEvent 생성
    DetectionEvent event;
    event.stream_id = stream_id_;
    event.timestamp = GetCurrentTimestampMs();
    event.frame_number = frame_count_.load();
    event.fps = current_fps_.load();
    event.width = width;
    event.height = height;
    event.detections = std::move(detections);

    // 이벤트 체크 (각 detection에 event_setting_id 태깅)
    if (!event.detections.empty() && event_compositor_) {
        event_compositor_->CheckEvents(event.detections, width, height);
    }

    // 이미지 포함 여부
    if (publish_images_) {
        event.image_data = std::move(jpeg_data);
    }

    // Detection이 있으면 시간 업데이트
    if (!event.detections.empty()) {
        last_detection_time_ = event.timestamp;
    }

    // NATS 발행 (모든 프레임, detection 유무 상관없이)
    if (nats_publisher_ && nats_publisher_->IsConnected()) {
        if (auto result = nats_publisher_->Publish(event); IsError(result)) {
            LogWarning("Failed to publish to NATS: " + GetError(result));
        }
    }

    // Detection callback 호출
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (detection_callback_) {
            detection_callback_(event);
        }
    }
}

void StreamProcessor::UpdateFps() {
    ++frames_since_last_update_;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_fps_update_).count();

    if (elapsed >= 1000) {  // Update FPS every second
        current_fps_ = static_cast<double>(frames_since_last_update_) * 1000.0 /
                      static_cast<double>(elapsed);
        frames_since_last_update_ = 0;
        last_fps_update_ = now;
    }
}

// ============================================================================
// State Management
// ============================================================================

void StreamProcessor::SetState(StreamState new_state) {
    StreamState old_state = state_.exchange(new_state);

    if (old_state != new_state) {
        LogInfo("Stream " + stream_id_ + " state changed: " +
                std::string(StreamStateToString(old_state)) + " -> " +
                std::string(StreamStateToString(new_state)));

        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (state_change_callback_) {
            state_change_callback_(stream_id_, new_state);
        }
    }
}

void StreamProcessor::SetError(std::string_view error) {
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = std::string(error);
    }

    LogError("Stream " + stream_id_ + " error: " + std::string(error));

    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (error_callback_) {
        error_callback_(stream_id_, error);
    }
}

// ============================================================================
// GStreamer Callbacks
// ============================================================================

GstFlowReturn StreamProcessor::OnNewSample(GstElement* sink, gpointer user_data) {
    auto* self = static_cast<StreamProcessor*>(user_data);

    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (buffer) {
        self->ProcessDetections(buffer);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

gboolean StreamProcessor::OnBusMessage(
    [[maybe_unused]] GstBus* bus,
    GstMessage* msg,
    gpointer user_data) {

    auto* self = static_cast<StreamProcessor*>(user_data);

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);

            std::string error_msg = err ? err->message : "Unknown error";
            self->SetError(error_msg);

            if (err) g_error_free(err);
            if (debug) g_free(debug);

            // Stop pipeline and schedule reconnect
            self->DestroyPipeline();
            self->ScheduleReconnect();
            break;
        }

        case GST_MESSAGE_EOS: {
            LogWarning("Stream " + self->stream_id_ + " received EOS");
            self->DestroyPipeline();
            self->ScheduleReconnect();
            break;
        }

        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->pipeline_)) {
                GstState old_state, new_state, pending;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);

                if (new_state == GST_STATE_PLAYING &&
                    self->state_ != StreamState::kRunning) {
                    self->SetState(StreamState::kRunning);
                    self->reconnect_attempts_ = 0;
                }
            }
            break;
        }

        case GST_MESSAGE_WARNING: {
            GError* err = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_warning(msg, &err, &debug);

            if (err) {
                LogWarning("Stream " + self->stream_id_ +
                          " warning: " + std::string(err->message));
                g_error_free(err);
            }
            if (debug) g_free(debug);
            break;
        }

        default:
            break;
    }

    return TRUE;
}

// Hailo NMS output format parser
// NMS BY CLASS: for each class: [num_detections, bbox1, bbox2, ...]
// bbox format: [y_min, x_min, y_max, x_max, score]
std::vector<Detection> ParseHailoNmsOutput(
    const float* data,
    size_t data_size,
    int num_classes,
    int max_bboxes_per_class,
    float confidence_threshold,
    int frame_width,
    int frame_height) {

    std::vector<Detection> detections;

    // NMS output structure (per class):
    // - First value: number of detections for this class
    // - Then: [y_min, x_min, y_max, x_max, score] for each detection
    const int bbox_params = 5;  // y_min, x_min, y_max, x_max, score
    const int class_stride = 1 + max_bboxes_per_class * bbox_params;

    for (int cls = 0; cls < num_classes && cls < 80; ++cls) {
        size_t class_offset = cls * class_stride;
        if (class_offset >= data_size) break;

        int num_dets = static_cast<int>(data[class_offset]);
        if (num_dets <= 0 || num_dets > max_bboxes_per_class) continue;

        for (int i = 0; i < num_dets; ++i) {
            size_t bbox_offset = class_offset + 1 + i * bbox_params;
            if (bbox_offset + bbox_params > data_size) break;

            float y_min = data[bbox_offset + 0];
            float x_min = data[bbox_offset + 1];
            float y_max = data[bbox_offset + 2];
            float x_max = data[bbox_offset + 3];
            float score = data[bbox_offset + 4];

            if (score < confidence_threshold) continue;

            Detection det;
            det.class_id = cls;
            det.class_name = (cls < NUM_COCO_LABELS) ? COCO_LABELS[cls] : "unknown";
            det.confidence = score;

            // Convert normalized coords to pixels
            det.bbox.x = static_cast<int>(x_min * frame_width);
            det.bbox.y = static_cast<int>(y_min * frame_height);
            det.bbox.width = static_cast<int>((x_max - x_min) * frame_width);
            det.bbox.height = static_cast<int>((y_max - y_min) * frame_height);

            if (det.bbox.width > 0 && det.bbox.height > 0) {
                detections.push_back(det);
            }
        }
    }

    return detections;
}

GstPadProbeReturn StreamProcessor::OnHailoProbe(
    [[maybe_unused]] GstPad* pad,
    GstPadProbeInfo* info,
    gpointer user_data) {

    auto* self = static_cast<StreamProcessor*>(user_data);
    static int probe_count = 0;

    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    ++probe_count;

    // Debug: Print all metadata types attached to buffer
    if (probe_count == 1) {
        gpointer state = nullptr;
        GstMeta* meta;
        LogInfo("HailoProbe: Listing all metadata on first buffer:");
        while ((meta = gst_buffer_iterate_meta(buffer, &state)) != nullptr) {
            const GstMetaInfo* info = meta->info;
            LogInfo("  - Meta type: " + std::string(g_type_name(info->api)));
        }
    }

    // Try to get tensor metadata first
    guint tensor_count = GST_TENSOR_META_COUNT(buffer);

    if (tensor_count > 0) {
        if (probe_count == 1) {
            LogInfo("HailoProbe: Found " + std::to_string(tensor_count) + " tensor metas");
        }

        // Use tensor metadata with actual frame dimensions
        int fw = self->frame_width_ > 0 ? self->frame_width_ : self->config_.width;
        int fh = self->frame_height_ > 0 ? self->frame_height_ : self->config_.height;
        auto detections = ExtractHailoDetections(
            buffer,
            self->config_.confidence_threshold,
            fw, fh);

        if (!detections.empty()) {
            std::lock_guard<std::mutex> lock(self->detection_mutex_);
            self->pending_detections_ = std::move(detections);
        }
    } else {
        // No tensor metadata - try to get NMS output from buffer memories
        guint num_mems = gst_buffer_n_memory(buffer);

        if (probe_count == 1 || probe_count % 100 == 0) {
            LogInfo("HailoProbe: buffer has " + std::to_string(num_mems) +
                    " memory blocks, no tensor meta");
        }

        // For NMS models, the output should be in a separate memory block
        // Try each memory block to find NMS data
        for (guint i = 0; i < num_mems; ++i) {
            GstMemory* mem = gst_buffer_peek_memory(buffer, i);
            if (!mem) continue;

            GstMapInfo map;
            if (gst_memory_map(mem, &map, GST_MAP_READ)) {
                // Debug: Print memory block info on first buffer
                if (probe_count == 1) {
                    LogInfo("HailoProbe: mem[" + std::to_string(i) + "] size=" +
                            std::to_string(map.size) + " bytes");

                    // Print first few floats if size is reasonable
                    if (map.size < 200000 && map.size >= sizeof(float) * 10) {
                        const float* fdata = reinterpret_cast<const float*>(map.data);
                        std::ostringstream oss;
                        oss << "HailoProbe: First 10 floats: ";
                        for (int j = 0; j < 10 && j < static_cast<int>(map.size / sizeof(float)); ++j) {
                            oss << fdata[j] << " ";
                        }
                        LogInfo(oss.str());
                    }
                }

                // Skip if this looks like video frame data (too large)
                // Original resolution RGB is much larger than NMS output
                size_t expected_frame_size = static_cast<size_t>(self->config_.width) *
                                             static_cast<size_t>(self->config_.height) * 3;
                if (map.size >= expected_frame_size / 2) {
                    gst_memory_unmap(mem, &map);
                    continue;
                }

                // Try to parse as NMS output
                if (map.size >= sizeof(float) * 6) {  // At least one detection possible
                    const float* nms_data = reinterpret_cast<const float*>(map.data);
                    size_t num_floats = map.size / sizeof(float);

                    int fw = self->frame_width_ > 0 ? self->frame_width_ : self->config_.width;
                    int fh = self->frame_height_ > 0 ? self->frame_height_ : self->config_.height;
                    auto detections = ParseHailoNmsOutput(
                        nms_data, num_floats,
                        80, 100,  // 80 classes, 100 max bboxes per class
                        self->config_.confidence_threshold,
                        fw, fh);

                    if (!detections.empty()) {
                        if (probe_count % 30 == 1) {
                            LogInfo("HailoProbe: Found " + std::to_string(detections.size()) +
                                    " detections in mem[" + std::to_string(i) + "]");
                        }
                        std::lock_guard<std::mutex> lock(self->detection_mutex_);
                        self->pending_detections_ = std::move(detections);
                    }
                }

                gst_memory_unmap(mem, &map);
            }
        }
    }

    return GST_PAD_PROBE_OK;
}

gboolean StreamProcessor::OnReconnectTimeout(gpointer user_data) {
    auto* self = static_cast<StreamProcessor*>(user_data);
    self->reconnect_source_id_ = 0;

    LogInfo("Attempting reconnect for stream: " + self->stream_id_);

    if (auto result = self->Start(); IsError(result)) {
        LogError("Reconnect failed: " + GetError(result));
        self->ScheduleReconnect();
    }

    return G_SOURCE_REMOVE;
}

}  // namespace stream_daemon
