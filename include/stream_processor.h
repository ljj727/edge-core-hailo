#ifndef STREAM_DAEMON_STREAM_PROCESSOR_H_
#define STREAM_DAEMON_STREAM_PROCESSOR_H_

#include "common.h"
#include "nats_publisher.h"
#include "hailo_inference.h"
#include "batch_inference_manager.h"
#include "event_compositor.h"

#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace stream_daemon {

/**
 * @brief Individual stream processor using GStreamer + Hailo
 *
 * Manages a single GStreamer pipeline for RTSP stream processing
 * with Hailo NPU inference. Thread-safe and supports automatic reconnection.
 */
class StreamProcessor {
public:
    /**
     * @brief Factory method with error handling
     */
    [[nodiscard]] static Result<std::unique_ptr<StreamProcessor>> Create(
        const StreamInfo& info,
        std::shared_ptr<NatsPublisher> nats_publisher);

    // Non-copyable, non-movable (due to GStreamer callbacks)
    StreamProcessor(const StreamProcessor&) = delete;
    StreamProcessor& operator=(const StreamProcessor&) = delete;
    StreamProcessor(StreamProcessor&&) = delete;
    StreamProcessor& operator=(StreamProcessor&&) = delete;

    ~StreamProcessor();

    /**
     * @brief Start the stream processing pipeline
     */
    [[nodiscard]] VoidResult Start();

    /**
     * @brief Stop the stream processing pipeline
     */
    void Stop();

    /**
     * @brief Update stream configuration (stops and restarts with new config)
     */
    [[nodiscard]] VoidResult Update(const StreamInfo& new_info);

    /**
     * @brief Clear inference model and restart in video-only mode
     */
    [[nodiscard]] VoidResult ClearInference();

    /**
     * @brief Update event settings
     * @return 터미널 이벤트 ID 목록
     */
    [[nodiscard]] Result<std::vector<std::string>> UpdateEventSettings(
        const std::string& settings_json);

    /**
     * @brief Clear event settings
     */
    void ClearEventSettings();

    /**
     * @brief Get current stream status
     */
    [[nodiscard]] StreamStatus GetStatus() const;

    /**
     * @brief Get stream ID
     */
    [[nodiscard]] std::string_view GetStreamId() const noexcept { return stream_id_; }

    /**
     * @brief Get current state
     */
    [[nodiscard]] StreamState GetState() const noexcept { return state_.load(); }

    /**
     * @brief Check if running
     */
    [[nodiscard]] bool IsRunning() const noexcept {
        return state_.load() == StreamState::kRunning;
    }

    /**
     * @brief Get latest snapshot (JPEG)
     */
    [[nodiscard]] std::optional<std::vector<uint8_t>> GetSnapshot() const;

    /**
     * @brief Get model ID
     */
    [[nodiscard]] std::string_view GetModelId() const noexcept { return model_id_; }

    // Callback setters
    void SetDetectionCallback(DetectionCallback callback);
    void SetStateChangeCallback(StateChangeCallback callback);
    void SetErrorCallback(ErrorCallback callback);

private:
    explicit StreamProcessor(
        const StreamInfo& info,
        std::shared_ptr<NatsPublisher> nats_publisher);

    /**
     * @brief Create GStreamer pipeline
     */
    [[nodiscard]] VoidResult CreatePipeline();

    /**
     * @brief Destroy GStreamer pipeline and free resources
     */
    void DestroyPipeline();

    /**
     * @brief Build pipeline string for GStreamer
     */
    [[nodiscard]] std::string BuildPipelineString() const;

    /**
     * @brief Schedule reconnection attempt
     */
    void ScheduleReconnect();

    /**
     * @brief Cancel scheduled reconnection
     */
    void CancelReconnect();

    /**
     * @brief Process detections from Hailo inference
     */
    void ProcessDetections(GstBuffer* buffer);

    /**
     * @brief Update FPS calculation
     */
    void UpdateFps();

    /**
     * @brief Set state and notify callbacks
     */
    void SetState(StreamState new_state);

    /**
     * @brief Set error and notify callbacks
     */
    void SetError(std::string_view error);

    // GStreamer callbacks (static to be compatible with C callbacks)
    static GstFlowReturn OnNewSample(GstElement* sink, gpointer user_data);
    static gboolean OnBusMessage(GstBus* bus, GstMessage* msg, gpointer user_data);
    static GstPadProbeReturn OnHailoProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static gboolean OnReconnectTimeout(gpointer user_data);

    // Stream info
    std::string stream_id_;
    std::string rtsp_url_;
    std::string hef_path_;
    std::string model_id_;
    StreamConfig config_;

    // Model info for inference
    std::string task_;                    // "det" or "pose"
    int num_keypoints_{0};                // Number of keypoints for pose model
    std::vector<std::string> labels_;     // Class labels

    // GStreamer elements
    GstElement* pipeline_{nullptr};
    GstElement* appsink_{nullptr};
    GstBus* bus_{nullptr};
    guint bus_watch_id_{0};
    guint reconnect_source_id_{0};
    gulong hailo_probe_id_{0};

    // NATS publisher (shared)
    std::shared_ptr<NatsPublisher> nats_publisher_;

    // State
    std::atomic<StreamState> state_{StreamState::kStopped};
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<int64_t> last_detection_time_{0};
    std::string last_error_;
    mutable std::mutex error_mutex_;

    // Timing
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_fps_update_;
    uint64_t frames_since_last_update_{0};
    std::atomic<double> current_fps_{0.0};

    // Reconnection
    int reconnect_attempts_{0};
    static constexpr int kMaxReconnectAttempts = 10;

    // Callbacks
    DetectionCallback detection_callback_;
    StateChangeCallback state_change_callback_;
    ErrorCallback error_callback_;
    mutable std::mutex callback_mutex_;

    // Snapshot storage (latest JPEG frame)
    std::vector<uint8_t> last_snapshot_;
    mutable std::mutex snapshot_mutex_;
    int frame_width_{0};
    int frame_height_{0};

    // Hailo detection storage (from probe)
    std::vector<Detection> pending_detections_;
    mutable std::mutex detection_mutex_;

    // HailoRT direct inference (when HEF is specified)
    // Shared instance for efficient multi-stream processing
    std::shared_ptr<HailoInference> hailo_inference_;

    // Batch inference manager (for batch > 1 models)
    std::shared_ptr<BatchInferenceManager> batch_manager_;

    // Helper for batch inference callback
    void OnBatchResult(const std::string& stream_id,
                       std::vector<Detection> detections,
                       const std::vector<uint8_t>& jpeg_data,
                       int width, int height);

    // Event compositor (이벤트 설정 및 감지)
    std::unique_ptr<EventCompositor> event_compositor_;

    // Settings
    bool publish_images_{true};        // NATS로 이미지 포함 발행 여부
    int jpeg_quality_{75};             // JPEG 압축 품질 (1-100)
};

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_STREAM_PROCESSOR_H_
