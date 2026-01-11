#include "config.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>

namespace stream_daemon {

namespace {

// Helper to safely get value from YAML node
template <typename T>
T GetOr(const YAML::Node& node, const std::string& key, const T& default_value) {
    if (node[key]) {
        try {
            return node[key].as<T>();
        } catch (...) {
            return default_value;
        }
    }
    return default_value;
}

// Helper to get vector from YAML node
std::vector<std::string> GetStringVector(const YAML::Node& node, const std::string& key) {
    std::vector<std::string> result;
    if (node[key] && node[key].IsSequence()) {
        for (const auto& item : node[key]) {
            result.push_back(item.as<std::string>());
        }
    }
    return result;
}

void ParseNatsConfig(const YAML::Node& node, NatsConfig& config) {
    if (!node) return;

    config.url = GetOr<std::string>(node, "url", config.url);
    config.auto_reconnect = GetOr<bool>(node, "auto_reconnect", config.auto_reconnect);
    config.reconnect_interval_seconds = GetOr<int>(node, "reconnect_interval_seconds", config.reconnect_interval_seconds);
    config.max_reconnect_attempts = GetOr<int>(node, "max_reconnect_attempts", config.max_reconnect_attempts);
    config.connection_timeout_ms = GetOr<int>(node, "connection_timeout_ms", config.connection_timeout_ms);
}

void ParseGrpcConfig(const YAML::Node& node, GrpcConfig& config) {
    if (!node) return;

    config.port = GetOr<int>(node, "port", config.port);
    config.bind_address = GetOr<std::string>(node, "bind_address", config.bind_address);
    config.max_message_size_mb = GetOr<int>(node, "max_message_size_mb", config.max_message_size_mb);
    config.enable_health_check = GetOr<bool>(node, "enable_health_check", config.enable_health_check);
}

void ParseStreamConfig(const YAML::Node& node, DefaultStreamConfig& config) {
    if (!node) return;

    config.width = GetOr<int>(node, "width", config.width);
    config.height = GetOr<int>(node, "height", config.height);
    config.fps = GetOr<int>(node, "fps", config.fps);
    config.confidence_threshold = GetOr<float>(node, "confidence_threshold", config.confidence_threshold);
    config.class_filter = GetStringVector(node, "class_filter");
}

void ParseHailoConfig(const YAML::Node& node, HailoConfig& config) {
    if (!node) return;

    config.device_id = GetOr<std::string>(node, "device_id", config.device_id);
    config.batch_size = GetOr<int>(node, "batch_size", config.batch_size);
    config.post_process_so = GetOr<std::string>(node, "post_process_so", config.post_process_so);
    config.function_name = GetOr<std::string>(node, "function_name", config.function_name);
}

void ParseGStreamerConfig(const YAML::Node& node, GStreamerConfig& config) {
    if (!node) return;

    config.debug_level = GetOr<int>(node, "debug_level", config.debug_level);
    config.debug_categories = GetOr<std::string>(node, "debug_categories", config.debug_categories);
    config.plugin_path = GetOr<std::string>(node, "plugin_path", config.plugin_path);
    config.enable_dot_graphs = GetOr<bool>(node, "enable_dot_graphs", config.enable_dot_graphs);
    config.dot_graph_path = GetOr<std::string>(node, "dot_graph_path", config.dot_graph_path);
}

void ParseLogConfig(const YAML::Node& node, LogConfig& config) {
    if (!node) return;

    config.level = GetOr<std::string>(node, "level", config.level);
    config.file_path = GetOr<std::string>(node, "file_path", config.file_path);
    config.enable_color = GetOr<bool>(node, "enable_color", config.enable_color);
    config.enable_timestamp = GetOr<bool>(node, "enable_timestamp", config.enable_timestamp);
}

void ParsePerformanceConfig(const YAML::Node& node, PerformanceConfig& config) {
    if (!node) return;

    config.max_streams = GetOr<int>(node, "max_streams", config.max_streams);
    config.buffer_size = GetOr<int>(node, "buffer_size", config.buffer_size);
    config.drop_frames = GetOr<bool>(node, "drop_frames", config.drop_frames);
    config.rtsp_latency_ms = GetOr<int>(node, "rtsp_latency_ms", config.rtsp_latency_ms);
    config.rtsp_timeout_us = GetOr<int>(node, "rtsp_timeout_us", config.rtsp_timeout_us);
    config.rtsp_retry = GetOr<int>(node, "rtsp_retry", config.rtsp_retry);
}

void ParseModelsConfig(const YAML::Node& node, ModelStorageConfig& config) {
    if (!node) return;

    config.models_dir = GetOr<std::string>(node, "models_dir", config.models_dir);
}

}  // namespace

// ============================================================================
// DaemonConfig
// ============================================================================

Result<DaemonConfig> DaemonConfig::LoadFromFile(std::string_view path) {
    try {
        std::string path_str{path};
        std::ifstream file{path_str};
        if (!file.is_open()) {
            return std::string("Failed to open config file: " + path_str);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return LoadFromString(buffer.str());

    } catch (const std::exception& e) {
        return std::string("Failed to read config file: " + std::string(e.what()));
    }
}

Result<DaemonConfig> DaemonConfig::LoadFromString(std::string_view yaml_content) {
    try {
        YAML::Node root = YAML::Load(std::string(yaml_content));

        DaemonConfig config = GetDefault();

        ParseNatsConfig(root["nats"], config.nats);
        ParseGrpcConfig(root["grpc"], config.grpc);
        ParseStreamConfig(root["stream"], config.stream);
        ParseHailoConfig(root["hailo"], config.hailo);
        ParseGStreamerConfig(root["gstreamer"], config.gstreamer);
        ParseLogConfig(root["log"], config.log);
        ParsePerformanceConfig(root["performance"], config.performance);
        ParseModelsConfig(root["models"], config.models);

        // Validate
        if (auto result = config.Validate(); IsError(result)) {
            return GetError(result);
        }

        return config;

    } catch (const YAML::Exception& e) {
        return std::string("YAML parse error: " + std::string(e.what()));
    } catch (const std::exception& e) {
        return std::string("Config parse error: " + std::string(e.what()));
    }
}

VoidResult DaemonConfig::SaveToFile(std::string_view path) const {
    try {
        std::string path_str{path};
        std::ofstream file{path_str};
        if (!file.is_open()) {
            return MakeError("Failed to open file for writing: " + path_str);
        }

        file << ToYamlString();
        return MakeOk();

    } catch (const std::exception& e) {
        return MakeError("Failed to save config: " + std::string(e.what()));
    }
}

std::string DaemonConfig::ToYamlString() const {
    YAML::Emitter out;
    out << YAML::BeginMap;

    // NATS
    out << YAML::Key << "nats" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "url" << YAML::Value << nats.url;
    out << YAML::Key << "auto_reconnect" << YAML::Value << nats.auto_reconnect;
    out << YAML::Key << "reconnect_interval_seconds" << YAML::Value << nats.reconnect_interval_seconds;
    out << YAML::Key << "max_reconnect_attempts" << YAML::Value << nats.max_reconnect_attempts;
    out << YAML::Key << "connection_timeout_ms" << YAML::Value << nats.connection_timeout_ms;
    out << YAML::EndMap;

    // gRPC
    out << YAML::Key << "grpc" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "port" << YAML::Value << grpc.port;
    out << YAML::Key << "bind_address" << YAML::Value << grpc.bind_address;
    out << YAML::Key << "max_message_size_mb" << YAML::Value << grpc.max_message_size_mb;
    out << YAML::Key << "enable_health_check" << YAML::Value << grpc.enable_health_check;
    out << YAML::EndMap;

    // Stream defaults
    out << YAML::Key << "stream" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "width" << YAML::Value << stream.width;
    out << YAML::Key << "height" << YAML::Value << stream.height;
    out << YAML::Key << "fps" << YAML::Value << stream.fps;
    out << YAML::Key << "confidence_threshold" << YAML::Value << stream.confidence_threshold;
    out << YAML::Key << "class_filter" << YAML::Value << YAML::BeginSeq;
    for (const auto& cls : stream.class_filter) {
        out << cls;
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;

    // Hailo
    out << YAML::Key << "hailo" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "device_id" << YAML::Value << hailo.device_id;
    out << YAML::Key << "batch_size" << YAML::Value << hailo.batch_size;
    out << YAML::Key << "post_process_so" << YAML::Value << hailo.post_process_so;
    out << YAML::Key << "function_name" << YAML::Value << hailo.function_name;
    out << YAML::EndMap;

    // GStreamer
    out << YAML::Key << "gstreamer" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "debug_level" << YAML::Value << gstreamer.debug_level;
    out << YAML::Key << "debug_categories" << YAML::Value << gstreamer.debug_categories;
    out << YAML::Key << "plugin_path" << YAML::Value << gstreamer.plugin_path;
    out << YAML::Key << "enable_dot_graphs" << YAML::Value << gstreamer.enable_dot_graphs;
    out << YAML::Key << "dot_graph_path" << YAML::Value << gstreamer.dot_graph_path;
    out << YAML::EndMap;

    // Log
    out << YAML::Key << "log" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "level" << YAML::Value << log.level;
    out << YAML::Key << "file_path" << YAML::Value << log.file_path;
    out << YAML::Key << "enable_color" << YAML::Value << log.enable_color;
    out << YAML::Key << "enable_timestamp" << YAML::Value << log.enable_timestamp;
    out << YAML::EndMap;

    // Performance
    out << YAML::Key << "performance" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "max_streams" << YAML::Value << performance.max_streams;
    out << YAML::Key << "buffer_size" << YAML::Value << performance.buffer_size;
    out << YAML::Key << "drop_frames" << YAML::Value << performance.drop_frames;
    out << YAML::Key << "rtsp_latency_ms" << YAML::Value << performance.rtsp_latency_ms;
    out << YAML::Key << "rtsp_timeout_us" << YAML::Value << performance.rtsp_timeout_us;
    out << YAML::Key << "rtsp_retry" << YAML::Value << performance.rtsp_retry;
    out << YAML::EndMap;

    // Models
    out << YAML::Key << "models" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "models_dir" << YAML::Value << models.models_dir;
    out << YAML::EndMap;

    out << YAML::EndMap;

    return out.c_str();
}

DaemonConfig DaemonConfig::GetDefault() {
    return DaemonConfig{};
}

VoidResult DaemonConfig::Validate() const {
    // Validate NATS
    if (nats.url.empty()) {
        return MakeError("NATS URL cannot be empty");
    }
    if (nats.reconnect_interval_seconds < 1) {
        return MakeError("NATS reconnect interval must be at least 1 second");
    }

    // Validate gRPC
    if (grpc.port <= 0 || grpc.port > 65535) {
        return MakeError("gRPC port must be between 1 and 65535");
    }

    // Validate stream
    if (stream.width <= 0 || stream.height <= 0) {
        return MakeError("Stream width and height must be positive");
    }
    if (stream.fps <= 0 || stream.fps > 120) {
        return MakeError("Stream FPS must be between 1 and 120");
    }
    if (stream.confidence_threshold < 0.0f || stream.confidence_threshold > 1.0f) {
        return MakeError("Confidence threshold must be between 0.0 and 1.0");
    }

    // Validate Hailo
    if (hailo.batch_size < 1) {
        return MakeError("Hailo batch size must be at least 1");
    }

    // Validate GStreamer
    if (gstreamer.debug_level < 0 || gstreamer.debug_level > 9) {
        return MakeError("GStreamer debug level must be between 0 and 9");
    }

    // Validate performance
    if (performance.max_streams < 1 || performance.max_streams > 16) {
        return MakeError("Max streams must be between 1 and 16");
    }

    return MakeOk();
}

// ============================================================================
// ConfigManager
// ============================================================================

ConfigManager& ConfigManager::Instance() {
    static ConfigManager instance;
    return instance;
}

VoidResult ConfigManager::Load(std::string_view path) {
    auto result = DaemonConfig::LoadFromFile(path);
    if (IsError(result)) {
        return MakeError(GetError(result));
    }

    config_ = GetValue(std::move(result));
    file_path_ = std::string(path);
    loaded_ = true;

    LogInfo("Configuration loaded from: " + file_path_);
    return MakeOk();
}

VoidResult ConfigManager::Reload() {
    if (!loaded_ || file_path_.empty()) {
        return MakeError("No configuration file loaded");
    }

    return Load(file_path_);
}

}  // namespace stream_daemon
