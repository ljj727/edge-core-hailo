#ifndef STREAM_DAEMON_NATS_PUBLISHER_H_
#define STREAM_DAEMON_NATS_PUBLISHER_H_

#include "common.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

// Forward declarations for NATS C library
struct __natsConnection;
typedef struct __natsConnection natsConnection;
struct __natsOptions;
typedef struct __natsOptions natsOptions;

namespace stream_daemon {

/**
 * @brief NATS connection state
 */
enum class NatsState {
    kDisconnected,
    kConnecting,
    kConnected,
    kReconnecting
};

[[nodiscard]] constexpr std::string_view NatsStateToString(NatsState state) noexcept {
    switch (state) {
        case NatsState::kDisconnected: return "DISCONNECTED";
        case NatsState::kConnecting:   return "CONNECTING";
        case NatsState::kConnected:    return "CONNECTED";
        case NatsState::kReconnecting: return "RECONNECTING";
    }
    return "UNKNOWN";
}

/**
 * @brief NATS statistics
 */
struct NatsStats {
    uint64_t messages_published{0};
    int64_t last_publish_time{0};
    int32_t reconnect_attempts{0};
    std::string last_error;
};

/**
 * @brief NATS Publisher with auto-reconnect support
 *
 * Thread-safe publisher that sends detection results to NATS server.
 * Supports automatic background reconnection when connection is lost.
 */
class NatsPublisher {
public:
    /**
     * @brief Create publisher (does NOT connect immediately)
     */
    [[nodiscard]] static std::unique_ptr<NatsPublisher> Create(
        std::string_view nats_url = kDefaultNatsUrl);

    /**
     * @brief Create and connect (returns error if connection fails)
     */
    [[nodiscard]] static Result<std::unique_ptr<NatsPublisher>> CreateAndConnect(
        std::string_view nats_url = kDefaultNatsUrl);

    // Non-copyable, movable
    NatsPublisher(const NatsPublisher&) = delete;
    NatsPublisher& operator=(const NatsPublisher&) = delete;
    NatsPublisher(NatsPublisher&&) noexcept;
    NatsPublisher& operator=(NatsPublisher&&) noexcept;

    ~NatsPublisher();

    /**
     * @brief Connect to NATS server
     * @return Success or error message
     */
    [[nodiscard]] VoidResult Connect();

    /**
     * @brief Connect with new URL
     */
    [[nodiscard]] VoidResult Connect(std::string_view new_url);

    /**
     * @brief Disconnect from NATS server
     */
    void Disconnect();

    /**
     * @brief Check if connected
     */
    [[nodiscard]] bool IsConnected() const noexcept;

    /**
     * @brief Get current state
     */
    [[nodiscard]] NatsState GetState() const noexcept { return state_.load(); }

    /**
     * @brief Publish detection event to NATS
     * @param event Detection event data
     * @return Success or error message (fails silently if not connected)
     */
    [[nodiscard]] VoidResult Publish(const DetectionEvent& event);

    /**
     * @brief Publish raw JSON string to subject
     */
    [[nodiscard]] VoidResult PublishRaw(std::string_view subject, std::string_view json_data);

    /**
     * @brief Get NATS server URL
     */
    [[nodiscard]] std::string GetUrl() const;

    /**
     * @brief Set new URL (will reconnect if currently connected)
     */
    void SetUrl(std::string_view new_url);

    /**
     * @brief Get statistics
     */
    [[nodiscard]] NatsStats GetStats() const;

    /**
     * @brief Enable/disable auto-reconnect
     */
    void SetAutoReconnect(bool enabled);

    /**
     * @brief Check if auto-reconnect is enabled
     */
    [[nodiscard]] bool IsAutoReconnectEnabled() const noexcept {
        return auto_reconnect_enabled_.load();
    }

    /**
     * @brief Force reconnection attempt
     */
    [[nodiscard]] VoidResult ForceReconnect();

    /**
     * @brief Start background reconnect thread
     */
    void StartBackgroundReconnect();

    /**
     * @brief Stop background reconnect thread
     */
    void StopBackgroundReconnect();

private:
    explicit NatsPublisher(std::string nats_url);

    /**
     * @brief Internal connect implementation
     */
    [[nodiscard]] VoidResult ConnectInternal();

    /**
     * @brief Serialize DetectionEvent to JSON string
     */
    [[nodiscard]] std::string SerializeToJson(const DetectionEvent& event) const;

    /**
     * @brief Build subject for stream
     */
    [[nodiscard]] std::string BuildSubject(std::string_view stream_id) const;

    /**
     * @brief Background reconnect thread function
     */
    void ReconnectThreadFunc();

    /**
     * @brief Set state and log
     */
    void SetState(NatsState new_state);

    /**
     * @brief Set last error
     */
    void SetError(std::string_view error);

    std::string nats_url_;
    mutable std::mutex url_mutex_;

    natsConnection* connection_{nullptr};
    natsOptions* options_{nullptr};
    mutable std::mutex connection_mutex_;

    std::atomic<NatsState> state_{NatsState::kDisconnected};

    // Statistics
    std::atomic<uint64_t> messages_published_{0};
    std::atomic<int64_t> last_publish_time_{0};
    std::atomic<int32_t> reconnect_attempts_{0};
    std::string last_error_;
    mutable std::mutex error_mutex_;

    // Auto-reconnect
    std::atomic<bool> auto_reconnect_enabled_{true};
    std::thread reconnect_thread_;
    std::atomic<bool> reconnect_thread_running_{false};
    std::condition_variable reconnect_cv_;
    std::mutex reconnect_mutex_;

    static constexpr int kReconnectIntervalSeconds = 5;
    static constexpr int kMaxReconnectAttempts = 0;  // 0 = unlimited
};

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_NATS_PUBLISHER_H_
