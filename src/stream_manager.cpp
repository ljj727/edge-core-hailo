#include "stream_manager.h"

#include <gst/gst.h>

#include <algorithm>

namespace stream_daemon {

// ============================================================================
// Factory Method
// ============================================================================

Result<std::unique_ptr<StreamManager>> StreamManager::Create(std::string_view nats_url) {
    // Initialize GStreamer if not already done
    static bool gst_initialized = false;
    if (!gst_initialized) {
        gst_init(nullptr, nullptr);
        gst_initialized = true;
        LogInfo("GStreamer initialized");
    }

    // Create NATS publisher (does NOT connect immediately)
    auto nats_publisher = NatsPublisher::Create(nats_url);

    auto manager = std::unique_ptr<StreamManager>(
        new StreamManager(std::move(nats_publisher)));

    return manager;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

StreamManager::StreamManager(std::shared_ptr<NatsPublisher> nats_publisher)
    : nats_publisher_(std::move(nats_publisher)) {

    // Create main context and main loop
    main_context_ = g_main_context_new();
    main_loop_ = g_main_loop_new(main_context_, FALSE);
}

StreamManager::~StreamManager() {
    Stop();

    if (main_loop_) {
        g_main_loop_unref(main_loop_);
        main_loop_ = nullptr;
    }

    if (main_context_) {
        g_main_context_unref(main_context_);
        main_context_ = nullptr;
    }
}

// ============================================================================
// Lifecycle Management
// ============================================================================

void StreamManager::Start() {
    if (running_.load()) {
        return;
    }

    running_ = true;
    LogInfo("StreamManager starting...");

    // Try to connect to NATS (non-blocking, will auto-reconnect)
    if (nats_publisher_) {
        auto result = nats_publisher_->Connect();
        if (IsError(result)) {
            LogWarning("NATS connection failed: " + GetError(result));
            LogWarning("NATS will auto-reconnect in background. Detection events will be buffered.");
        }
        // Start background reconnect thread
        nats_publisher_->StartBackgroundReconnect();
    }

    // Start main loop in separate thread
    main_loop_thread_ = std::thread(&StreamManager::MainLoopThread, this);

    LogInfo("StreamManager started");
}

void StreamManager::Stop() {
    if (!running_.load()) {
        return;
    }

    LogInfo("StreamManager stopping...");
    running_ = false;

    // Stop all streams first
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        for (auto& [id, processor] : streams_) {
            processor->Stop();
        }
        streams_.clear();
    }

    // Quit main loop
    if (main_loop_ && g_main_loop_is_running(main_loop_)) {
        g_main_loop_quit(main_loop_);
    }

    // Wait for thread to finish
    if (main_loop_thread_.joinable()) {
        main_loop_thread_.join();
    }

    // Disconnect from NATS
    if (nats_publisher_) {
        nats_publisher_->Disconnect();
    }

    LogInfo("StreamManager stopped");
}

void StreamManager::MainLoopThread() {
    LogInfo("GLib main loop thread started");

    // Push context for this thread
    g_main_context_push_thread_default(main_context_);

    // Run main loop
    g_main_loop_run(main_loop_);

    // Pop context
    g_main_context_pop_thread_default(main_context_);

    LogInfo("GLib main loop thread exited");
}

// ============================================================================
// Stream Management
// ============================================================================

VoidResult StreamManager::AddStream(const StreamInfo& info) {
    std::lock_guard<std::mutex> lock(streams_mutex_);

    // Check if stream already exists
    if (streams_.find(info.stream_id) != streams_.end()) {
        return MakeError("Stream " + info.stream_id + " already exists");
    }

    // Check max streams limit
    if (streams_.size() >= static_cast<size_t>(kMaxStreams)) {
        return MakeError("Maximum number of streams (" +
                        std::to_string(kMaxStreams) + ") reached");
    }

    // Create stream processor
    auto result = StreamProcessor::Create(info, nats_publisher_);
    if (IsError(result)) {
        return MakeError("Failed to create stream: " + GetError(result));
    }

    auto processor = GetValue(std::move(result));

    // Apply global callbacks
    ApplyCallbacks(processor.get());

    // Start the stream
    if (auto start_result = processor->Start(); IsError(start_result)) {
        return start_result;
    }

    // Store processor
    streams_[info.stream_id] = std::move(processor);

    LogInfo("Stream added: " + info.stream_id);
    return MakeOk();
}

VoidResult StreamManager::RemoveStream(std::string_view stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return MakeError("Stream " + std::string(stream_id) + " not found");
    }

    // Stop and remove
    it->second->Stop();
    streams_.erase(it);

    LogInfo("Stream removed: " + std::string(stream_id));
    return MakeOk();
}

VoidResult StreamManager::UpdateStream(const StreamInfo& info) {
    std::lock_guard<std::mutex> lock(streams_mutex_);

    auto it = streams_.find(info.stream_id);
    if (it == streams_.end()) {
        return MakeError("Stream " + info.stream_id + " not found");
    }

    // Update the stream
    if (auto result = it->second->Update(info); IsError(result)) {
        return result;
    }

    LogInfo("Stream updated: " + info.stream_id);
    return MakeOk();
}

VoidResult StreamManager::ClearStreamInference(std::string_view stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return MakeError("Stream " + std::string(stream_id) + " not found");
    }

    // Clear inference (keeps camera running)
    if (auto result = it->second->ClearInference(); IsError(result)) {
        return result;
    }

    LogInfo("Inference cleared from stream: " + std::string(stream_id));
    return MakeOk();
}

Result<std::vector<std::string>> StreamManager::UpdateEventSettings(
    std::string_view stream_id,
    const std::string& settings_json) {

    std::lock_guard<std::mutex> lock(streams_mutex_);

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return MakeErrorT<std::vector<std::string>>("Stream " + std::string(stream_id) + " not found");
    }

    auto result = it->second->UpdateEventSettings(settings_json);
    if (IsOk(result)) {
        LogInfo("Event settings updated for stream: " + std::string(stream_id));
    }
    return result;
}

VoidResult StreamManager::ClearEventSettings(std::string_view stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return MakeError("Stream " + std::string(stream_id) + " not found");
    }

    it->second->ClearEventSettings();
    LogInfo("Event settings cleared for stream: " + std::string(stream_id));
    return MakeOk();
}

// ============================================================================
// Status Queries
// ============================================================================

std::optional<StreamStatus> StreamManager::GetStreamStatus(std::string_view stream_id) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return std::nullopt;
    }

    return it->second->GetStatus();
}

std::vector<StreamStatus> StreamManager::GetAllStreamStatus() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);

    std::vector<StreamStatus> statuses;
    statuses.reserve(streams_.size());

    for (const auto& [id, processor] : streams_) {
        statuses.push_back(processor->GetStatus());
    }

    return statuses;
}

size_t StreamManager::GetStreamCount() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    return streams_.size();
}

bool StreamManager::HasStream(std::string_view stream_id) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    return streams_.find(stream_id) != streams_.end();
}

std::optional<std::vector<uint8_t>> StreamManager::GetSnapshot(std::string_view stream_id) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return std::nullopt;
    }
    return it->second->GetSnapshot();
}

// ============================================================================
// NATS Control
// ============================================================================

VoidResult StreamManager::ConnectNats() {
    if (!nats_publisher_) {
        return MakeError("NATS publisher not initialized");
    }
    return nats_publisher_->Connect();
}

VoidResult StreamManager::ConnectNats(std::string_view new_url) {
    if (!nats_publisher_) {
        return MakeError("NATS publisher not initialized");
    }
    return nats_publisher_->Connect(new_url);
}

void StreamManager::DisconnectNats() {
    if (nats_publisher_) {
        nats_publisher_->Disconnect();
    }
}

VoidResult StreamManager::ReconnectNats() {
    if (!nats_publisher_) {
        return MakeError("NATS publisher not initialized");
    }
    return nats_publisher_->ForceReconnect();
}

bool StreamManager::IsNatsConnected() const noexcept {
    return nats_publisher_ && nats_publisher_->IsConnected();
}

NatsState StreamManager::GetNatsState() const noexcept {
    if (!nats_publisher_) {
        return NatsState::kDisconnected;
    }
    return nats_publisher_->GetState();
}

std::string StreamManager::GetNatsUrl() const {
    if (!nats_publisher_) {
        return "";
    }
    return nats_publisher_->GetUrl();
}

NatsStats StreamManager::GetNatsStats() const {
    if (!nats_publisher_) {
        return NatsStats{};
    }
    return nats_publisher_->GetStats();
}

// ============================================================================
// Callback Management
// ============================================================================

void StreamManager::SetGlobalDetectionCallback(DetectionCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    global_detection_callback_ = std::move(callback);

    // Apply to existing streams
    std::lock_guard<std::mutex> streams_lock(streams_mutex_);
    for (auto& [id, processor] : streams_) {
        processor->SetDetectionCallback(global_detection_callback_);
    }
}

void StreamManager::SetGlobalStateChangeCallback(StateChangeCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    global_state_change_callback_ = std::move(callback);

    // Apply to existing streams
    std::lock_guard<std::mutex> streams_lock(streams_mutex_);
    for (auto& [id, processor] : streams_) {
        processor->SetStateChangeCallback(global_state_change_callback_);
    }
}

void StreamManager::SetGlobalErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    global_error_callback_ = std::move(callback);

    // Apply to existing streams
    std::lock_guard<std::mutex> streams_lock(streams_mutex_);
    for (auto& [id, processor] : streams_) {
        processor->SetErrorCallback(global_error_callback_);
    }
}

void StreamManager::ApplyCallbacks(StreamProcessor* processor) {
    std::lock_guard<std::mutex> lock(callback_mutex_);

    if (global_detection_callback_) {
        processor->SetDetectionCallback(global_detection_callback_);
    }
    if (global_state_change_callback_) {
        processor->SetStateChangeCallback(global_state_change_callback_);
    }
    if (global_error_callback_) {
        processor->SetErrorCallback(global_error_callback_);
    }
}

}  // namespace stream_daemon
