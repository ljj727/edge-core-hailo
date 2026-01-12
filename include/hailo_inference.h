#ifndef STREAM_DAEMON_HAILO_INFERENCE_H_
#define STREAM_DAEMON_HAILO_INFERENCE_H_

#include "common.h"
#include <hailo/hailort.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

// Forward declaration
namespace stream_daemon {
class BatchInferenceManager;
}

namespace stream_daemon {

/**
 * @brief Wrapper for HailoRT inference with NMS output parsing
 *
 * Uses HailoRT VStreams API for inference and properly parses
 * NMS detection output from YOLOv8 models.
 *
 * Designed for multi-stream support:
 * - VDevice is shared across all model instances
 * - Hailo scheduler handles concurrent inference efficiently
 * - Thread-safe for parallel camera processing
 */
class HailoInference {
public:
    /**
     * @brief Get or create inference instance for a model
     *
     * Uses shared VDevice for efficient multi-stream processing.
     * Same HEF path returns the same instance (cached).
     *
     * @param hef_path Path to the HEF model file
     * @return Shared HailoInference instance or error
     */
    [[nodiscard]] static Result<std::shared_ptr<HailoInference>> GetInstance(
        const std::string& hef_path);

    /**
     * @brief Release instance for a model
     */
    static void ReleaseInstance(const std::string& hef_path);

    /**
     * @brief Shutdown all instances and release VDevice
     */
    static void Shutdown();

    ~HailoInference();

    // Non-copyable
    HailoInference(const HailoInference&) = delete;
    HailoInference& operator=(const HailoInference&) = delete;

    /**
     * @brief Frame data for batch inference
     */
    struct FrameInput {
        const uint8_t* rgb_data;
        int width;
        int height;
        std::string stream_id;  // To map results back
    };

    /**
     * @brief Run inference on RGB frame and get detections
     * @param rgb_data RGB pixel data (width * height * 3 bytes)
     * @param width Frame width
     * @param height Frame height
     * @param confidence_threshold Minimum confidence for detections
     * @return Vector of detected objects
     */
    [[nodiscard]] std::vector<Detection> RunInference(
        const uint8_t* rgb_data,
        int width,
        int height,
        float confidence_threshold = 0.25f);

    /**
     * @brief Run batch inference on multiple frames
     * @param frames Vector of frame inputs (up to batch_size)
     * @param confidence_threshold Minimum confidence for detections
     * @return Map of stream_id to detections
     */
    [[nodiscard]] std::unordered_map<std::string, std::vector<Detection>> RunBatchInference(
        const std::vector<FrameInput>& frames,
        float confidence_threshold = 0.25f);

    /**
     * @brief Get model batch size
     */
    int GetBatchSize() const { return batch_size_; }

    /**
     * @brief Get model input dimensions
     */
    int GetInputWidth() const { return input_width_; }
    int GetInputHeight() const { return input_height_; }

    /**
     * @brief Check if inference is ready
     */
    bool IsReady() const { return is_ready_; }

    /**
     * @brief Get or create BatchInferenceManager for this model
     * Returns nullptr if batch_size == 1
     * @param batch_timeout_ms Timeout for batch collection
     */
    std::shared_ptr<BatchInferenceManager> GetBatchManager(int batch_timeout_ms = 50);

    /**
     * @brief Set model configuration for proper output parsing
     * @param task "det" for detection, "pose" for pose estimation
     * @param num_keypoints Number of keypoints (for pose models)
     * @param labels Class labels
     */
    void SetModelConfig(const std::string& task, int num_keypoints,
                        const std::vector<std::string>& labels);

private:
    HailoInference() = default;

    // Letterbox info for coordinate transformation
    struct LetterboxInfo {
        float scale{1.0f};   // Scale factor applied
        int pad_x{0};        // Padding on left (and right)
        int pad_y{0};        // Padding on top (and bottom)
        int new_w{0};        // Resized width before padding
        int new_h{0};        // Resized height before padding
    };

    VoidResult Initialize(const std::string& hef_path);
    std::vector<Detection> ParseNmsOutput(const std::vector<uint8_t>& output_data,
                                           float confidence_threshold,
                                           int frame_width,
                                           int frame_height,
                                           const LetterboxInfo& letterbox);

    // Raw YOLO output parsing (for non-NMS models like best12.hef)
    std::vector<Detection> ParseRawYoloOutput(
        const std::vector<std::vector<uint8_t>>& output_buffers,
        float confidence_threshold,
        float iou_threshold,
        int frame_width,
        int frame_height,
        const LetterboxInfo& letterbox);

    // NMS helper
    static std::vector<int> ApplyNMS(
        const std::vector<std::array<float, 4>>& boxes,
        const std::vector<float>& scores,
        float iou_threshold);

    // Letterbox resize helper
    static LetterboxInfo LetterboxResize(const uint8_t* src, int src_w, int src_h,
                                          uint8_t* dst, int dst_w, int dst_h,
                                          uint8_t pad_value = 114);

    // Static members for VDevice sharing (multi-stream efficiency)
    static std::shared_ptr<hailort::VDevice> shared_vdevice_;
    static std::unordered_map<std::string, std::shared_ptr<HailoInference>> instances_;
    static std::mutex static_mutex_;

    // Per-instance HailoRT objects
    std::shared_ptr<hailort::ConfiguredNetworkGroup> network_group_;
    std::vector<hailort::InputVStream> input_vstreams_;
    std::vector<hailort::OutputVStream> output_vstreams_;
    std::string hef_path_;

    // Model info
    int input_width_{640};
    int input_height_{640};
    int batch_size_{1};  // Batch size from HEF model
    size_t input_frame_size_{0};

    // NMS output info
    int num_classes_{80};
    int max_bboxes_per_class_{100};
    bool is_nms_output_{false};
    bool is_raw_yolo_output_{false};  // For multi-output models without NMS (e.g., best12.hef)

    // Model config (set via SetModelConfig)
    std::string task_{"det"};           // "det" or "pose"
    int num_keypoints_{0};              // Number of keypoints for pose model
    std::vector<std::string> labels_;   // Class labels

    // Input/Output buffers (per-instance for thread safety)
    std::vector<uint8_t> input_buffer_;
    std::vector<std::vector<uint8_t>> output_buffers_;  // One buffer per output vstream
    std::vector<size_t> output_frame_sizes_;            // Size of each output

    // State
    bool is_ready_{false};
    mutable std::mutex inference_mutex_;

    // Batch manager (created on demand for batch > 1)
    std::shared_ptr<BatchInferenceManager> batch_manager_;
    std::mutex batch_manager_mutex_;
};

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_HAILO_INFERENCE_H_
