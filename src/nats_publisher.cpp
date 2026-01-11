#include "nats_publisher.h"

#include <nats/nats.h>
#include <nlohmann/json.hpp>

#include <sstream>

namespace stream_daemon {

using json = nlohmann::json;

namespace {

// Base64 인코딩 테이블
constexpr char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Base64 인코딩
std::string Base64Encode(const std::vector<uint8_t>& data) {
    std::string encoded;
    encoded.reserve(((data.size() + 2) / 3) * 4);

    uint32_t val = 0;
    int valb = -6;

    for (uint8_t c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            encoded.push_back(kBase64Table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }

    if (valb > -6) {
        encoded.push_back(kBase64Table[((val << 8) >> (valb + 8)) & 0x3F]);
    }

    while (encoded.size() % 4) {
        encoded.push_back('=');
    }

    return encoded;
}

}  // namespace

// ============================================================================
// Factory Methods
// ============================================================================

std::unique_ptr<NatsPublisher> NatsPublisher::Create(std::string_view nats_url) {
    return std::unique_ptr<NatsPublisher>(
        new NatsPublisher(std::string(nats_url)));
}

Result<std::unique_ptr<NatsPublisher>> NatsPublisher::CreateAndConnect(std::string_view nats_url) {
    auto publisher = Create(nats_url);

    if (auto result = publisher->Connect(); IsError(result)) {
        return GetError(result);
    }

    return publisher;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

NatsPublisher::NatsPublisher(std::string nats_url)
    : nats_url_(std::move(nats_url)) {
}

NatsPublisher::~NatsPublisher() {
    StopBackgroundReconnect();
    Disconnect();
}

NatsPublisher::NatsPublisher(NatsPublisher&& other) noexcept
    : nats_url_(std::move(other.nats_url_))
    , connection_(other.connection_)
    , options_(other.options_)
    , state_(other.state_.load())
    , messages_published_(other.messages_published_.load())
    , last_publish_time_(other.last_publish_time_.load())
    , reconnect_attempts_(other.reconnect_attempts_.load())
    , auto_reconnect_enabled_(other.auto_reconnect_enabled_.load()) {
    other.connection_ = nullptr;
    other.options_ = nullptr;
    other.state_ = NatsState::kDisconnected;
}

NatsPublisher& NatsPublisher::operator=(NatsPublisher&& other) noexcept {
    if (this != &other) {
        StopBackgroundReconnect();
        Disconnect();

        nats_url_ = std::move(other.nats_url_);
        connection_ = other.connection_;
        options_ = other.options_;
        state_ = other.state_.load();
        messages_published_ = other.messages_published_.load();
        last_publish_time_ = other.last_publish_time_.load();
        reconnect_attempts_ = other.reconnect_attempts_.load();
        auto_reconnect_enabled_ = other.auto_reconnect_enabled_.load();

        other.connection_ = nullptr;
        other.options_ = nullptr;
        other.state_ = NatsState::kDisconnected;
    }
    return *this;
}

// ============================================================================
// Connection Management
// ============================================================================

VoidResult NatsPublisher::Connect() {
    return ConnectInternal();
}

VoidResult NatsPublisher::Connect(std::string_view new_url) {
    {
        std::lock_guard<std::mutex> lock(url_mutex_);
        nats_url_ = std::string(new_url);
    }
    return ConnectInternal();
}

VoidResult NatsPublisher::ConnectInternal() {
    std::lock_guard<std::mutex> lock(connection_mutex_);

    // Already connected?
    if (state_ == NatsState::kConnected && connection_) {
        if (natsConnection_Status(connection_) == NATS_CONN_STATUS_CONNECTED) {
            return MakeOk();
        }
    }

    // Clean up existing connection
    if (connection_) {
        natsConnection_Close(connection_);
        natsConnection_Destroy(connection_);
        connection_ = nullptr;
    }
    if (options_) {
        natsOptions_Destroy(options_);
        options_ = nullptr;
    }

    SetState(NatsState::kConnecting);

    std::string url;
    {
        std::lock_guard<std::mutex> url_lock(url_mutex_);
        url = nats_url_;
    }

    LogInfo("Connecting to NATS server at " + url + "...");

    // Create options
    natsStatus status = natsOptions_Create(&options_);
    if (status != NATS_OK) {
        SetState(NatsState::kDisconnected);
        SetError("Failed to create NATS options: " + std::string(natsStatus_GetText(status)));
        return MakeError(last_error_);
    }

    // Set URL
    status = natsOptions_SetURL(options_, url.c_str());
    if (status != NATS_OK) {
        natsOptions_Destroy(options_);
        options_ = nullptr;
        SetState(NatsState::kDisconnected);
        SetError("Failed to set NATS URL: " + std::string(natsStatus_GetText(status)));
        return MakeError(last_error_);
    }

    // Set connection timeout (5 seconds)
    natsOptions_SetTimeout(options_, 5000);

    // Set reconnect options (handled by NATS client internally too)
    natsOptions_SetMaxReconnect(options_, 3);
    natsOptions_SetReconnectWait(options_, 1000);
    natsOptions_SetReconnectBufSize(options_, 8 * 1024 * 1024);

    // Connect
    status = natsConnection_Connect(&connection_, options_);
    if (status != NATS_OK) {
        natsOptions_Destroy(options_);
        options_ = nullptr;
        SetState(NatsState::kDisconnected);
        SetError("Failed to connect to NATS server: " + std::string(natsStatus_GetText(status)));
        LogWarning("NATS connection failed: " + last_error_);

        // Start background reconnect if enabled
        if (auto_reconnect_enabled_) {
            StartBackgroundReconnect();
        }

        return MakeError(last_error_);
    }

    SetState(NatsState::kConnected);
    reconnect_attempts_ = 0;
    LogInfo("Connected to NATS server at " + url);

    return MakeOk();
}

void NatsPublisher::Disconnect() {
    StopBackgroundReconnect();

    std::lock_guard<std::mutex> lock(connection_mutex_);

    if (connection_) {
        natsConnection_Close(connection_);
        natsConnection_Destroy(connection_);
        connection_ = nullptr;
    }

    if (options_) {
        natsOptions_Destroy(options_);
        options_ = nullptr;
    }

    if (state_ != NatsState::kDisconnected) {
        SetState(NatsState::kDisconnected);
        LogInfo("Disconnected from NATS server");
    }
}

bool NatsPublisher::IsConnected() const noexcept {
    if (state_ != NatsState::kConnected) {
        return false;
    }

    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (!connection_) {
        return false;
    }

    return natsConnection_Status(connection_) == NATS_CONN_STATUS_CONNECTED;
}

// ============================================================================
// Publishing
// ============================================================================

VoidResult NatsPublisher::Publish(const DetectionEvent& event) {
    // Don't fail if not connected - just skip
    if (!IsConnected()) {
        return MakeOk();  // Silent skip
    }

    const std::string subject = BuildSubject(event.stream_id);
    const std::string json_data = SerializeToJson(event);

    return PublishRaw(subject, json_data);
}

VoidResult NatsPublisher::PublishRaw(std::string_view subject, std::string_view json_data) {
    std::lock_guard<std::mutex> lock(connection_mutex_);

    if (state_ != NatsState::kConnected || !connection_) {
        // Silent skip if not connected
        return MakeOk();
    }

    // Check connection status
    if (natsConnection_Status(connection_) != NATS_CONN_STATUS_CONNECTED) {
        SetState(NatsState::kDisconnected);
        SetError("NATS connection lost");

        if (auto_reconnect_enabled_) {
            StartBackgroundReconnect();
        }

        return MakeOk();  // Silent skip
    }

    natsStatus status = natsConnection_PublishString(
        connection_,
        std::string(subject).c_str(),
        std::string(json_data).c_str()
    );

    if (status != NATS_OK) {
        SetError("Failed to publish: " + std::string(natsStatus_GetText(status)));
        return MakeError(last_error_);
    }

    ++messages_published_;
    last_publish_time_ = GetCurrentTimestampMs();

    return MakeOk();
}

// ============================================================================
// URL Management
// ============================================================================

std::string NatsPublisher::GetUrl() const {
    std::lock_guard<std::mutex> lock(url_mutex_);
    return nats_url_;
}

void NatsPublisher::SetUrl(std::string_view new_url) {
    bool was_connected = IsConnected();

    if (was_connected) {
        Disconnect();
    }

    {
        std::lock_guard<std::mutex> lock(url_mutex_);
        nats_url_ = std::string(new_url);
    }

    if (was_connected) {
        Connect();
    }
}

// ============================================================================
// Statistics
// ============================================================================

NatsStats NatsPublisher::GetStats() const {
    NatsStats stats;
    stats.messages_published = messages_published_.load();
    stats.last_publish_time = last_publish_time_.load();
    stats.reconnect_attempts = reconnect_attempts_.load();

    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        stats.last_error = last_error_;
    }

    return stats;
}

// ============================================================================
// Auto-Reconnect
// ============================================================================

void NatsPublisher::SetAutoReconnect(bool enabled) {
    auto_reconnect_enabled_ = enabled;

    if (!enabled) {
        StopBackgroundReconnect();
    } else if (state_ == NatsState::kDisconnected) {
        StartBackgroundReconnect();
    }
}

VoidResult NatsPublisher::ForceReconnect() {
    Disconnect();
    return Connect();
}

void NatsPublisher::StartBackgroundReconnect() {
    if (reconnect_thread_running_) {
        return;  // Already running
    }

    if (state_ == NatsState::kConnected) {
        return;  // Already connected
    }

    reconnect_thread_running_ = true;
    SetState(NatsState::kReconnecting);

    reconnect_thread_ = std::thread(&NatsPublisher::ReconnectThreadFunc, this);
}

void NatsPublisher::StopBackgroundReconnect() {
    if (!reconnect_thread_running_) {
        return;
    }

    reconnect_thread_running_ = false;

    // Wake up the thread
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        reconnect_cv_.notify_all();
    }

    if (reconnect_thread_.joinable()) {
        reconnect_thread_.join();
    }
}

void NatsPublisher::ReconnectThreadFunc() {
    LogInfo("NATS background reconnect thread started");

    while (reconnect_thread_running_ && auto_reconnect_enabled_) {
        // Check if max attempts reached
        if (kMaxReconnectAttempts > 0 &&
            reconnect_attempts_ >= kMaxReconnectAttempts) {
            LogError("NATS max reconnect attempts reached");
            break;
        }

        // Wait before attempting
        {
            std::unique_lock<std::mutex> lock(reconnect_mutex_);
            reconnect_cv_.wait_for(lock,
                std::chrono::seconds(kReconnectIntervalSeconds),
                [this] { return !reconnect_thread_running_; });
        }

        if (!reconnect_thread_running_) {
            break;
        }

        // Already connected?
        if (IsConnected()) {
            SetState(NatsState::kConnected);
            break;
        }

        ++reconnect_attempts_;
        LogInfo("NATS reconnect attempt " + std::to_string(reconnect_attempts_.load()) +
               (kMaxReconnectAttempts > 0 ?
                "/" + std::to_string(kMaxReconnectAttempts) : ""));

        auto result = ConnectInternal();
        if (IsOk(result)) {
            LogInfo("NATS reconnected successfully");
            break;
        }
    }

    reconnect_thread_running_ = false;
    LogInfo("NATS background reconnect thread stopped");
}

// ============================================================================
// State Management
// ============================================================================

void NatsPublisher::SetState(NatsState new_state) {
    NatsState old_state = state_.exchange(new_state);
    if (old_state != new_state) {
        LogDebug("NATS state: " + std::string(NatsStateToString(old_state)) +
                " -> " + std::string(NatsStateToString(new_state)));
    }
}

void NatsPublisher::SetError(std::string_view error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = std::string(error);
}

// ============================================================================
// Serialization
// ============================================================================

std::string NatsPublisher::SerializeToJson(const DetectionEvent& event) const {
    json j;

    j["stream_id"] = event.stream_id;
    j["timestamp"] = event.timestamp;
    j["frame_number"] = event.frame_number;
    j["fps"] = event.fps;
    j["width"] = event.width;
    j["height"] = event.height;

    // Detections array (각 detection에 event 정보 포함)
    json detections_array = json::array();
    for (const auto& det : event.detections) {
        json det_obj;
        det_obj["class"] = det.class_name;
        det_obj["class_id"] = det.class_id;
        det_obj["confidence"] = det.confidence;
        det_obj["bbox"] = {
            {"x", det.bbox.x},
            {"y", det.bbox.y},
            {"width", det.bbox.width},
            {"height", det.bbox.height}
        };
        // 이 객체가 발생시킨 이벤트 (없으면 null)
        if (!det.event_setting_id.empty()) {
            det_obj["event"] = det.event_setting_id;
        } else {
            det_obj["event"] = nullptr;
        }

        // Keypoints (pose model only)
        if (!det.keypoints.empty()) {
            json kpts_array = json::array();
            for (const auto& kpt : det.keypoints) {
                kpts_array.push_back({kpt.x, kpt.y, kpt.visible});
            }
            det_obj["keypoints"] = std::move(kpts_array);
        }

        detections_array.push_back(std::move(det_obj));
    }
    j["detections"] = std::move(detections_array);

    // 이미지 데이터 (Base64 인코딩)
    if (!event.image_data.empty()) {
        j["image"] = Base64Encode(event.image_data);
    }

    return j.dump();
}

std::string NatsPublisher::BuildSubject(std::string_view stream_id) const {
    // Frontend가 직접 subscribe 할 수 있는 간단한 subject
    return "stream." + std::string(stream_id);
}

}  // namespace stream_daemon
