#include "hailo_inference.h"
#include "batch_inference_manager.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <numeric>
#include <sstream>
#include <thread>

namespace stream_daemon {

// Static member definitions
std::shared_ptr<hailort::VDevice> HailoInference::shared_vdevice_;
std::unordered_map<std::string, std::shared_ptr<HailoInference>> HailoInference::instances_;
std::mutex HailoInference::static_mutex_;

namespace {

// COCO 80 class labels
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

}  // namespace

// Static member function for letterbox resize
HailoInference::LetterboxInfo HailoInference::LetterboxResize(
    const uint8_t* src, int src_w, int src_h,
    uint8_t* dst, int dst_w, int dst_h,
    uint8_t pad_value) {

    LetterboxInfo info;

    // Calculate scale to fit while maintaining aspect ratio
    float scale_w = static_cast<float>(dst_w) / src_w;
    float scale_h = static_cast<float>(dst_h) / src_h;
    info.scale = std::min(scale_w, scale_h);

    // New dimensions after scaling
    info.new_w = static_cast<int>(src_w * info.scale);
    info.new_h = static_cast<int>(src_h * info.scale);

    // Padding to center the image
    info.pad_x = (dst_w - info.new_w) / 2;
    info.pad_y = (dst_h - info.new_h) / 2;

    // Fill destination with padding color (gray 114 is common for YOLO)
    std::memset(dst, pad_value, dst_w * dst_h * 3);

    // Resize and copy to center of destination
    const float x_ratio = static_cast<float>(src_w) / info.new_w;
    const float y_ratio = static_cast<float>(src_h) / info.new_h;

    for (int y = 0; y < info.new_h; ++y) {
        for (int x = 0; x < info.new_w; ++x) {
            int src_x = static_cast<int>(x * x_ratio);
            int src_y = static_cast<int>(y * y_ratio);

            src_x = std::min(src_x, src_w - 1);
            src_y = std::min(src_y, src_h - 1);

            int dst_x = x + info.pad_x;
            int dst_y = y + info.pad_y;

            int dst_idx = (dst_y * dst_w + dst_x) * 3;
            int src_idx = (src_y * src_w + src_x) * 3;

            dst[dst_idx + 0] = src[src_idx + 0];
            dst[dst_idx + 1] = src[src_idx + 1];
            dst[dst_idx + 2] = src[src_idx + 2];
        }
    }

    return info;
}

Result<std::shared_ptr<HailoInference>> HailoInference::GetInstance(
    const std::string& hef_path) {

    std::lock_guard<std::mutex> lock(static_mutex_);

    // Check if instance already exists
    auto it = instances_.find(hef_path);
    if (it != instances_.end()) {
        return it->second;
    }

    // Create new instance
    auto inference = std::shared_ptr<HailoInference>(new HailoInference());

    if (auto result = inference->Initialize(hef_path); IsError(result)) {
        return GetError(result);
    }

    instances_[hef_path] = inference;
    return inference;
}

void HailoInference::ReleaseInstance(const std::string& hef_path) {
    std::lock_guard<std::mutex> lock(static_mutex_);
    instances_.erase(hef_path);
}

void HailoInference::Shutdown() {
    std::lock_guard<std::mutex> lock(static_mutex_);
    instances_.clear();
    shared_vdevice_.reset();
    LogInfo("HailoRT shutdown complete");
}

HailoInference::~HailoInference() {
    is_ready_ = false;
}

VoidResult HailoInference::Initialize(const std::string& hef_path) {
    using namespace hailort;

    hef_path_ = hef_path;
    LogInfo("Initializing HailoRT inference with HEF: " + hef_path);

    // Create shared VDevice if not exists
    if (!shared_vdevice_) {
        auto vdevice_exp = VDevice::create();
        if (!vdevice_exp) {
            return MakeError("Failed to create VDevice: " +
                            std::to_string(static_cast<int>(vdevice_exp.status())));
        }
        shared_vdevice_ = std::shared_ptr<VDevice>(vdevice_exp.release());
        LogInfo("Shared VDevice created for multi-stream inference");
    }

    // Load HEF
    auto hef_exp = Hef::create(hef_path);
    if (!hef_exp) {
        return MakeError("Failed to load HEF: " +
                        std::to_string(static_cast<int>(hef_exp.status())));
    }
    auto hef = hef_exp.release();

    // Configure network group on shared VDevice
    auto network_groups_exp = shared_vdevice_->configure(hef);
    if (!network_groups_exp) {
        return MakeError("Failed to configure network: " +
                        std::to_string(static_cast<int>(network_groups_exp.status())));
    }
    auto network_groups = network_groups_exp.release();

    if (network_groups.empty()) {
        return MakeError("No network groups found in HEF");
    }
    network_group_ = network_groups[0];

    // Get input/output info
    auto input_vstream_infos = network_group_->get_input_vstream_infos();
    if (!input_vstream_infos) {
        return MakeError("Failed to get input vstream infos");
    }

    auto output_vstream_infos = network_group_->get_output_vstream_infos();
    if (!output_vstream_infos) {
        return MakeError("Failed to get output vstream infos");
    }

    // Get input dimensions from first input
    if (!input_vstream_infos->empty()) {
        const auto& input_info = (*input_vstream_infos)[0];
        input_height_ = input_info.shape.height;
        input_width_ = input_info.shape.width;

        // Use batch=1 for stable operation
        batch_size_ = 1;

        LogInfo("Model input: " + std::to_string(input_width_) + "x" +
                std::to_string(input_height_) + ", batch=" + std::to_string(batch_size_));
    }

    // Check output for NMS format
    if (!output_vstream_infos->empty()) {
        const auto& output_info = (*output_vstream_infos)[0];
        if (output_info.nms_shape.number_of_classes > 0) {
            is_nms_output_ = true;
            num_classes_ = output_info.nms_shape.number_of_classes;
            max_bboxes_per_class_ = output_info.nms_shape.max_bboxes_per_class;
            LogInfo("NMS output: " + std::to_string(num_classes_) + " classes, " +
                    std::to_string(max_bboxes_per_class_) + " max bboxes/class");
        }
    }

    // Create VStreams with separate params for input (UINT8) and output (FLOAT32)
    hailo_vstream_params_t input_params = HailoRTDefaults::get_vstreams_params();
    input_params.user_buffer_format.type = HAILO_FORMAT_TYPE_UINT8;
    input_params.timeout_ms = 30000;  // 30 second timeout for heavy models

    hailo_vstream_params_t output_params = HailoRTDefaults::get_vstreams_params();
    output_params.user_buffer_format.type = HAILO_FORMAT_TYPE_FLOAT32;
    output_params.timeout_ms = 30000;  // 30 second timeout for heavy models

    // Build input params map
    std::map<std::string, hailo_vstream_params_t> input_params_map;
    for (const auto& info : *input_vstream_infos) {
        input_params_map[info.name] = input_params;
    }

    // Build output params map
    std::map<std::string, hailo_vstream_params_t> output_params_map;
    for (const auto& info : *output_vstream_infos) {
        output_params_map[info.name] = output_params;
    }

    // Create input vstreams
    auto input_vstreams_exp = VStreamsBuilder::create_input_vstreams(*network_group_, input_params_map);
    if (!input_vstreams_exp) {
        return MakeError("Failed to create input vstreams");
    }
    input_vstreams_ = input_vstreams_exp.release();

    // Create output vstreams
    auto output_vstreams_exp = VStreamsBuilder::create_output_vstreams(*network_group_, output_params_map);
    if (!output_vstreams_exp) {
        return MakeError("Failed to create output vstreams");
    }
    output_vstreams_ = output_vstreams_exp.release();

    // Get frame sizes
    if (!input_vstreams_.empty()) {
        input_frame_size_ = input_vstreams_[0].get_frame_size();
        input_buffer_.resize(input_frame_size_);
        LogInfo("Input frame size: " + std::to_string(input_frame_size_) + " bytes");
    }

    // Create buffers for ALL output vstreams (critical for multi-output models like best12.hef)
    // Each read() returns one frame's output, so buffer size = single frame size
    output_buffers_.resize(output_vstreams_.size());
    output_frame_sizes_.resize(output_vstreams_.size());

    for (size_t i = 0; i < output_vstreams_.size(); ++i) {
        output_frame_sizes_[i] = output_vstreams_[i].get_frame_size();
        output_buffers_[i].resize(output_frame_sizes_[i]);
        LogInfo("Output[" + std::to_string(i) + "] '" + output_vstreams_[i].name() +
                "': " + std::to_string(output_frame_sizes_[i]) + " bytes");
    }

    if (output_vstreams_.size() > 1) {
        LogInfo("Multi-output model detected: " + std::to_string(output_vstreams_.size()) + " output vstreams");
        // Multi-output models (like best12.hef) are raw YOLO outputs, not NMS
        is_raw_yolo_output_ = true;
        is_nms_output_ = false;
        LogInfo("Using raw YOLO output parsing (multi-scale feature maps)");
    }

    // Note: Don't manually activate - the scheduler handles activation automatically
    // when using VStreams with shared VDevice

    is_ready_ = true;
    LogInfo("HailoRT inference initialized successfully");

    return MakeOk();
}

std::vector<Detection> HailoInference::RunInference(
    const uint8_t* rgb_data,
    int width,
    int height,
    float confidence_threshold) {

    static int inference_count = 0;

    if (!is_ready_ || input_vstreams_.empty() || output_vstreams_.empty()) {
        LogWarning("RunInference: not ready");
        return {};
    }

    std::lock_guard<std::mutex> lock(inference_mutex_);

    ++inference_count;
    if (inference_count == 1 || inference_count % 100 == 0) {
        LogInfo("RunInference: frame #" + std::to_string(inference_count) +
                " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    }

    // Letterbox resize input (maintains aspect ratio with padding)
    LetterboxInfo letterbox_info;
    if (width != input_width_ || height != input_height_) {
        letterbox_info = LetterboxResize(rgb_data, width, height,
                                          input_buffer_.data(), input_width_, input_height_);
        if (inference_count == 1) {
            LogInfo("RunInference: letterbox resize " + std::to_string(width) + "x" +
                    std::to_string(height) + " -> " + std::to_string(input_width_) + "x" +
                    std::to_string(input_height_) + " (scale=" +
                    std::to_string(letterbox_info.scale) + ", pad=" +
                    std::to_string(letterbox_info.pad_x) + "," +
                    std::to_string(letterbox_info.pad_y) + ")");
        }
    } else {
        std::memcpy(input_buffer_.data(), rgb_data, input_frame_size_);
        letterbox_info.scale = 1.0f;
        letterbox_info.pad_x = 0;
        letterbox_info.pad_y = 0;
        letterbox_info.new_w = width;
        letterbox_info.new_h = height;
    }

    // Write to input vstream
    if (inference_count == 1) {
        LogInfo("RunInference: writing to input vstream...");
    }
    auto status = input_vstreams_[0].write(
        hailort::MemoryView(input_buffer_.data(), input_buffer_.size()));
    if (status != HAILO_SUCCESS) {
        LogWarning("Failed to write to input vstream: " + std::to_string(static_cast<int>(status)));
        // On error, wait a bit before next attempt
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return {};
    }

    // Read from ALL output vstreams (critical to prevent buffer overflow/timeout)
    if (inference_count == 1) {
        LogInfo("RunInference: reading from " + std::to_string(output_vstreams_.size()) + " output vstream(s)...");
    }

    for (size_t i = 0; i < output_vstreams_.size(); ++i) {
        status = output_vstreams_[i].read(
            hailort::MemoryView(output_buffers_[i].data(), output_buffers_[i].size()));
        if (status != HAILO_SUCCESS) {
            LogWarning("Failed to read from output vstream[" + std::to_string(i) + "]: " +
                      std::to_string(static_cast<int>(status)));
            // On error, wait a bit before next attempt
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return {};
        }
    }

    // Parse output - use appropriate parser based on model type
    std::vector<Detection> detections;
    if (is_raw_yolo_output_ && !output_buffers_.empty()) {
        // Multi-output model (like best12.hef) - use raw YOLO parsing
        if (inference_count == 1) {
            LogInfo("Using raw YOLO output parsing for " + std::to_string(output_vstreams_.size()) + " outputs");
        }
        detections = ParseRawYoloOutput(output_buffers_, confidence_threshold, 0.45f,
                                         width, height, letterbox_info);
    } else if (is_nms_output_ && !output_buffers_.empty()) {
        // Single NMS output - parse first vstream
        detections = ParseNmsOutput(output_buffers_[0], confidence_threshold,
                                     width, height, letterbox_info);
    }

    if (inference_count == 1 || (inference_count % 100 == 0 && !detections.empty())) {
        LogInfo("RunInference: found " + std::to_string(detections.size()) + " detections");
    }

    return detections;
}

std::unordered_map<std::string, std::vector<Detection>> HailoInference::RunBatchInference(
    const std::vector<FrameInput>& frames,
    float confidence_threshold) {

    static int batch_inference_count = 0;
    std::unordered_map<std::string, std::vector<Detection>> results;

    if (!is_ready_ || input_vstreams_.empty() || output_vstreams_.empty()) {
        LogWarning("RunBatchInference: not ready");
        return results;
    }

    if (frames.empty()) {
        return results;
    }

    std::lock_guard<std::mutex> lock(inference_mutex_);
    ++batch_inference_count;

    const int num_frames = static_cast<int>(frames.size());
    const int actual_batch = std::min(num_frames, batch_size_);

    if (batch_inference_count == 1 || batch_inference_count % 100 == 0) {
        LogInfo("RunBatchInference: batch #" + std::to_string(batch_inference_count) +
                ", frames=" + std::to_string(num_frames) + "/" + std::to_string(batch_size_));
    }

    // Prepare per-frame buffers (Hailo batch = multiple write() calls, not concatenated buffer)
    const size_t single_frame_size = input_width_ * input_height_ * 3;
    std::vector<std::vector<uint8_t>> frame_buffers(batch_size_, std::vector<uint8_t>(single_frame_size, 114));
    std::vector<LetterboxInfo> letterbox_infos(batch_size_);

    // Process each frame in the batch
    for (int i = 0; i < batch_size_; ++i) {
        uint8_t* dst = frame_buffers[i].data();

        if (i < num_frames) {
            const auto& frame = frames[i];
            if (frame.width != input_width_ || frame.height != input_height_) {
                letterbox_infos[i] = LetterboxResize(frame.rgb_data, frame.width, frame.height,
                                                      dst, input_width_, input_height_);
            } else {
                std::memcpy(dst, frame.rgb_data, single_frame_size);
                letterbox_infos[i].scale = 1.0f;
                letterbox_infos[i].pad_x = 0;
                letterbox_infos[i].pad_y = 0;
                letterbox_infos[i].new_w = frame.width;
                letterbox_infos[i].new_h = frame.height;
            }
        }
        // Else: already padded with gray (114)
    }

    // Write each frame separately (Hailo batch_size=N means N sequential writes before read)
    hailo_status status;
    for (int i = 0; i < batch_size_; ++i) {
        status = input_vstreams_[0].write(
            hailort::MemoryView(frame_buffers[i].data(), frame_buffers[i].size()));
        if (status != HAILO_SUCCESS) {
            LogWarning("RunBatchInference: failed to write frame " + std::to_string(i) +
                      ": " + std::to_string(static_cast<int>(status)));
            return results;
        }
    }

    // For batch mode: read and parse each frame's outputs separately
    // Hailo batch = multiple write() calls followed by multiple read() calls
    // Each read() returns one frame's output
    for (int frame_idx = 0; frame_idx < actual_batch && frame_idx < num_frames; ++frame_idx) {
        // Read from all output vstreams for this frame
        for (size_t i = 0; i < output_vstreams_.size(); ++i) {
            status = output_vstreams_[i].read(
                hailort::MemoryView(output_buffers_[i].data(), output_buffers_[i].size()));
            if (status != HAILO_SUCCESS) {
                LogWarning("RunBatchInference: failed to read output[" + std::to_string(i) +
                          "] for frame " + std::to_string(frame_idx));
                return results;
            }
        }

        // Parse outputs for this frame
        const auto& frame = frames[frame_idx];
        std::vector<Detection> detections;

        if (is_raw_yolo_output_ && !output_buffers_.empty()) {
            // output_buffers_ now contains single frame outputs (already read)
            detections = ParseRawYoloOutput(output_buffers_, confidence_threshold, 0.45f,
                                             frame.width, frame.height, letterbox_infos[frame_idx]);
        } else if (is_nms_output_ && !output_buffers_.empty()) {
            detections = ParseNmsOutput(output_buffers_[0], confidence_threshold,
                                         frame.width, frame.height, letterbox_infos[frame_idx]);
        }

        results[frame.stream_id] = std::move(detections);
    }

    if (batch_inference_count == 1 || batch_inference_count % 100 == 0) {
        size_t total_detections = 0;
        for (const auto& [id, dets] : results) {
            total_detections += dets.size();
        }
        LogInfo("RunBatchInference: found " + std::to_string(total_detections) + " total detections");
    }

    return results;
}

std::vector<Detection> HailoInference::ParseNmsOutput(
    const std::vector<uint8_t>& output_data,
    float confidence_threshold,
    int frame_width,
    int frame_height,
    const LetterboxInfo& letterbox) {

    std::vector<Detection> detections;

    if (!is_nms_output_) {
        LogWarning("Model doesn't have NMS output");
        return detections;
    }

    const float* data = reinterpret_cast<const float*>(output_data.data());
    size_t num_floats = output_data.size() / sizeof(float);

    // Calculate actual params per detection slot from output size
    // num_floats = num_classes * max_bboxes_per_class * params_per_det
    const int total_slots = num_classes_ * max_bboxes_per_class_;
    const int actual_det_params = (total_slots > 0) ? (num_floats / total_slots) : 0;

    // Expected params for pose model: 5 (bbox+score) + num_keypoints*3
    const int keypoint_params = (task_ == "pose") ? num_keypoints_ * 3 : 0;
    const int expected_det_params = 5 + keypoint_params;

    // Debug: print structure info
    static int debug_count = 0;
    if (debug_count < 3) {
        std::ostringstream oss;
        oss << "NMS Parse: num_floats=" << num_floats
            << ", total_slots=" << total_slots
            << ", actual_params_per_det=" << actual_det_params
            << ", expected=" << expected_det_params;
        LogInfo(oss.str());

        // Print first detection slot to analyze format
        if (actual_det_params > 0 && num_floats >= static_cast<size_t>(actual_det_params)) {
            std::ostringstream det_oss;
            det_oss << "First slot [0.." << actual_det_params-1 << "]: ";
            for (int i = 0; i < actual_det_params && i < 30; ++i) {
                det_oss << data[i] << " ";
            }
            LogInfo(det_oss.str());
        }
        ++debug_count;
    }

    // Use actual_det_params if it differs from expected (model-specific format)
    const int det_params = (actual_det_params > 0 && actual_det_params != expected_det_params)
                           ? actual_det_params : expected_det_params;

    // For Hailo NMS output: iterate through all detection slots
    // Format: [class0_det0, class0_det1, ..., class1_det0, ...]
    for (int cls = 0; cls < num_classes_; ++cls) {
        for (int i = 0; i < max_bboxes_per_class_; ++i) {
            size_t det_offset = (cls * max_bboxes_per_class_ + i) * det_params;
            if (det_offset + 5 > num_floats) break;

            // Try standard format: [y_min, x_min, y_max, x_max, score, ...]
            float y_min = data[det_offset + 0];
            float x_min = data[det_offset + 1];
            float y_max = data[det_offset + 2];
            float x_max = data[det_offset + 3];
            float score = data[det_offset + 4];

            if (score < confidence_threshold) continue;

            // Convert normalized coords (0-1) to model input pixel coords
            float x1_model = x_min * input_width_;
            float y1_model = y_min * input_height_;
            float x2_model = x_max * input_width_;
            float y2_model = y_max * input_height_;

            // Remove letterbox padding and scale back to original frame
            // Formula: original_coord = (model_coord - padding) / scale
            float x1_orig = (x1_model - letterbox.pad_x) / letterbox.scale;
            float y1_orig = (y1_model - letterbox.pad_y) / letterbox.scale;
            float x2_orig = (x2_model - letterbox.pad_x) / letterbox.scale;
            float y2_orig = (y2_model - letterbox.pad_y) / letterbox.scale;

            Detection det;
            det.class_id = cls;

            // Use labels_ if available, otherwise fallback to COCO labels
            if (!labels_.empty() && cls < static_cast<int>(labels_.size())) {
                det.class_name = labels_[cls];
            } else if (cls < NUM_COCO_LABELS) {
                det.class_name = COCO_LABELS[cls];
            } else {
                det.class_name = "object";
            }
            det.confidence = score;

            det.bbox.x = static_cast<int>(x1_orig);
            det.bbox.y = static_cast<int>(y1_orig);
            det.bbox.width = static_cast<int>(x2_orig - x1_orig);
            det.bbox.height = static_cast<int>(y2_orig - y1_orig);

            // Clamp to frame bounds
            det.bbox.x = std::max(0, det.bbox.x);
            det.bbox.y = std::max(0, det.bbox.y);
            det.bbox.width = std::min(det.bbox.width, frame_width - det.bbox.x);
            det.bbox.height = std::min(det.bbox.height, frame_height - det.bbox.y);

            // Parse keypoints for pose model
            if (task_ == "pose" && num_keypoints_ > 0) {
                for (int k = 0; k < num_keypoints_; ++k) {
                    size_t kp_offset = det_offset + 5 + k * 3;
                    if (kp_offset + 3 > num_floats) break;

                    float kp_x = data[kp_offset + 0];
                    float kp_y = data[kp_offset + 1];
                    float kp_conf = data[kp_offset + 2];

                    // Convert to model pixel coords then to original frame
                    float kp_x_model = kp_x * input_width_;
                    float kp_y_model = kp_y * input_height_;
                    float kp_x_orig = (kp_x_model - letterbox.pad_x) / letterbox.scale;
                    float kp_y_orig = (kp_y_model - letterbox.pad_y) / letterbox.scale;

                    // Normalize to 0-1 in original frame
                    Keypoint kp;
                    kp.x = kp_x_orig / frame_width;
                    kp.y = kp_y_orig / frame_height;
                    kp.visible = kp_conf;
                    det.keypoints.push_back(kp);
                }
            }

            if (det.bbox.width > 0 && det.bbox.height > 0) {
                // Debug: log first few detections with coordinates
                static int det_log_count = 0;
                if (det_log_count < 5) {
                    std::ostringstream oss;
                    oss << "Detection[" << det_log_count << "]: class_id=" << det.class_id
                        << " (" << det.class_name << ") conf=" << det.confidence
                        << " bbox=(" << det.bbox.x << "," << det.bbox.y
                        << "," << det.bbox.width << "," << det.bbox.height << ")"
                        << " frame=" << frame_width << "x" << frame_height
                        << " (labels_size=" << labels_.size() << ")";
                    LogInfo(oss.str());
                    ++det_log_count;
                }
                detections.push_back(det);
            }
        }
    }

    return detections;
}

// ============================================================================
// Raw YOLO Output Parsing (for multi-output models like best12.hef)
// ============================================================================

std::vector<int> HailoInference::ApplyNMS(
    const std::vector<std::array<float, 4>>& boxes,
    const std::vector<float>& scores,
    float iou_threshold) {

    // Sort by score (descending)
    std::vector<int> indices(scores.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), [&scores](int a, int b) {
        return scores[a] > scores[b];
    });

    std::vector<int> keep;
    std::vector<bool> suppressed(scores.size(), false);

    for (int idx : indices) {
        if (suppressed[idx]) continue;
        keep.push_back(idx);

        const auto& box_i = boxes[idx];
        float area_i = (box_i[2] - box_i[0]) * (box_i[3] - box_i[1]);

        for (size_t j = 0; j < indices.size(); ++j) {
            int jdx = indices[j];
            if (suppressed[jdx] || jdx == idx) continue;

            const auto& box_j = boxes[jdx];

            // Calculate IoU
            float x1 = std::max(box_i[0], box_j[0]);
            float y1 = std::max(box_i[1], box_j[1]);
            float x2 = std::min(box_i[2], box_j[2]);
            float y2 = std::min(box_i[3], box_j[3]);

            float inter_w = std::max(0.0f, x2 - x1);
            float inter_h = std::max(0.0f, y2 - y1);
            float inter_area = inter_w * inter_h;

            float area_j = (box_j[2] - box_j[0]) * (box_j[3] - box_j[1]);
            float union_area = area_i + area_j - inter_area;
            float iou = (union_area > 0) ? inter_area / union_area : 0.0f;

            if (iou > iou_threshold) {
                suppressed[jdx] = true;
            }
        }
    }

    return keep;
}

std::vector<Detection> HailoInference::ParseRawYoloOutput(
    const std::vector<std::vector<uint8_t>>& output_buffers,
    float confidence_threshold,
    float iou_threshold,
    int frame_width,
    int frame_height,
    const LetterboxInfo& letterbox) {

    std::vector<Detection> detections;

    if (output_buffers.empty()) {
        return detections;
    }

    // Model parameters - use SetModelConfig values or defaults
    const int model_num_keypoints = (num_keypoints_ > 0) ? num_keypoints_ : 4;
    const int reg_max = 16;  // DFL bins

    // Debug output structure
    static int debug_count = 0;
    if (debug_count < 3) {
        std::ostringstream oss;
        oss << "RawYOLO Parse: " << output_buffers.size() << " outputs, ";
        oss << "keypoints=" << model_num_keypoints;
        LogInfo(oss.str());

        for (size_t i = 0; i < output_buffers.size(); ++i) {
            size_t num_floats = output_buffers[i].size() / sizeof(float);
            LogInfo("  Output[" + std::to_string(i) + "]: " + std::to_string(num_floats) + " floats (" +
                    output_vstreams_[i].name() + ")");
        }
        ++debug_count;
    }

    // NEW 9-output model structure (multi-scale with full detection heads):
    // P3 (120×120, stride 8):
    //   - conv43: 120×120×64 = 921600 floats (DFL)
    //   - conv44: 120×120×13 = 187200 floats (class)
    //   - conv45: 120×120×12 = 172800 floats (keypoints)
    // P4 (60×60, stride 16):
    //   - conv57: 60×60×64 = 230400 floats (DFL)
    //   - conv58: 60×60×13 = 46800 floats (class)
    //   - conv59: 60×60×12 = 43200 floats (keypoints)
    // P5 (30×30, stride 32):
    //   - conv70: 30×30×64 = 57600 floats (DFL)
    //   - conv71: 30×30×13 = 11700 floats (class)
    //   - conv72: 30×30×12 = 10800 floats (keypoints)

    // Scale configuration
    struct ScaleInfo {
        int grid_h;
        int grid_w;
        int stride;
        int dfl_idx;      // Index of DFL output (64 channels)
        int class_idx;    // Index of class output (13 channels)
        int kp_idx;       // Index of keypoint output (12 channels)
        int num_classes;  // Number of class channels
    };

    std::vector<ScaleInfo> scales;

    // Map outputs by size to scales
    int p3_dfl = -1, p3_class = -1, p3_kp = -1;
    int p4_dfl = -1, p4_class = -1, p4_kp = -1;
    int p5_dfl = -1, p5_class = -1, p5_kp = -1;

    // Dynamic num_classes from labels (fallback to 13 for backward compatibility)
    const int num_classes = labels_.empty() ? 13 : static_cast<int>(labels_.size());
    const int num_kp_channels = (num_keypoints_ > 0) ? num_keypoints_ * 3 : 12;

    // Map outputs by tensor NAME (not size - size can collide when num_classes=16)
    // conv43/44/45 = P3 (DFL/Class/KP)
    // conv57/58/59 = P4 (DFL/Class/KP)
    // conv70/71/72 = P5 (DFL/Class/KP)
    for (size_t i = 0; i < output_buffers.size(); ++i) {
        std::string name = output_vstreams_[i].name();

        // P3 outputs
        if (name.find("conv43") != std::string::npos) p3_dfl = i;
        else if (name.find("conv44") != std::string::npos) p3_class = i;
        else if (name.find("conv45") != std::string::npos) p3_kp = i;
        // P4 outputs
        else if (name.find("conv57") != std::string::npos) p4_dfl = i;
        else if (name.find("conv58") != std::string::npos) p4_class = i;
        else if (name.find("conv59") != std::string::npos) p4_kp = i;
        // P5 outputs
        else if (name.find("conv70") != std::string::npos) p5_dfl = i;
        else if (name.find("conv71") != std::string::npos) p5_class = i;
        else if (name.find("conv72") != std::string::npos) p5_kp = i;
    }

    // Build scale configs for available scales
    if (p3_dfl >= 0 && p3_class >= 0) {
        scales.push_back({120, 120, 8, p3_dfl, p3_class, p3_kp, num_classes});
    }
    if (p4_dfl >= 0 && p4_class >= 0) {
        scales.push_back({60, 60, 16, p4_dfl, p4_class, p4_kp, num_classes});
    }
    if (p5_dfl >= 0 && p5_class >= 0) {
        scales.push_back({30, 30, 32, p5_dfl, p5_class, p5_kp, num_classes});
    }

    if (debug_count == 1) {
        LogInfo("  num_classes=" + std::to_string(num_classes) + ", num_kp_channels=" + std::to_string(num_kp_channels));
        LogInfo("  P3 outputs: dfl=" + std::to_string(p3_dfl) + " class=" + std::to_string(p3_class) + " kp=" + std::to_string(p3_kp));
        LogInfo("  P4 outputs: dfl=" + std::to_string(p4_dfl) + " class=" + std::to_string(p4_class) + " kp=" + std::to_string(p4_kp));
        LogInfo("  P5 outputs: dfl=" + std::to_string(p5_dfl) + " class=" + std::to_string(p5_class) + " kp=" + std::to_string(p5_kp));
        LogInfo("  Active scales: " + std::to_string(scales.size()));
    }

    if (scales.empty()) {
        LogWarning("No valid detection scales found in outputs");
        return detections;
    }

    // DFL decode helper lambda - sequential layout [L0..L15, T0..T15, R0..R15, B0..B15]
    auto decode_dfl = [reg_max](const float* values, int edge_idx) -> float {
        const float* edge_values = values + edge_idx * reg_max;

        float max_val = edge_values[0];
        for (int i = 1; i < reg_max; ++i) {
            if (edge_values[i] > max_val) max_val = edge_values[i];
        }

        // Softmax-like weighted sum
        float weighted_sum = 0.0f;
        float total_weight = 0.0f;

        for (int i = 0; i < reg_max; ++i) {
            float weight = std::exp((edge_values[i] - max_val) * 5.0f);
            weighted_sum += weight * i;
            total_weight += weight;
        }

        return weighted_sum / total_weight;
    };

    // Collect all detections from all scales
    std::vector<std::array<float, 4>> all_boxes;
    std::vector<float> all_scores;
    std::vector<int> all_class_ids;
    std::vector<std::vector<std::array<float, 3>>> all_keypoints;

    // Process each scale
    for (const auto& scale : scales) {
        const float* dfl_data = reinterpret_cast<const float*>(output_buffers[scale.dfl_idx].data());
        const float* class_data = reinterpret_cast<const float*>(output_buffers[scale.class_idx].data());
        const float* kp_data = (scale.kp_idx >= 0) ?
            reinterpret_cast<const float*>(output_buffers[scale.kp_idx].data()) : nullptr;

        for (int gy = 0; gy < scale.grid_h; ++gy) {
            for (int gx = 0; gx < scale.grid_w; ++gx) {
                int pixel_idx = gy * scale.grid_w + gx;
                int dfl_base = pixel_idx * 64;
                int class_base = pixel_idx * scale.num_classes;

                // Find best class score
                float max_class_score = 0.0f;
                int best_class_id = 0;

                for (int c = 0; c < scale.num_classes; ++c) {
                    float raw_score = class_data[class_base + c];
                    float class_score = raw_score;

                    // Apply sigmoid if values look like logits
                    if (raw_score < -10.0f || raw_score > 10.0f ||
                        raw_score < 0.0f || raw_score > 1.0f) {
                        class_score = 1.0f / (1.0f + std::exp(-raw_score));
                    }

                    if (class_score > max_class_score) {
                        max_class_score = class_score;
                        best_class_id = c;
                    }
                }

                // Skip low confidence
                if (max_class_score < confidence_threshold) continue;

                // Decode bbox using DFL
                float dist_left = decode_dfl(&dfl_data[dfl_base], 0);
                float dist_top = decode_dfl(&dfl_data[dfl_base], 1);
                float dist_right = decode_dfl(&dfl_data[dfl_base], 2);
                float dist_bottom = decode_dfl(&dfl_data[dfl_base], 3);

                // Convert to pixel coordinates
                float anchor_x = (gx + 0.5f) * scale.stride;
                float anchor_y = (gy + 0.5f) * scale.stride;

                float x1 = anchor_x - dist_left * scale.stride;
                float y1 = anchor_y - dist_top * scale.stride;
                float x2 = anchor_x + dist_right * scale.stride;
                float y2 = anchor_y + dist_bottom * scale.stride;

                // Skip invalid boxes
                if (x2 <= 0 || y2 <= 0 || x1 >= input_width_ || y1 >= input_height_) continue;
                if (x2 - x1 <= 0 || y2 - y1 <= 0) continue;

                all_boxes.push_back({x1, y1, x2, y2});
                all_scores.push_back(max_class_score);
                all_class_ids.push_back(best_class_id);

                // Parse keypoints
                std::vector<std::array<float, 3>> kpts;
                if (kp_data != nullptr) {
                    int kp_base = pixel_idx * 12;

                    for (int k = 0; k < model_num_keypoints; ++k) {
                        // Sequential layout: [x0,y0,c0, x1,y1,c1, x2,y2,c2, x3,y3,c3]
                        float kp_x_raw = kp_data[kp_base + k * 3 + 0];
                        float kp_y_raw = kp_data[kp_base + k * 3 + 1];
                        float kp_vis = kp_data[kp_base + k * 3 + 2];

                        // Apply sigmoid to visibility (raw is logit)
                        if (kp_vis < 0.0f || kp_vis > 1.0f) {
                            kp_vis = 1.0f / (1.0f + std::exp(-kp_vis));
                        }

                        // YOLOv8-pose keypoint decoding:
                        // kp = (grid_cell + raw_offset * 2) * stride
                        float kp_x = (gx + kp_x_raw * 2.0f) * scale.stride;
                        float kp_y = (gy + kp_y_raw * 2.0f) * scale.stride;

                        kpts.push_back({kp_x, kp_y, kp_vis});
                    }
                }
                all_keypoints.push_back(kpts);
            }
        }
    }

    if (debug_count == 1) {
        LogInfo("  Pre-NMS detections: " + std::to_string(all_boxes.size()));
    }

    // Apply NMS
    std::vector<int> keep_indices = ApplyNMS(all_boxes, all_scores, iou_threshold);

    // Convert to Detection objects and transform coordinates
    for (int idx : keep_indices) {
        const auto& box = all_boxes[idx];

        // Transform from model coords to original frame coords
        float x1_orig = (box[0] - letterbox.pad_x) / letterbox.scale;
        float y1_orig = (box[1] - letterbox.pad_y) / letterbox.scale;
        float x2_orig = (box[2] - letterbox.pad_x) / letterbox.scale;
        float y2_orig = (box[3] - letterbox.pad_y) / letterbox.scale;

        // Clamp all coordinates to frame bounds FIRST, then calculate width/height
        float x1_clamped = std::max(0.0f, std::min(static_cast<float>(frame_width), x1_orig));
        float y1_clamped = std::max(0.0f, std::min(static_cast<float>(frame_height), y1_orig));
        float x2_clamped = std::max(0.0f, std::min(static_cast<float>(frame_width), x2_orig));
        float y2_clamped = std::max(0.0f, std::min(static_cast<float>(frame_height), y2_orig));

        Detection det;
        det.class_id = all_class_ids[idx];

        // Set class name
        if (!labels_.empty() && det.class_id < static_cast<int>(labels_.size())) {
            det.class_name = labels_[det.class_id];
        } else if (det.class_id < NUM_COCO_LABELS) {
            det.class_name = COCO_LABELS[det.class_id];
        } else {
            det.class_name = "object";
        }

        det.confidence = all_scores[idx];
        det.bbox.x = static_cast<int>(x1_clamped);
        det.bbox.y = static_cast<int>(y1_clamped);
        det.bbox.width = static_cast<int>(x2_clamped - x1_clamped);
        det.bbox.height = static_cast<int>(y2_clamped - y1_clamped);

        // Transform keypoints from model coords to original frame coords
        if (!all_keypoints[idx].empty()) {
            for (const auto& kp : all_keypoints[idx]) {
                // kp[0], kp[1] are in model input pixel coords (0-960)
                // Transform: remove letterbox padding, then scale to original frame
                float kp_x_orig = (kp[0] - letterbox.pad_x) / letterbox.scale;
                float kp_y_orig = (kp[1] - letterbox.pad_y) / letterbox.scale;

                // Clip to frame bounds
                kp_x_orig = std::max(0.0f, std::min(kp_x_orig, static_cast<float>(frame_width - 1)));
                kp_y_orig = std::max(0.0f, std::min(kp_y_orig, static_cast<float>(frame_height - 1)));

                Keypoint keypoint;
                keypoint.x = kp_x_orig / frame_width;   // Normalize to 0-1
                keypoint.y = kp_y_orig / frame_height;  // Normalize to 0-1
                keypoint.visible = kp[2];
                det.keypoints.push_back(keypoint);
            }
        }

        if (det.bbox.width > 0 && det.bbox.height > 0) {
            // Debug: log General class OR first 3 detections
            static int det_log_count = 0;
            static int general_log_count = 0;
            bool should_log = (det_log_count < 3) || (det.class_name == "General" && general_log_count < 3);
            if (should_log) {
                LogInfo("  Det: class=" + det.class_name + " conf=" + std::to_string(det.confidence));
                LogInfo("    model_box: x1=" + std::to_string(box[0]) + " y1=" + std::to_string(box[1]) +
                        " x2=" + std::to_string(box[2]) + " y2=" + std::to_string(box[3]));
                LogInfo("    restored: x1=" + std::to_string(x1_orig) + " y1=" + std::to_string(y1_orig) +
                        " x2=" + std::to_string(x2_orig) + " y2=" + std::to_string(y2_orig));
                LogInfo("    clamped: x1=" + std::to_string(x1_clamped) + " y1=" + std::to_string(y1_clamped) +
                        " x2=" + std::to_string(x2_clamped) + " y2=" + std::to_string(y2_clamped));
                LogInfo("    final_bbox: x=" + std::to_string(det.bbox.x) + " y=" + std::to_string(det.bbox.y) +
                        " w=" + std::to_string(det.bbox.width) + " h=" + std::to_string(det.bbox.height));
                ++det_log_count;
                if (det.class_name == "General") ++general_log_count;
            }
            detections.push_back(det);
        }
    }

    return detections;
}

void HailoInference::SetModelConfig(const std::string& task, int num_keypoints,
                                     const std::vector<std::string>& labels) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    task_ = task;
    num_keypoints_ = num_keypoints;
    labels_ = labels;

    // Note: num_classes_ is set from HEF NMS output info during Initialize()
    // labels_ is only used for class name mapping, not for limiting detection classes

    LogInfo("HailoInference: task=" + task_ + ", keypoints=" +
            std::to_string(num_keypoints_) + ", labels=" +
            std::to_string(labels_.size()) + ", nms_classes=" +
            std::to_string(num_classes_));
}

std::shared_ptr<BatchInferenceManager> HailoInference::GetBatchManager(int batch_timeout_ms) {
    // Only create batch manager for batch > 1
    if (batch_size_ <= 1) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(batch_manager_mutex_);

    // Create on first call
    if (!batch_manager_) {
        // We need a shared_ptr to this, but we're not managed by shared_ptr ourselves
        // Get the existing shared instance from the static cache
        auto self = instances_.find(hef_path_);
        if (self != instances_.end()) {
            batch_manager_ = std::make_shared<BatchInferenceManager>(
                self->second, batch_timeout_ms);
            batch_manager_->Start();
            LogInfo("Created BatchInferenceManager for " + hef_path_ +
                    " with batch=" + std::to_string(batch_size_));
        }
    }

    return batch_manager_;
}

}  // namespace stream_daemon
