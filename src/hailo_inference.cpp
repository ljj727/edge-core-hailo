#include "hailo_inference.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <numeric>
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
        LogInfo("Model input: " + std::to_string(input_width_) + "x" +
                std::to_string(input_height_));
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

    // Parse output - use first buffer for NMS models, or combine for multi-output
    std::vector<Detection> detections;
    if (is_nms_output_ && !output_buffers_.empty()) {
        // Single NMS output - parse first vstream
        detections = ParseNmsOutput(output_buffers_[0], confidence_threshold,
                                     width, height, letterbox_info);
    } else if (!output_buffers_.empty()) {
        // Multi-output model (like best12.hef) - need raw YOLO parsing
        // For now, just log that we received all outputs successfully
        if (inference_count == 1) {
            LogInfo("Non-NMS model: received " + std::to_string(output_vstreams_.size()) +
                   " outputs, total bytes = " + std::to_string(
                       std::accumulate(output_frame_sizes_.begin(), output_frame_sizes_.end(), 0UL)));
        }
        // TODO: Implement raw YOLO output parsing for multi-head models
        // For now, return empty detections but at least don't timeout
    }

    if (inference_count == 1 || (inference_count % 100 == 0 && !detections.empty())) {
        LogInfo("RunInference: found " + std::to_string(detections.size()) + " detections");
    }

    return detections;
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
                detections.push_back(det);
            }
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

}  // namespace stream_daemon
