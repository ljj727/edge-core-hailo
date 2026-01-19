#ifndef STREAM_DAEMON_COMMON_H_
#define STREAM_DAEMON_COMMON_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace stream_daemon {

// ============================================================================
// Constants
// ============================================================================

inline constexpr int kDefaultWidth = 1920;
inline constexpr int kDefaultHeight = 1080;
inline constexpr int kDefaultFps = 30;
inline constexpr float kDefaultConfidenceThreshold = 0.5f;
inline constexpr int kDefaultGrpcPort = 50051;
inline constexpr std::string_view kDefaultNatsUrl = "nats://localhost:4222";
inline constexpr int kMaxStreams = 4;
inline constexpr int kReconnectDelaySeconds = 3;

// ============================================================================
// Enums
// ============================================================================

enum class StreamState {
    kStarting,
    kRunning,
    kStopped,
    kError,
    kReconnecting
};

[[nodiscard]] constexpr std::string_view StreamStateToString(StreamState state) noexcept {
    switch (state) {
        case StreamState::kStarting:     return "STARTING";
        case StreamState::kRunning:      return "RUNNING";
        case StreamState::kStopped:      return "STOPPED";
        case StreamState::kError:        return "ERROR";
        case StreamState::kReconnecting: return "RECONNECTING";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr StreamState StringToStreamState(std::string_view str) noexcept {
    if (str == "STARTING")     return StreamState::kStarting;
    if (str == "RUNNING")      return StreamState::kRunning;
    if (str == "STOPPED")      return StreamState::kStopped;
    if (str == "ERROR")        return StreamState::kError;
    if (str == "RECONNECTING") return StreamState::kReconnecting;
    return StreamState::kStopped;
}

// ============================================================================
// Data Structures
// ============================================================================

struct BoundingBox {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

struct Keypoint {
    float x{0.0f};       // normalized 0.0 ~ 1.0
    float y{0.0f};       // normalized 0.0 ~ 1.0
    float visible{0.0f}; // visibility/confidence (0.0 ~ 1.0)
};

struct Detection {
    std::string class_name;
    int class_id{0};
    float confidence{0.0f};
    BoundingBox bbox;
    std::vector<std::string> event_setting_ids;  // 이 객체가 발생시킨 이벤트들 (복수 ROI 지원)
    std::vector<Keypoint> keypoints;  // pose keypoints (4 points for vehicle)
};

struct StreamConfig {
    int width{kDefaultWidth};
    int height{kDefaultHeight};
    int fps{kDefaultFps};
    float confidence_threshold{kDefaultConfidenceThreshold};
};

struct StreamInfo {
    std::string stream_id;
    std::string rtsp_url;
    std::string hef_path;
    std::string model_id;              // App ID (for tracking)
    StreamConfig config;

    // Model configuration for inference
    std::string task;                  // "det" or "pose"
    int num_keypoints{0};              // Number of keypoints for pose model
    std::vector<std::string> labels;   // Class labels
};

struct StreamStatus {
    std::string stream_id;
    std::string rtsp_url;
    std::string model_id;
    StreamState state{StreamState::kStopped};
    uint64_t frame_count{0};
    double current_fps{0.0};
    uint64_t uptime_seconds{0};
    std::string last_error;
    int64_t last_detection_time{0};
};

// 이벤트 상태 (0=SAFE/NONE, 1=WARNING, 2=DANGER/ALARM)
struct EventStatus {
    int status{0};
    std::vector<std::string> labels;   // 해당 이벤트에 걸린 라벨들
};

struct DetectionEvent {
    std::string stream_id;
    int64_t timestamp{0};              // Unix timestamp in milliseconds
    uint64_t frame_number{0};
    double fps{0.0};
    int width{0};                      // Frame width
    int height{0};                     // Frame height
    std::vector<Detection> detections; // 객체 정보
    std::unordered_map<std::string, EventStatus> events;  // event_id -> status
    std::vector<uint8_t> image_data;   // JPEG encoded frame (optional)
};

// ============================================================================
// Result Type (Modern Error Handling)
// ============================================================================

template <typename T>
using Result = std::variant<T, std::string>;

template <typename T>
[[nodiscard]] constexpr bool IsOk(const Result<T>& result) noexcept {
    return std::holds_alternative<T>(result);
}

template <typename T>
[[nodiscard]] constexpr bool IsError(const Result<T>& result) noexcept {
    return std::holds_alternative<std::string>(result);
}

template <typename T>
[[nodiscard]] constexpr const T& GetValue(const Result<T>& result) {
    return std::get<T>(result);
}

template <typename T>
[[nodiscard]] constexpr T&& GetValue(Result<T>&& result) {
    return std::get<T>(std::move(result));
}

template <typename T>
[[nodiscard]] constexpr const std::string& GetError(const Result<T>& result) {
    return std::get<std::string>(result);
}

// Success result helper
struct Ok {};

using VoidResult = Result<Ok>;

[[nodiscard]] inline VoidResult MakeOk() {
    return Ok{};
}

[[nodiscard]] inline VoidResult MakeError(std::string message) {
    return message;
}

// Template version of MakeError for any Result<T>
template <typename T>
[[nodiscard]] inline Result<T> MakeErrorT(std::string message) {
    return message;
}

// String result (wrapped to avoid variant<string, string> issue)
struct StringValue {
    std::string value;
    explicit StringValue(std::string v) : value(std::move(v)) {}
    operator std::string() const { return value; }
};

using StringResult = Result<StringValue>;

[[nodiscard]] inline StringResult MakeStringResult(std::string value) {
    return StringValue{std::move(value)};
}

[[nodiscard]] inline StringResult MakeStringError(std::string message) {
    return message;
}

// ============================================================================
// Callback Types
// ============================================================================

using DetectionCallback = std::function<void(const DetectionEvent&)>;
using StateChangeCallback = std::function<void(std::string_view stream_id, StreamState state)>;
using ErrorCallback = std::function<void(std::string_view stream_id, std::string_view error)>;

// ============================================================================
// Time Utilities
// ============================================================================

[[nodiscard]] inline int64_t GetCurrentTimestampMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

[[nodiscard]] inline uint64_t GetCurrentTimestampSeconds() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

// ============================================================================
// Logging (Simple)
// ============================================================================

enum class LogLevel {
    kDebug,
    kInfo,
    kWarning,
    kError
};

void Log(LogLevel level, std::string_view message);
void LogDebug(std::string_view message);
void LogInfo(std::string_view message);
void LogWarning(std::string_view message);
void LogError(std::string_view message);

// Format helper
template <typename... Args>
[[nodiscard]] std::string Format(std::string_view fmt, Args&&... args);

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_COMMON_H_
