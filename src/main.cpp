#include "common.h"
#include "config.h"
#include "debug_utils.h"
#include "grpc_server.h"
#include "model_registry.h"
#include "stream_manager.h"

#include <gst/gst.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <thread>

namespace {

// Global instances for signal handling
std::atomic<bool> g_shutdown_requested{false};
stream_daemon::StreamManager* g_stream_manager = nullptr;
stream_daemon::GrpcServer* g_grpc_server = nullptr;

/**
 * @brief Signal handler for graceful shutdown
 */
void SignalHandler(int signum) {
    const char* signal_name = (signum == SIGINT) ? "SIGINT" : "SIGTERM";
    std::cout << "\nReceived " << signal_name << ", initiating shutdown...\n";

    g_shutdown_requested = true;

    // Stop components in reverse order
    if (g_grpc_server) {
        g_grpc_server->Stop();
    }

    if (g_stream_manager) {
        g_stream_manager->Stop();
    }
}

/**
 * @brief Setup signal handlers
 */
void SetupSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = SignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

/**
 * @brief Print usage information
 */
void PrintUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  -c, --config PATH      Configuration file path (default: config.yaml)\n"
              << "  -g, --generate-config  Generate default config file and exit\n"
              << "  -d, --debug LEVEL      Override GStreamer debug level (0-9)\n"
              << "  -p, --port PORT        Override gRPC server port\n"
              << "  -n, --nats-url URL     Override NATS server URL\n"
              << "      --check-plugins    Check for required plugins and exit\n"
              << "  -h, --help             Show this help message\n"
              << "  -v, --version          Show version information\n"
              << "\n"
              << "Environment Variables:\n"
              << "  STREAM_DAEMON_CONFIG   Default config file path\n"
              << "  GST_DEBUG              GStreamer debug level\n"
              << "\n"
              << "Examples:\n"
              << "  " << program_name << "\n"
              << "  " << program_name << " -c /etc/stream-daemon/config.yaml\n"
              << "  " << program_name << " --generate-config > my-config.yaml\n"
              << "  " << program_name << " -c config.yaml --port 50052 --debug 3\n"
              << "\n";
}

/**
 * @brief Print version information
 */
void PrintVersion() {
    std::cout << "Stream Processing Daemon v1.0.0\n"
              << "Built with:\n"
              << "  - GStreamer " << GST_VERSION_MAJOR << "."
              << GST_VERSION_MINOR << "." << GST_VERSION_MICRO << "\n"
              << "  - C++17\n";
}

/**
 * @brief Check for required plugins
 */
bool CheckRequiredPlugins() {
    using namespace stream_daemon::debug;

    std::cout << "Checking required plugins...\n\n";

    // Check GStreamer
    std::cout << "GStreamer: OK\n";

    // Check Hailo plugins
    bool hailo_ok = GStreamerDebug::CheckHailoPlugins();
    std::cout << "Hailo plugins: " << (hailo_ok ? "OK" : "NOT FOUND") << "\n";

    // List key elements
    const char* required_elements[] = {
        "rtspsrc", "rtph264depay", "h264parse", "avdec_h264",
        "videoconvert", "appsink"
    };

    std::cout << "\nRequired GStreamer elements:\n";
    bool all_ok = true;
    for (const char* elem : required_elements) {
        GstElementFactory* factory = gst_element_factory_find(elem);
        bool available = (factory != nullptr);
        if (factory) gst_object_unref(factory);

        std::cout << "  " << elem << ": " << (available ? "OK" : "NOT FOUND") << "\n";
        if (!available) all_ok = false;
    }

    std::cout << "\nHailo elements:\n";
    const char* hailo_elements[] = {"hailonet", "hailofilter"};
    for (const char* elem : hailo_elements) {
        GstElementFactory* factory = gst_element_factory_find(elem);
        bool available = (factory != nullptr);
        if (factory) gst_object_unref(factory);

        std::cout << "  " << elem << ": " << (available ? "OK" : "NOT FOUND") << "\n";
    }

    return all_ok && hailo_ok;
}

/**
 * @brief Command line options
 */
struct CommandLineOptions {
    std::string config_path = "config.yaml";
    bool generate_config = false;
    bool check_plugins = false;
    bool show_help = false;
    bool show_version = false;

    // Overrides (optional)
    std::optional<int> debug_level;
    std::optional<int> grpc_port;
    std::optional<std::string> nats_url;
};

/**
 * @brief Parse command line arguments
 */
CommandLineOptions ParseArguments(int argc, char* argv[]) {
    CommandLineOptions opts;

    // Check environment variable for config path
    if (const char* env_config = std::getenv("STREAM_DAEMON_CONFIG")) {
        opts.config_path = env_config;
    }

    static struct option long_options[] = {
        {"config",          required_argument, nullptr, 'c'},
        {"generate-config", no_argument,       nullptr, 'g'},
        {"debug",           required_argument, nullptr, 'd'},
        {"port",            required_argument, nullptr, 'p'},
        {"nats-url",        required_argument, nullptr, 'n'},
        {"check-plugins",   no_argument,       nullptr, 'C'},
        {"help",            no_argument,       nullptr, 'h'},
        {"version",         no_argument,       nullptr, 'v'},
        {nullptr,           0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:gd:p:n:hv", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                opts.config_path = optarg;
                break;
            case 'g':
                opts.generate_config = true;
                break;
            case 'd':
                opts.debug_level = std::atoi(optarg);
                break;
            case 'p':
                opts.grpc_port = std::atoi(optarg);
                break;
            case 'n':
                opts.nats_url = optarg;
                break;
            case 'C':
                opts.check_plugins = true;
                break;
            case 'h':
                opts.show_help = true;
                break;
            case 'v':
                opts.show_version = true;
                break;
            default:
                opts.show_help = true;
                break;
        }
    }

    return opts;
}

/**
 * @brief Find config file in common locations
 */
std::string FindConfigFile(const std::string& specified_path) {
    // If specified path exists, use it
    if (std::filesystem::exists(specified_path)) {
        return specified_path;
    }

    // Search in common locations
    std::vector<std::string> search_paths = {
        "config.yaml",
        "./config.yaml",
        "/etc/stream-daemon/config.yaml",
        "/opt/stream-daemon/config.yaml",
        std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config/stream-daemon/config.yaml"
    };

    for (const auto& path : search_paths) {
        if (!path.empty() && std::filesystem::exists(path)) {
            return path;
        }
    }

    // Return original path (will trigger error later)
    return specified_path;
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace stream_daemon;

    // Parse arguments
    auto opts = ParseArguments(argc, argv);

    if (opts.show_help) {
        PrintUsage(argv[0]);
        return 0;
    }

    if (opts.show_version) {
        PrintVersion();
        return 0;
    }

    // Initialize GStreamer
    gst_init(&argc, &argv);

    // Check plugins mode
    if (opts.check_plugins) {
        bool ok = CheckRequiredPlugins();
        gst_deinit();
        return ok ? 0 : 1;
    }

    // Generate config mode
    if (opts.generate_config) {
        std::cout << DaemonConfig::GetDefault().ToYamlString();
        gst_deinit();
        return 0;
    }

    // Print startup banner
    std::cout << "\n"
              << "╔═══════════════════════════════════════════════════════╗\n"
              << "║       Stream Processing Daemon v1.0.0                 ║\n"
              << "║       GStreamer + Hailo NPU Inference                 ║\n"
              << "╚═══════════════════════════════════════════════════════╝\n\n";

    // Load configuration
    std::string config_path = FindConfigFile(opts.config_path);
    DaemonConfig config;

    if (std::filesystem::exists(config_path)) {
        auto result = DaemonConfig::LoadFromFile(config_path);
        if (IsError(result)) {
            LogError("Failed to load config: " + GetError(result));
            gst_deinit();
            return 1;
        }
        config = GetValue(std::move(result));
        LogInfo("Configuration loaded from: " + config_path);
    } else {
        LogWarning("Config file not found: " + config_path);
        LogWarning("Using default configuration. Run with --generate-config to create one.");
        config = DaemonConfig::GetDefault();
    }

    // Apply command line overrides
    if (opts.debug_level) {
        config.gstreamer.debug_level = *opts.debug_level;
    }
    if (opts.grpc_port) {
        config.grpc.port = *opts.grpc_port;
    }
    if (opts.nats_url) {
        config.nats.url = *opts.nats_url;
    }

    // Apply GStreamer debug level
    if (config.gstreamer.debug_level > 0) {
        debug::GStreamerDebug::SetDebugLevel(config.gstreamer.debug_level,
                                              config.gstreamer.debug_categories);
    }

    // Enable DOT graphs if configured
    if (config.gstreamer.enable_dot_graphs) {
        debug::GStreamerDebug::EnableDotFileGeneration(config.gstreamer.dot_graph_path);
    }

    LogInfo("Starting Stream Processing Daemon...");
    LogInfo("NATS URL: " + config.nats.url);
    LogInfo("gRPC port: " + std::to_string(config.grpc.port));

    // Setup signal handlers
    SetupSignalHandlers();

    // Create ModelRegistry
    auto model_registry = std::make_shared<ModelRegistry>(config.models.models_dir);
    if (auto result = model_registry->Initialize(); IsError(result)) {
        LogError("Failed to initialize ModelRegistry: " + GetError(result));
        gst_deinit();
        return 1;
    }
    LogInfo("Models directory: " + config.models.models_dir);
    LogInfo("Registered models: " + std::to_string(model_registry->GetModelCount()));

    // Create StreamManager
    auto manager_result = StreamManager::Create(config.nats.url);
    if (IsError(manager_result)) {
        LogError("Failed to create StreamManager: " + GetError(manager_result));
        gst_deinit();
        return 1;
    }

    std::shared_ptr<StreamManager> stream_manager = std::move(GetValue(std::move(manager_result)));
    g_stream_manager = stream_manager.get();

    // Create gRPC server
    auto grpc_result = GrpcServer::Create(stream_manager, model_registry, config.grpc.port);
    if (IsError(grpc_result)) {
        LogError("Failed to create gRPC server: " + GetError(grpc_result));
        gst_deinit();
        return 1;
    }

    auto grpc_server = GetValue(std::move(grpc_result));
    g_grpc_server = grpc_server.get();

    // Start StreamManager
    stream_manager->Start();

    // Start gRPC server
    if (auto result = grpc_server->Start(); IsError(result)) {
        LogError("Failed to start gRPC server: " + GetError(result));
        stream_manager->Stop();
        gst_deinit();
        return 1;
    }

    // Set up global callbacks for monitoring
    stream_manager->SetGlobalDetectionCallback([](const DetectionEvent& event) {
        LogDebug("Detection on " + event.stream_id +
                ": " + std::to_string(event.detections.size()) + " objects");
    });

    stream_manager->SetGlobalStateChangeCallback([](std::string_view stream_id, StreamState state) {
        LogInfo("Stream " + std::string(stream_id) +
               " state changed to: " + std::string(StreamStateToString(state)));
    });

    stream_manager->SetGlobalErrorCallback([](std::string_view stream_id, std::string_view error) {
        LogError("Stream " + std::string(stream_id) + " error: " + std::string(error));
    });

    LogInfo("Daemon started successfully. Press Ctrl+C to stop.");

    // Wait for shutdown signal
    while (!g_shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    LogInfo("Shutting down...");

    grpc_server->Stop();
    stream_manager->Stop();

    g_grpc_server = nullptr;
    g_stream_manager = nullptr;

    gst_deinit();

    LogInfo("Daemon stopped.");

    return 0;
}
