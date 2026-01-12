#ifndef STREAM_DAEMON_BATCH_INFERENCE_MANAGER_H_
#define STREAM_DAEMON_BATCH_INFERENCE_MANAGER_H_

#include "common.h"
#include "hailo_inference.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stream_daemon {

/**
 * @brief Manages batch inference across multiple streams
 *
 * Collects frames from multiple streams and batches them together
 * for efficient inference when using batch-enabled HEF models.
 *
 * For batch=2 models:
 * - If 1 camera connected: runs with padding (less efficient but works)
 * - If 2 cameras connected: batches both frames for optimal throughput
 */
class BatchInferenceManager {
public:
    using ResultCallback = std::function<void(const std::string& stream_id,
                                               std::vector<Detection> detections)>;

    /**
     * @brief Create manager for a specific HEF model
     * @param inference Shared HailoInference instance
     * @param batch_timeout_ms Max time to wait for batch to fill (default 50ms)
     */
    explicit BatchInferenceManager(
        std::shared_ptr<HailoInference> inference,
        int batch_timeout_ms = 50);

    ~BatchInferenceManager();

    // Non-copyable
    BatchInferenceManager(const BatchInferenceManager&) = delete;
    BatchInferenceManager& operator=(const BatchInferenceManager&) = delete;

    /**
     * @brief Submit a frame for batch inference
     * @param stream_id ID of the stream submitting the frame
     * @param rgb_data RGB pixel data
     * @param width Frame width
     * @param height Frame height
     * @param callback Function to call with results (may be called from worker thread)
     */
    void SubmitFrame(
        const std::string& stream_id,
        const uint8_t* rgb_data,
        int width,
        int height,
        ResultCallback callback);

    /**
     * @brief Register a stream for batch processing
     */
    void RegisterStream(const std::string& stream_id);

    /**
     * @brief Unregister a stream
     */
    void UnregisterStream(const std::string& stream_id);

    /**
     * @brief Get number of registered streams
     */
    size_t GetStreamCount() const;

    /**
     * @brief Get batch size of the model
     */
    int GetBatchSize() const;

    /**
     * @brief Start the batch processing worker
     */
    void Start();

    /**
     * @brief Stop the batch processing worker
     */
    void Stop();

private:
    struct PendingFrame {
        std::string stream_id;
        std::vector<uint8_t> rgb_data;  // Copy of frame data
        int width;
        int height;
        ResultCallback callback;
        std::chrono::steady_clock::time_point submit_time;
    };

    void WorkerLoop();
    void ProcessBatch(std::vector<PendingFrame>& frames);

    std::shared_ptr<HailoInference> inference_;
    int batch_timeout_ms_;
    float confidence_threshold_{0.25f};

    // Pending frames queue
    std::queue<PendingFrame> pending_frames_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Registered streams
    std::unordered_set<std::string> registered_streams_;
    mutable std::mutex streams_mutex_;

    // Worker thread
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
};

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_BATCH_INFERENCE_MANAGER_H_
