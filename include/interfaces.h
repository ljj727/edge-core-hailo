#ifndef STREAM_DAEMON_INTERFACES_H_
#define STREAM_DAEMON_INTERFACES_H_

#include "common.h"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace stream_daemon {

/**
 * @brief Interface for message publishing (NATS, Mock, etc.)
 *
 * Enables dependency injection and testing without actual NATS connection.
 */
class IMessagePublisher {
public:
    virtual ~IMessagePublisher() = default;

    [[nodiscard]] virtual VoidResult Connect() = 0;
    virtual void Disconnect() = 0;
    [[nodiscard]] virtual bool IsConnected() const noexcept = 0;
    [[nodiscard]] virtual VoidResult Publish(const DetectionEvent& event) = 0;
    [[nodiscard]] virtual VoidResult PublishRaw(
        std::string_view subject, std::string_view json_data) = 0;
};

/**
 * @brief Interface for stream processing (GStreamer, Mock, etc.)
 *
 * Enables testing without actual GStreamer pipelines.
 */
class IStreamProcessor {
public:
    virtual ~IStreamProcessor() = default;

    [[nodiscard]] virtual VoidResult Start() = 0;
    virtual void Stop() = 0;
    [[nodiscard]] virtual VoidResult Update(const StreamInfo& new_info) = 0;
    [[nodiscard]] virtual StreamStatus GetStatus() const = 0;
    [[nodiscard]] virtual std::string_view GetStreamId() const noexcept = 0;
    [[nodiscard]] virtual StreamState GetState() const noexcept = 0;
    [[nodiscard]] virtual bool IsRunning() const noexcept = 0;

    virtual void SetDetectionCallback(DetectionCallback callback) = 0;
    virtual void SetStateChangeCallback(StateChangeCallback callback) = 0;
    virtual void SetErrorCallback(ErrorCallback callback) = 0;
};

/**
 * @brief Interface for stream management
 */
class IStreamManager {
public:
    virtual ~IStreamManager() = default;

    virtual void Start() = 0;
    virtual void Stop() = 0;
    [[nodiscard]] virtual bool IsRunning() const noexcept = 0;

    [[nodiscard]] virtual VoidResult AddStream(const StreamInfo& info) = 0;
    [[nodiscard]] virtual VoidResult RemoveStream(std::string_view stream_id) = 0;
    [[nodiscard]] virtual VoidResult UpdateStream(const StreamInfo& info) = 0;

    [[nodiscard]] virtual std::optional<StreamStatus> GetStreamStatus(
        std::string_view stream_id) const = 0;
    [[nodiscard]] virtual std::vector<StreamStatus> GetAllStreamStatus() const = 0;
    [[nodiscard]] virtual size_t GetStreamCount() const = 0;
    [[nodiscard]] virtual bool HasStream(std::string_view stream_id) const = 0;
};

/**
 * @brief Factory interface for creating stream processors
 *
 * Enables injection of mock processors for testing.
 */
class IStreamProcessorFactory {
public:
    virtual ~IStreamProcessorFactory() = default;

    [[nodiscard]] virtual Result<std::unique_ptr<IStreamProcessor>> Create(
        const StreamInfo& info,
        std::shared_ptr<IMessagePublisher> publisher) = 0;
};

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_INTERFACES_H_
