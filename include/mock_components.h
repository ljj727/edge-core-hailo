#ifndef STREAM_DAEMON_MOCK_COMPONENTS_H_
#define STREAM_DAEMON_MOCK_COMPONENTS_H_

#include "interfaces.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace stream_daemon {
namespace testing {

/**
 * @brief Mock message publisher for testing
 *
 * Records all published messages for verification.
 */
class MockMessagePublisher : public IMessagePublisher {
public:
    MockMessagePublisher() = default;
    ~MockMessagePublisher() override = default;

    [[nodiscard]] VoidResult Connect() override {
        connected_ = true;
        return MakeOk();
    }

    void Disconnect() override {
        connected_ = false;
    }

    [[nodiscard]] bool IsConnected() const noexcept override {
        return connected_.load();
    }

    [[nodiscard]] VoidResult Publish(const DetectionEvent& event) override {
        if (!connected_) {
            return MakeError("Not connected");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        published_events_.push_back(event);
        return MakeOk();
    }

    [[nodiscard]] VoidResult PublishRaw(
        std::string_view subject,
        std::string_view json_data) override {
        if (!connected_) {
            return MakeError("Not connected");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        raw_messages_.emplace_back(std::string(subject), std::string(json_data));
        return MakeOk();
    }

    // Test helpers
    [[nodiscard]] std::vector<DetectionEvent> GetPublishedEvents() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return published_events_;
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> GetRawMessages() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return raw_messages_;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        published_events_.clear();
        raw_messages_.clear();
    }

    [[nodiscard]] size_t GetEventCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return published_events_.size();
    }

private:
    std::atomic<bool> connected_{false};
    mutable std::mutex mutex_;
    std::vector<DetectionEvent> published_events_;
    std::vector<std::pair<std::string, std::string>> raw_messages_;
};

/**
 * @brief Mock stream processor for testing
 *
 * Simulates stream processing without actual GStreamer.
 */
class MockStreamProcessor : public IStreamProcessor {
public:
    explicit MockStreamProcessor(const StreamInfo& info)
        : stream_id_(info.stream_id)
        , rtsp_url_(info.rtsp_url)
        , config_(info.config) {}

    ~MockStreamProcessor() override {
        Stop();
    }

    [[nodiscard]] VoidResult Start() override {
        if (should_fail_start_) {
            return MakeError(fail_message_);
        }
        state_ = StreamState::kRunning;
        start_time_ = std::chrono::steady_clock::now();
        return MakeOk();
    }

    void Stop() override {
        state_ = StreamState::kStopped;
    }

    [[nodiscard]] VoidResult Update(const StreamInfo& new_info) override {
        rtsp_url_ = new_info.rtsp_url;
        config_ = new_info.config;
        return MakeOk();
    }

    [[nodiscard]] StreamStatus GetStatus() const override {
        StreamStatus status;
        status.stream_id = stream_id_;
        status.rtsp_url = rtsp_url_;
        status.state = state_.load();
        status.frame_count = frame_count_.load();
        status.current_fps = current_fps_.load();
        status.last_error = last_error_;

        if (state_ == StreamState::kRunning) {
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                now - start_time_);
            status.uptime_seconds = static_cast<uint64_t>(duration.count());
        }

        return status;
    }

    [[nodiscard]] std::string_view GetStreamId() const noexcept override {
        return stream_id_;
    }

    [[nodiscard]] StreamState GetState() const noexcept override {
        return state_.load();
    }

    [[nodiscard]] bool IsRunning() const noexcept override {
        return state_.load() == StreamState::kRunning;
    }

    void SetDetectionCallback(DetectionCallback callback) override {
        detection_callback_ = std::move(callback);
    }

    void SetStateChangeCallback(StateChangeCallback callback) override {
        state_change_callback_ = std::move(callback);
    }

    void SetErrorCallback(ErrorCallback callback) override {
        error_callback_ = std::move(callback);
    }

    // Test helpers - simulate events
    void SimulateDetection(const DetectionEvent& event) {
        ++frame_count_;
        if (detection_callback_) {
            detection_callback_(event);
        }
    }

    void SimulateError(std::string_view error) {
        last_error_ = std::string(error);
        state_ = StreamState::kError;
        if (error_callback_) {
            error_callback_(stream_id_, error);
        }
    }

    void SimulateFps(double fps) {
        current_fps_ = fps;
    }

    // Test configuration
    void SetShouldFailStart(bool should_fail, std::string message = "Simulated failure") {
        should_fail_start_ = should_fail;
        fail_message_ = std::move(message);
    }

private:
    std::string stream_id_;
    std::string rtsp_url_;
    StreamConfig config_;

    std::atomic<StreamState> state_{StreamState::kStopped};
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<double> current_fps_{0.0};
    std::string last_error_;

    std::chrono::steady_clock::time_point start_time_;

    DetectionCallback detection_callback_;
    StateChangeCallback state_change_callback_;
    ErrorCallback error_callback_;

    bool should_fail_start_{false};
    std::string fail_message_;
};

/**
 * @brief Mock stream processor factory for testing
 */
class MockStreamProcessorFactory : public IStreamProcessorFactory {
public:
    [[nodiscard]] Result<std::unique_ptr<IStreamProcessor>> Create(
        const StreamInfo& info,
        [[maybe_unused]] std::shared_ptr<IMessagePublisher> publisher) override {

        auto processor = std::make_unique<MockStreamProcessor>(info);

        // Store weak reference for test access
        {
            std::lock_guard<std::mutex> lock(mutex_);
            created_processors_.push_back(processor.get());
        }

        return processor;
    }

    // Get all created processors (for test verification)
    [[nodiscard]] std::vector<MockStreamProcessor*> GetCreatedProcessors() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return created_processors_;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        created_processors_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<MockStreamProcessor*> created_processors_;
};

}  // namespace testing
}  // namespace stream_daemon

#endif  // STREAM_DAEMON_MOCK_COMPONENTS_H_
