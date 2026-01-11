#ifndef STREAM_DAEMON_STREAM_MANAGER_H_
#define STREAM_DAEMON_STREAM_MANAGER_H_

#include "common.h"
#include "nats_publisher.h"
#include "stream_processor.h"

#include <glib.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace stream_daemon {

/**
 * @brief Manages multiple stream processors
 *
 * Handles lifecycle of all streams, provides thread-safe access,
 * and manages the GLib main loop for GStreamer event processing.
 */
class StreamManager {
public:
    /**
     * @brief Factory method with error handling
     */
    [[nodiscard]] static Result<std::unique_ptr<StreamManager>> Create(
        std::string_view nats_url = kDefaultNatsUrl);

    // Non-copyable, non-movable
    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;
    StreamManager(StreamManager&&) = delete;
    StreamManager& operator=(StreamManager&&) = delete;

    ~StreamManager();

    /**
     * @brief Start the manager (GLib main loop)
     */
    void Start();

    /**
     * @brief Stop the manager and all streams
     */
    void Stop();

    /**
     * @brief Check if running
     */
    [[nodiscard]] bool IsRunning() const noexcept { return running_.load(); }

    /**
     * @brief Add a new stream
     */
    [[nodiscard]] VoidResult AddStream(const StreamInfo& info);

    /**
     * @brief Remove a stream
     */
    [[nodiscard]] VoidResult RemoveStream(std::string_view stream_id);

    /**
     * @brief Update an existing stream
     */
    [[nodiscard]] VoidResult UpdateStream(const StreamInfo& info);

    /**
     * @brief Clear inference from a stream (keep camera running in video-only mode)
     */
    [[nodiscard]] VoidResult ClearStreamInference(std::string_view stream_id);

    /**
     * @brief Update event settings for a stream
     * @return 터미널 이벤트 ID 목록
     */
    [[nodiscard]] Result<std::vector<std::string>> UpdateEventSettings(
        std::string_view stream_id,
        const std::string& settings_json);

    /**
     * @brief Clear event settings for a stream
     */
    [[nodiscard]] VoidResult ClearEventSettings(std::string_view stream_id);

    /**
     * @brief Get status of a specific stream
     */
    [[nodiscard]] std::optional<StreamStatus> GetStreamStatus(std::string_view stream_id) const;

    /**
     * @brief Get status of all streams
     */
    [[nodiscard]] std::vector<StreamStatus> GetAllStreamStatus() const;

    /**
     * @brief Get number of active streams
     */
    [[nodiscard]] size_t GetStreamCount() const;

    /**
     * @brief Check if stream exists
     */
    [[nodiscard]] bool HasStream(std::string_view stream_id) const;

    /**
     * @brief Get snapshot from a stream
     */
    [[nodiscard]] std::optional<std::vector<uint8_t>> GetSnapshot(std::string_view stream_id) const;

    /**
     * @brief Get NATS publisher (for direct access if needed)
     */
    [[nodiscard]] std::shared_ptr<NatsPublisher> GetNatsPublisher() const noexcept {
        return nats_publisher_;
    }

    // ========== NATS Control ==========

    /**
     * @brief Connect to NATS (uses current URL)
     */
    [[nodiscard]] VoidResult ConnectNats();

    /**
     * @brief Connect to NATS with new URL
     */
    [[nodiscard]] VoidResult ConnectNats(std::string_view new_url);

    /**
     * @brief Disconnect from NATS
     */
    void DisconnectNats();

    /**
     * @brief Force NATS reconnection
     */
    [[nodiscard]] VoidResult ReconnectNats();

    /**
     * @brief Check if NATS is connected
     */
    [[nodiscard]] bool IsNatsConnected() const noexcept;

    /**
     * @brief Get NATS connection state
     */
    [[nodiscard]] NatsState GetNatsState() const noexcept;

    /**
     * @brief Get current NATS URL
     */
    [[nodiscard]] std::string GetNatsUrl() const;

    /**
     * @brief Get NATS statistics
     */
    [[nodiscard]] NatsStats GetNatsStats() const;

    // Global callbacks for all streams
    void SetGlobalDetectionCallback(DetectionCallback callback);
    void SetGlobalStateChangeCallback(StateChangeCallback callback);
    void SetGlobalErrorCallback(ErrorCallback callback);

private:
    explicit StreamManager(std::shared_ptr<NatsPublisher> nats_publisher);

    /**
     * @brief Main loop thread function
     */
    void MainLoopThread();

    /**
     * @brief Apply global callbacks to a stream processor
     */
    void ApplyCallbacks(StreamProcessor* processor);

    // Stream storage
    std::map<std::string, std::unique_ptr<StreamProcessor>, std::less<>> streams_;
    mutable std::mutex streams_mutex_;

    // NATS publisher (shared among all streams)
    std::shared_ptr<NatsPublisher> nats_publisher_;

    // GLib main loop
    GMainLoop* main_loop_{nullptr};
    GMainContext* main_context_{nullptr};
    std::thread main_loop_thread_;
    std::atomic<bool> running_{false};

    // Global callbacks
    DetectionCallback global_detection_callback_;
    StateChangeCallback global_state_change_callback_;
    ErrorCallback global_error_callback_;
    mutable std::mutex callback_mutex_;
};

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_STREAM_MANAGER_H_
