#include "batch_inference_manager.h"
#include "common.h"
#include <algorithm>
#include <cstring>

namespace stream_daemon {

BatchInferenceManager::BatchInferenceManager(
    std::shared_ptr<HailoInference> inference,
    int batch_timeout_ms)
    : inference_(std::move(inference)),
      batch_timeout_ms_(batch_timeout_ms) {
    LogInfo("BatchInferenceManager created with batch_size=" +
            std::to_string(GetBatchSize()) +
            ", timeout=" + std::to_string(batch_timeout_ms_) + "ms");
}

BatchInferenceManager::~BatchInferenceManager() {
    Stop();
}

void BatchInferenceManager::Start() {
    if (running_.exchange(true)) {
        return;  // Already running
    }

    worker_thread_ = std::thread(&BatchInferenceManager::WorkerLoop, this);
    LogInfo("BatchInferenceManager worker started");
}

void BatchInferenceManager::Stop() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    // Wake up worker thread
    queue_cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    LogInfo("BatchInferenceManager worker stopped");
}

void BatchInferenceManager::RegisterStream(const std::string& stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    registered_streams_.insert(stream_id);
    LogInfo("BatchInferenceManager: registered stream " + stream_id +
            " (total: " + std::to_string(registered_streams_.size()) + ")");
}

void BatchInferenceManager::UnregisterStream(const std::string& stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    registered_streams_.erase(stream_id);
    LogInfo("BatchInferenceManager: unregistered stream " + stream_id +
            " (remaining: " + std::to_string(registered_streams_.size()) + ")");
}

size_t BatchInferenceManager::GetStreamCount() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    return registered_streams_.size();
}

int BatchInferenceManager::GetBatchSize() const {
    return inference_ ? inference_->GetBatchSize() : 1;
}

void BatchInferenceManager::SubmitFrame(
    const std::string& stream_id,
    const uint8_t* rgb_data,
    int width,
    int height,
    ResultCallback callback) {

    if (!running_) {
        LogWarning("BatchInferenceManager: not running, dropping frame from " + stream_id);
        return;
    }

    // Create pending frame with copy of data
    PendingFrame frame;
    frame.stream_id = stream_id;
    frame.width = width;
    frame.height = height;
    frame.callback = std::move(callback);
    frame.submit_time = std::chrono::steady_clock::now();

    // Copy RGB data
    size_t data_size = static_cast<size_t>(width * height * 3);
    frame.rgb_data.resize(data_size);
    std::memcpy(frame.rgb_data.data(), rgb_data, data_size);

    // Add to queue
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_frames_.push(std::move(frame));
    }
    queue_cv_.notify_one();
}

void BatchInferenceManager::WorkerLoop() {
    const int batch_size = GetBatchSize();
    std::vector<PendingFrame> batch_frames;
    batch_frames.reserve(batch_size);

    while (running_) {
        batch_frames.clear();

        // Wait for frames or timeout
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            // Wait until we have at least one frame or shutdown
            queue_cv_.wait(lock, [this] {
                return !pending_frames_.empty() || !running_;
            });

            if (!running_ && pending_frames_.empty()) {
                break;
            }

            // Get first frame
            if (!pending_frames_.empty()) {
                batch_frames.push_back(std::move(pending_frames_.front()));
                pending_frames_.pop();
            }

            // Try to collect more frames for batch (up to batch_size)
            if (batch_frames.size() < static_cast<size_t>(batch_size)) {
                auto deadline = batch_frames[0].submit_time +
                               std::chrono::milliseconds(batch_timeout_ms_);

                // Wait for more frames or timeout
                while (batch_frames.size() < static_cast<size_t>(batch_size) && running_) {
                    auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        break;  // Timeout reached
                    }

                    // Wait with timeout
                    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - now);

                    bool got_frame = queue_cv_.wait_for(lock, remaining, [this] {
                        return !pending_frames_.empty() || !running_;
                    });

                    if (!running_) break;

                    if (got_frame && !pending_frames_.empty()) {
                        batch_frames.push_back(std::move(pending_frames_.front()));
                        pending_frames_.pop();
                    }
                }
            }
        }

        // Process the batch
        if (!batch_frames.empty()) {
            ProcessBatch(batch_frames);
        }
    }

    // Process any remaining frames on shutdown
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!pending_frames_.empty()) {
            batch_frames.push_back(std::move(pending_frames_.front()));
            pending_frames_.pop();

            if (batch_frames.size() >= static_cast<size_t>(batch_size)) {
                ProcessBatch(batch_frames);
                batch_frames.clear();
            }
        }
        if (!batch_frames.empty()) {
            ProcessBatch(batch_frames);
        }
    }
}

void BatchInferenceManager::ProcessBatch(std::vector<PendingFrame>& frames) {
    if (frames.empty() || !inference_) {
        return;
    }

    // Prepare frame inputs for batch inference
    std::vector<HailoInference::FrameInput> inputs;
    inputs.reserve(frames.size());

    for (const auto& frame : frames) {
        HailoInference::FrameInput input;
        input.rgb_data = frame.rgb_data.data();
        input.width = frame.width;
        input.height = frame.height;
        input.stream_id = frame.stream_id;
        inputs.push_back(input);
    }

    // Run batch inference
    auto results = inference_->RunBatchInference(inputs, confidence_threshold_);

    // Deliver results via callbacks
    for (auto& frame : frames) {
        auto it = results.find(frame.stream_id);
        if (it != results.end()) {
            if (frame.callback) {
                frame.callback(frame.stream_id, std::move(it->second));
            }
        } else {
            // No results for this stream (shouldn't happen normally)
            if (frame.callback) {
                frame.callback(frame.stream_id, {});
            }
        }
    }

    // Log batch info occasionally
    static int batch_count = 0;
    if (++batch_count % 100 == 0) {
        LogDebug("BatchInferenceManager: processed " + std::to_string(batch_count) +
                 " batches (last batch size: " + std::to_string(frames.size()) + ")");
    }
}

}  // namespace stream_daemon
