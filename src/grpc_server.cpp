#include "grpc_server.h"

#include "detector.grpc.pb.h"
#include "detector.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <nlohmann/json.hpp>

namespace stream_daemon {

using json = nlohmann::json;

namespace {

// Status 코드 변환 (Backend와 맞춤)
constexpr int kStatusStarting = 0;
constexpr int kStatusRunning = 1;
constexpr int kStatusStopped = 2;
constexpr int kStatusError = 3;
constexpr int kStatusReconnecting = 4;

[[nodiscard]] int StateToStatus(StreamState state) {
    switch (state) {
        case StreamState::kStarting:
            return kStatusStarting;
        case StreamState::kRunning:
            return kStatusRunning;
        case StreamState::kStopped:
            return kStatusStopped;
        case StreamState::kError:
            return kStatusError;
        case StreamState::kReconnecting:
            return kStatusReconnecting;
        default:
            return kStatusStopped;
    }
}

// JSON settings 파싱
[[nodiscard]] StreamConfig ParseSettings(const std::string& settings_json) {
    StreamConfig config;
    if (settings_json.empty()) return config;

    try {
        auto j = json::parse(settings_json);
        if (j.contains("width")) config.width = j["width"].get<int>();
        if (j.contains("height")) config.height = j["height"].get<int>();
        if (j.contains("fps")) config.fps = j["fps"].get<int>();
        if (j.contains("confidence_threshold")) {
            config.confidence_threshold = j["confidence_threshold"].get<float>();
        }
    } catch (...) {
        // 파싱 실패 시 기본값 사용
    }
    return config;
}

// Model → Proto 변환
void ToProto(const ModelInfo& info, autocare::Model* proto) {
    proto->set_id(info.model_id);
    proto->set_name(info.name);
    proto->set_path(info.hef_path);
    proto->set_platform("hailo8");
    proto->set_framework("hailo");
    proto->set_desc(info.description);
    proto->set_ref_count(info.usage_count);

    // Add outputs
    for (const auto& output : info.outputs) {
        auto* out_proto = proto->add_outputs();
        out_proto->set_label(output.label);
        for (const auto& classifier : output.classifiers) {
            out_proto->add_classifiers(classifier);
        }
    }

    // Add labels (단순 라벨 목록)
    for (const auto& label : info.labels) {
        proto->add_labels(label);
    }
}

// App → Proto 변환 (App = 단일 Model wrapper)
void ToProto(const ModelInfo& info, autocare::App* proto) {
    proto->set_id(info.model_id);
    proto->set_name(info.name);
    proto->set_desc(info.description);
    proto->set_version(info.version);
    proto->set_date(info.date);
    proto->set_framework("hailo");

    // 모델 정보 추가 (1개)
    ToProto(info, proto->add_models());
}

// Inference → Proto 변환
void ToProto(const StreamStatus& status, autocare::Inference* proto) {
    proto->set_app_id(status.model_id);
    proto->set_stream_id(status.stream_id);
    proto->set_uri(status.rtsp_url);
    proto->set_name(status.stream_id);  // name = stream_id
    proto->set_status(StateToStatus(status.state));
    proto->set_frame_count(status.frame_count);
    proto->set_current_fps(status.current_fps);
    proto->set_uptime_seconds(status.uptime_seconds);
    proto->set_last_error(status.last_error);
}

// Camera → Proto 변환
void ToProto(const StreamStatus& status, autocare::Camera* proto) {
    proto->set_id(status.stream_id);
    proto->set_uri(status.rtsp_url);
    proto->set_name(status.stream_id);
    proto->set_status(StateToStatus(status.state));
    proto->set_fps(status.current_fps);
    proto->set_frame_count(status.frame_count);
    proto->set_app_id(status.model_id);
    proto->set_uptime_seconds(status.uptime_seconds);
}

}  // namespace

/**
 * @brief gRPC Detector 서비스 구현
 */
class DetectorServiceImpl final : public autocare::Detector::Service {
public:
    DetectorServiceImpl(std::shared_ptr<StreamManager> manager,
                        std::shared_ptr<ModelRegistry> model_registry)
        : manager_(std::move(manager))
        , model_registry_(std::move(model_registry)) {}

    // ========== App APIs ==========

    grpc::Status InstallApp(
        grpc::ServerContext* context,
        grpc::ServerReader<autocare::AppReq>* reader,
        autocare::AppRes* response) override {

        LogInfo("gRPC: InstallApp request");

        std::vector<uint8_t> zip_data;
        std::string app_id;

        autocare::AppReq req;
        while (reader->Read(&req)) {
            if (context->IsCancelled()) {
                response->set_result(false);
                return grpc::Status::CANCELLED;
            }

            // app_id 저장 (첫 청크에서)
            if (app_id.empty() && !req.app_id().empty()) {
                app_id = req.app_id();
            }

            // 청크 누적
            if (!req.chunk().empty()) {
                const auto& chunk = req.chunk();
                zip_data.insert(zip_data.end(), chunk.begin(), chunk.end());
            }
        }

        if (zip_data.empty()) {
            LogWarning("InstallApp: No data received");
            response->set_result(false);
            return grpc::Status::OK;
        }

        LogInfo("InstallApp: Received " + std::to_string(zip_data.size()) + " bytes");

        // 중복 체크: app_id가 전달됐으면 먼저 확인
        if (!app_id.empty() && model_registry_->HasModel(app_id)) {
            LogWarning("InstallApp: Model '" + app_id + "' already exists");
            response->set_result(false);
            return grpc::Status::OK;
        }

        // 모델 등록 (overwrite=false - 중복 거부)
        auto result = model_registry_->UploadModel(zip_data, false);

        if (IsOk(result)) {
            std::string model_id = GetValue(result).value;
            LogInfo("InstallApp: Installed app '" + model_id + "'");
            response->set_result(true);
        } else {
            LogError("InstallApp: " + GetError(result));
            response->set_result(false);
        }

        return grpc::Status::OK;
    }

    grpc::Status UninstallApp(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::AppReq* request,
        autocare::AppRes* response) override {

        LogInfo("gRPC: UninstallApp request for " + request->app_id());

        if (request->app_id().empty()) {
            response->set_result(false);
            return grpc::Status::OK;
        }

        // 해당 앱의 모든 inference 먼저 제거
        auto statuses = manager_->GetAllStreamStatus();
        for (const auto& status : statuses) {
            if (status.model_id == request->app_id()) {
                manager_->RemoveStream(status.stream_id);
                model_registry_->DecrementUsage(request->app_id());
            }
        }

        // 앱(모델) 삭제
        auto result = model_registry_->DeleteModel(request->app_id(), true);
        response->set_result(IsOk(result));

        return grpc::Status::OK;
    }

    grpc::Status GetAppList(
        [[maybe_unused]] grpc::ServerContext* context,
        [[maybe_unused]] const autocare::AppReq* request,
        autocare::AppList* response) override {

        LogDebug("gRPC: GetAppList request");

        auto models = model_registry_->GetAllModels();
        for (const auto& model : models) {
            ToProto(model, response->add_app());
        }

        return grpc::Status::OK;
    }

    // ========== Camera APIs ==========

    grpc::Status AddCamera(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::CameraReq* request,
        autocare::CameraRes* response) override {

        LogInfo("gRPC: AddCamera camera_id=" + request->camera_id() +
                " uri=" + request->uri());

        // 검증: camera_id, uri 필수
        if (request->camera_id().empty()) {
            response->set_result(false);
            response->set_status(kStatusError);
            response->set_message("camera_id is required");
            return grpc::Status::OK;
        }

        if (request->uri().empty()) {
            response->set_result(false);
            response->set_status(kStatusError);
            response->set_message("uri is required");
            return grpc::Status::OK;
        }

        // 중복 확인
        auto existing = manager_->GetStreamStatus(request->camera_id());
        if (existing) {
            response->set_result(false);
            response->set_camera_id(request->camera_id());
            response->set_status(StateToStatus(existing->state));
            response->set_message("Camera already exists");
            return grpc::Status::OK;
        }

        // StreamInfo 생성 (모델 없이 영상만)
        StreamInfo info;
        info.stream_id = request->camera_id();
        info.rtsp_url = request->uri();
        // hef_path, model_id는 비어있음 → 영상만 스트림

        if (!request->settings().empty()) {
            info.config = ParseSettings(request->settings());
        }

        // 스트림 추가
        auto result = manager_->AddStream(info);

        if (IsOk(result)) {
            response->set_result(true);
            response->set_camera_id(request->camera_id());
            response->set_status(kStatusStarting);
            LogInfo("AddCamera: Camera '" + request->camera_id() + "' added");
        } else {
            response->set_result(false);
            response->set_camera_id(request->camera_id());
            response->set_status(kStatusError);
            response->set_message(GetError(result));
        }

        return grpc::Status::OK;
    }

    grpc::Status RemoveCamera(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::CameraReq* request,
        autocare::CameraRes* response) override {

        LogInfo("gRPC: RemoveCamera camera_id=" + request->camera_id());

        if (request->camera_id().empty()) {
            response->set_result(false);
            response->set_status(kStatusError);
            response->set_message("camera_id is required");
            return grpc::Status::OK;
        }

        // 스트림 상태 조회 (model_id 얻기 위해)
        auto status = manager_->GetStreamStatus(request->camera_id());
        std::string model_id;
        if (status) {
            model_id = status->model_id;
        }

        auto result = manager_->RemoveStream(request->camera_id());

        if (IsOk(result)) {
            if (!model_id.empty()) {
                model_registry_->DecrementUsage(model_id);
            }
            response->set_result(true);
            response->set_camera_id(request->camera_id());
            response->set_status(kStatusStopped);
            LogInfo("RemoveCamera: Camera '" + request->camera_id() + "' removed");
        } else {
            response->set_result(false);
            response->set_camera_id(request->camera_id());
            response->set_status(kStatusError);
            response->set_message(GetError(result));
        }

        return grpc::Status::OK;
    }

    grpc::Status GetCameraList(
        [[maybe_unused]] grpc::ServerContext* context,
        [[maybe_unused]] const autocare::CameraReq* request,
        autocare::CameraList* response) override {

        LogDebug("gRPC: GetCameraList");

        auto statuses = manager_->GetAllStreamStatus();
        for (const auto& status : statuses) {
            ToProto(status, response->add_cameras());
        }

        return grpc::Status::OK;
    }

    grpc::Status GetCamera(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::CameraReq* request,
        autocare::CameraRes* response) override {

        LogDebug("gRPC: GetCamera camera_id=" + request->camera_id());

        if (request->camera_id().empty()) {
            response->set_result(false);
            response->set_status(kStatusError);
            response->set_message("camera_id is required");
            return grpc::Status::OK;
        }

        auto status = manager_->GetStreamStatus(request->camera_id());

        if (status) {
            response->set_result(true);
            response->set_camera_id(status->stream_id);
            response->set_status(StateToStatus(status->state));
            if (!status->last_error.empty()) {
                response->set_message(status->last_error);
            }
        } else {
            response->set_result(false);
            response->set_camera_id(request->camera_id());
            response->set_status(kStatusError);
            response->set_message("Camera not found");
        }

        return grpc::Status::OK;
    }

    // ========== Inference APIs ==========

    grpc::Status AddInference(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::InferenceReq* request,
        autocare::InferenceRes* response) override {

        LogInfo("gRPC: AddInference app=" + request->app_id() +
                " stream=" + request->stream_id());

        // 검증: stream_id 필수
        if (request->stream_id().empty()) {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
            response->set_meta("{\"error\":\"stream_id is required\"}");
            return grpc::Status::OK;
        }

        // 기존 스트림 확인 (카메라가 이미 등록되어 있는지)
        auto existing = manager_->GetStreamStatus(request->stream_id());

        if (existing) {
            // 기존 카메라에 모델 추가 (파이프라인 재시작)
            LogInfo("AddInference: Attaching model to existing camera " + request->stream_id());

            if (request->app_id().empty()) {
                response->set_count(0);
                response->set_status(kStatusError);
                response->set_err(true);
                response->set_meta("{\"error\":\"app_id is required to attach model to existing camera\"}");
                return grpc::Status::OK;
            }

            auto model = model_registry_->GetModel(request->app_id());
            if (!model) {
                response->set_count(0);
                response->set_status(kStatusError);
                response->set_err(true);
                response->set_meta("{\"error\":\"app not found\"}");
                return grpc::Status::OK;
            }

            // 기존 모델이 있으면 usage count 감소
            if (!existing->model_id.empty()) {
                model_registry_->DecrementUsage(existing->model_id);
            }

            // 스트림 업데이트 정보
            StreamInfo info;
            info.stream_id = request->stream_id();
            info.rtsp_url = existing->rtsp_url;  // 기존 URI 유지
            info.hef_path = model->hef_path;
            info.model_id = request->app_id();
            info.task = model->task;
            info.num_keypoints = model->num_keypoints;
            info.labels = model->labels;

            if (!request->settings().empty()) {
                info.config = ParseSettings(request->settings());
            }

            // 스트림 업데이트 (파이프라인 재시작)
            auto result = manager_->UpdateStream(info);

            if (IsOk(result)) {
                model_registry_->IncrementUsage(request->app_id());
                response->set_count(1);
                response->set_status(kStatusStarting);
                response->set_err(false);
                response->set_app_id(request->app_id());
                response->set_stream_id(request->stream_id());
                LogInfo("AddInference: Model attached, pipeline restarting");
            } else {
                response->set_count(0);
                response->set_status(kStatusError);
                response->set_err(true);
                response->set_meta("{\"error\":\"" + GetError(result) + "\"}");
            }

            return grpc::Status::OK;
        }

        // 새 스트림 생성
        if (request->uri().empty()) {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
            response->set_meta("{\"error\":\"uri is required for new stream\"}");
            return grpc::Status::OK;
        }

        // StreamInfo 생성
        StreamInfo info;
        info.stream_id = request->stream_id();
        info.rtsp_url = request->uri();

        // 모델 조회 (app_id가 있으면)
        if (!request->app_id().empty()) {
            auto model = model_registry_->GetModel(request->app_id());
            if (!model) {
                response->set_count(0);
                response->set_status(kStatusError);
                response->set_err(true);
                response->set_meta("{\"error\":\"app not found\"}");
                return grpc::Status::OK;
            }
            info.hef_path = model->hef_path;
            info.model_id = request->app_id();
            info.task = model->task;
            info.num_keypoints = model->num_keypoints;
            info.labels = model->labels;
        }
        // app_id 없으면 hef_path, model_id 비어있음 → 영상만 스트림

        if (!request->settings().empty()) {
            info.config = ParseSettings(request->settings());
        }

        // 스트림 추가
        auto result = manager_->AddStream(info);

        if (IsOk(result)) {
            if (!request->app_id().empty()) {
                model_registry_->IncrementUsage(request->app_id());
            }
            response->set_count(1);
            response->set_status(kStatusStarting);
            response->set_err(false);
            response->set_app_id(request->app_id());
            response->set_stream_id(request->stream_id());
        } else {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
            response->set_meta("{\"error\":\"" + GetError(result) + "\"}");
        }

        return grpc::Status::OK;
    }

    grpc::Status RemoveInference(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::InferenceReq* request,
        autocare::InferenceRes* response) override {

        LogInfo("gRPC: RemoveInference stream=" + request->stream_id());

        if (request->stream_id().empty()) {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
            response->set_meta("{\"error\":\"stream_id is required\"}");
            return grpc::Status::OK;
        }

        // 스트림 상태 조회 (model_id 얻기 위해)
        auto status = manager_->GetStreamStatus(request->stream_id());
        std::string model_id;
        if (status) {
            model_id = status->model_id;
        }

        // 인퍼런스만 제거하고 카메라는 유지 (video-only 모드로 전환)
        auto result = manager_->ClearStreamInference(request->stream_id());

        if (IsOk(result)) {
            if (!model_id.empty()) {
                model_registry_->DecrementUsage(model_id);
            }
            response->set_count(1);
            response->set_status(kStatusStopped);
            response->set_err(false);
            response->set_stream_id(request->stream_id());
            LogInfo("RemoveInference: Inference cleared, camera still running");
        } else {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
            response->set_meta("{\"error\":\"" + GetError(result) + "\"}");
        }

        return grpc::Status::OK;
    }

    grpc::Status RemoveInferenceAll(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::AppReq* request,
        autocare::AppRes* response) override {

        LogInfo("gRPC: RemoveInferenceAll app=" + request->app_id());

        if (request->app_id().empty()) {
            response->set_result(false);
            return grpc::Status::OK;
        }

        int cleared = 0;
        auto statuses = manager_->GetAllStreamStatus();
        for (const auto& status : statuses) {
            if (status.model_id == request->app_id()) {
                // 인퍼런스만 제거하고 카메라는 유지
                if (IsOk(manager_->ClearStreamInference(status.stream_id))) {
                    model_registry_->DecrementUsage(request->app_id());
                    ++cleared;
                }
            }
        }

        LogInfo("RemoveInferenceAll: Cleared " + std::to_string(cleared) + " inferences, cameras still running");
        response->set_result(true);

        return grpc::Status::OK;
    }

    grpc::Status UpdateInference(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::InferenceReq* request,
        autocare::InferenceRes* response) override {

        LogInfo("gRPC: UpdateInference stream=" + request->stream_id() +
                " app=" + request->app_id());

        if (request->stream_id().empty()) {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
            response->set_meta("{\"error\":\"stream_id is required\"}");
            return grpc::Status::OK;
        }

        StreamInfo info;
        info.stream_id = request->stream_id();

        if (!request->uri().empty()) {
            info.rtsp_url = request->uri();
        }

        // 모델 변경/추가
        if (!request->app_id().empty()) {
            auto model = model_registry_->GetModel(request->app_id());
            if (!model) {
                response->set_count(0);
                response->set_status(kStatusError);
                response->set_err(true);
                response->set_meta("{\"error\":\"app not found\"}");
                return grpc::Status::OK;
            }
            info.hef_path = model->hef_path;
            info.model_id = request->app_id();
            info.task = model->task;
            info.num_keypoints = model->num_keypoints;
            info.labels = model->labels;
        }

        if (!request->settings().empty()) {
            info.config = ParseSettings(request->settings());
        }

        auto result = manager_->UpdateStream(info);

        if (IsOk(result)) {
            response->set_count(1);
            response->set_status(kStatusStarting);
            response->set_err(false);
            response->set_stream_id(request->stream_id());
        } else {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
            response->set_meta("{\"error\":\"" + GetError(result) + "\"}");
        }

        return grpc::Status::OK;
    }

    grpc::Status GetInferenceStatus(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::InferenceReq* request,
        autocare::InferenceRes* response) override {

        LogDebug("gRPC: GetInferenceStatus stream=" + request->stream_id());

        if (request->stream_id().empty()) {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
            return grpc::Status::OK;
        }

        auto status = manager_->GetStreamStatus(request->stream_id());

        if (status) {
            response->set_count(1);
            response->set_status(StateToStatus(status->state));
            response->set_err(status->state == StreamState::kError);
            response->set_app_id(status->model_id);
            response->set_stream_id(status->stream_id);
            if (!status->last_error.empty()) {
                response->set_meta("{\"error\":\"" + status->last_error + "\"}");
            }
        } else {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
        }

        return grpc::Status::OK;
    }

    grpc::Status GetInferenceStatusAll(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::AppReq* request,
        autocare::InferenceResList* response) override {

        LogDebug("gRPC: GetInferenceStatusAll");

        auto statuses = manager_->GetAllStreamStatus();

        for (const auto& status : statuses) {
            // app_id 필터링
            if (!request->app_id().empty()) {
                if (status.model_id != request->app_id()) {
                    continue;
                }
            }

            auto* res = response->add_res();
            res->set_count(1);
            res->set_status(StateToStatus(status.state));
            res->set_err(status.state == StreamState::kError);
            res->set_app_id(status.model_id);
            res->set_stream_id(status.stream_id);
        }

        return grpc::Status::OK;
    }

    grpc::Status GetInferenceList(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::InferenceReq* request,
        autocare::InferenceList* response) override {

        LogDebug("gRPC: GetInferenceList");

        auto statuses = manager_->GetAllStreamStatus();

        for (const auto& status : statuses) {
            // app_id 필터링
            if (!request->app_id().empty()) {
                if (status.model_id != request->app_id()) {
                    continue;
                }
            }

            ToProto(status, response->add_inferences());
        }

        return grpc::Status::OK;
    }

    grpc::Status RequestPreviewImage(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::InferenceReq* request,
        autocare::InferenceRes* response) override {

        LogDebug("gRPC: RequestPreviewImage stream=" + request->stream_id());

        if (request->stream_id().empty()) {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
            return grpc::Status::OK;
        }

        // 스냅샷 요청
        auto snapshot = manager_->GetSnapshot(request->stream_id());

        if (snapshot && !snapshot->empty()) {
            response->set_count(1);
            response->set_status(kStatusRunning);
            response->set_err(false);
            response->set_snapshot(snapshot->data(), snapshot->size());
            response->set_stream_id(request->stream_id());
        } else {
            response->set_count(0);
            response->set_status(kStatusError);
            response->set_err(true);
            response->set_meta("{\"error\":\"snapshot not available\"}");
        }

        return grpc::Status::OK;
    }

    // ========== Event Setting ==========

    grpc::Status UpdateEventSetting(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::EventSettingReq* request,
        autocare::EventSettingRes* response) override {

        LogInfo("gRPC: UpdateEventSetting stream=" + request->stream_id());

        if (request->stream_id().empty()) {
            response->set_result(false);
            response->set_message("stream_id is required");
            return grpc::Status::OK;
        }

        if (request->settings_json().empty()) {
            response->set_result(false);
            response->set_message("settings_json is required");
            return grpc::Status::OK;
        }

        auto result = manager_->UpdateEventSettings(
            request->stream_id(), request->settings_json());

        if (IsOk(result)) {
            const auto& term_list = GetValue(result);
            response->set_result(true);
            response->set_message("Success");
            for (const auto& term_id : term_list) {
                response->add_term_ev_list(term_id);
            }
            LogInfo("UpdateEventSetting: " + std::to_string(term_list.size()) +
                    " terminal events");
        } else {
            response->set_result(false);
            response->set_message(GetError(result));
        }

        return grpc::Status::OK;
    }

    grpc::Status ClearEventSetting(
        [[maybe_unused]] grpc::ServerContext* context,
        const autocare::EventSettingReq* request,
        autocare::EventSettingRes* response) override {

        LogInfo("gRPC: ClearEventSetting stream=" + request->stream_id());

        if (request->stream_id().empty()) {
            response->set_result(false);
            response->set_message("stream_id is required");
            return grpc::Status::OK;
        }

        auto result = manager_->ClearEventSettings(request->stream_id());

        if (IsOk(result)) {
            response->set_result(true);
            response->set_message("Event settings cleared");
        } else {
            response->set_result(false);
            response->set_message(GetError(result));
        }

        return grpc::Status::OK;
    }

private:
    std::shared_ptr<StreamManager> manager_;
    std::shared_ptr<ModelRegistry> model_registry_;
};

// ============================================================================
// GrpcServer Implementation
// ============================================================================

Result<std::unique_ptr<GrpcServer>> GrpcServer::Create(
    std::shared_ptr<StreamManager> stream_manager,
    std::shared_ptr<ModelRegistry> model_registry,
    int port) {

    if (!stream_manager) {
        return std::string("StreamManager is required");
    }

    if (!model_registry) {
        return std::string("ModelRegistry is required");
    }

    if (port <= 0 || port > 65535) {
        return std::string("Invalid port number: " + std::to_string(port));
    }

    auto server = std::unique_ptr<GrpcServer>(
        new GrpcServer(std::move(stream_manager), std::move(model_registry), port));

    return server;
}

GrpcServer::GrpcServer(std::shared_ptr<StreamManager> stream_manager,
                       std::shared_ptr<ModelRegistry> model_registry,
                       int port)
    : stream_manager_(std::move(stream_manager))
    , model_registry_(std::move(model_registry))
    , port_(port) {
}

GrpcServer::~GrpcServer() {
    Stop();
}

VoidResult GrpcServer::Start() {
    if (running_) {
        return MakeOk();
    }

    std::string server_address = "0.0.0.0:" + std::to_string(port_);

    service_impl_ = std::make_unique<DetectorServiceImpl>(stream_manager_, model_registry_);

    grpc::EnableDefaultHealthCheckService(true);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(dynamic_cast<grpc::Service*>(service_impl_.get()));

    // 큰 모델 업로드를 위한 메시지 크기 설정
    builder.SetMaxReceiveMessageSize(100 * 1024 * 1024);  // 100MB
    builder.SetMaxSendMessageSize(100 * 1024 * 1024);     // 100MB

    server_ = builder.BuildAndStart();

    if (!server_) {
        return MakeError("Failed to start gRPC server on " + server_address);
    }

    running_ = true;
    LogInfo("gRPC server listening on " + server_address);

    return MakeOk();
}

void GrpcServer::Stop() {
    if (!running_) {
        return;
    }

    LogInfo("Stopping gRPC server...");

    if (server_) {
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        server_->Shutdown(deadline);
        server_.reset();
    }

    service_impl_.reset();
    running_ = false;

    LogInfo("gRPC server stopped");
}

void GrpcServer::Wait() {
    if (server_) {
        server_->Wait();
    }
}

}  // namespace stream_daemon
