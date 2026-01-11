#ifndef STREAM_DAEMON_CONFIG_H_
#define STREAM_DAEMON_CONFIG_H_

#include "common.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stream_daemon {

/**
 * @brief NATS configuration
 */
struct NatsConfig {
    std::string url{"nats://localhost:4222"};
    bool auto_reconnect{true};
    int reconnect_interval_seconds{5};
    int max_reconnect_attempts{0};  // 0 = unlimited
    int connection_timeout_ms{5000};
};

/**
 * @brief gRPC server configuration
 */
struct GrpcConfig {
    int port{50051};
    std::string bind_address{"0.0.0.0"};
    int max_message_size_mb{4};
    bool enable_health_check{true};
};

/**
 * @brief Default stream configuration
 */
struct DefaultStreamConfig {
    int width{1920};
    int height{1080};
    int fps{30};
    float confidence_threshold{0.5f};
    std::vector<std::string> class_filter;  // 빈 배열이면 모든 클래스
};

/**
 * @brief Hailo configuration
 */
struct HailoConfig {
    std::string device_id;                  // 빈 문자열이면 자동 선택
    int batch_size{1};
    std::string post_process_so{"/usr/lib/hailo-post-processes/libyolo_hailortpp_post.so"};
    std::string function_name{"yolov8"};
};

/**
 * @brief GStreamer configuration
 */
struct GStreamerConfig {
    int debug_level{0};                     // 0-9
    std::string debug_categories;           // e.g., "hailonet:5,rtspsrc:3"
    std::string plugin_path;                // 추가 플러그인 경로
    bool enable_dot_graphs{false};
    std::string dot_graph_path{"/tmp/gst-dots"};
};

/**
 * @brief Logging configuration
 */
struct LogConfig {
    std::string level{"info"};              // debug, info, warning, error
    std::string file_path;                  // 빈 문자열이면 stdout만
    bool enable_color{true};
    bool enable_timestamp{true};
};

/**
 * @brief Performance configuration
 */
struct PerformanceConfig {
    int max_streams{4};
    int buffer_size{1};                     // appsink max-buffers
    bool drop_frames{true};                 // appsink drop
    int rtsp_latency_ms{0};
    int rtsp_timeout_us{10000000};          // 10초
    int rtsp_retry{3};
};

/**
 * @brief Model storage configuration
 */
struct ModelStorageConfig {
    std::string models_dir{"/var/lib/stream-daemon/models"};  // 모델 저장 경로
};

/**
 * @brief Complete daemon configuration
 */
struct DaemonConfig {
    NatsConfig nats;
    GrpcConfig grpc;
    DefaultStreamConfig stream;
    HailoConfig hailo;
    GStreamerConfig gstreamer;
    LogConfig log;
    PerformanceConfig performance;
    ModelStorageConfig models;

    /**
     * @brief Load configuration from YAML file
     */
    [[nodiscard]] static Result<DaemonConfig> LoadFromFile(std::string_view path);

    /**
     * @brief Load configuration from YAML string
     */
    [[nodiscard]] static Result<DaemonConfig> LoadFromString(std::string_view yaml_content);

    /**
     * @brief Save configuration to YAML file
     */
    [[nodiscard]] VoidResult SaveToFile(std::string_view path) const;

    /**
     * @brief Convert to YAML string
     */
    [[nodiscard]] std::string ToYamlString() const;

    /**
     * @brief Get default configuration
     */
    [[nodiscard]] static DaemonConfig GetDefault();

    /**
     * @brief Validate configuration
     */
    [[nodiscard]] VoidResult Validate() const;
};

/**
 * @brief Global configuration singleton
 */
class ConfigManager {
public:
    static ConfigManager& Instance();

    // Delete copy/move
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    /**
     * @brief Load configuration from file
     */
    [[nodiscard]] VoidResult Load(std::string_view path);

    /**
     * @brief Get current configuration (const reference)
     */
    [[nodiscard]] const DaemonConfig& Get() const noexcept { return config_; }

    /**
     * @brief Get mutable configuration
     */
    [[nodiscard]] DaemonConfig& GetMutable() noexcept { return config_; }

    /**
     * @brief Check if configuration is loaded
     */
    [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }

    /**
     * @brief Get config file path
     */
    [[nodiscard]] const std::string& GetFilePath() const noexcept { return file_path_; }

    /**
     * @brief Reload configuration from file
     */
    [[nodiscard]] VoidResult Reload();

private:
    ConfigManager() = default;

    DaemonConfig config_;
    std::string file_path_;
    bool loaded_{false};
};

// Convenience function
[[nodiscard]] inline const DaemonConfig& Config() {
    return ConfigManager::Instance().Get();
}

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_CONFIG_H_
