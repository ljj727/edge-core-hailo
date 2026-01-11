#include "event_compositor.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace stream_daemon {

using json = nlohmann::json;

namespace {

// 문자열을 EventType으로 변환 (대소문자 무시)
EventType ParseEventType(const std::string& type_str) {
    std::string lower = type_str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "roi") return EventType::kROI;
    if (lower == "line") return EventType::kLine;
    if (lower == "and") return EventType::kAnd;
    if (lower == "or") return EventType::kOr;
    if (lower == "speed") return EventType::kSpeed;
    if (lower == "hm") return EventType::kHM;
    if (lower == "filter") return EventType::kFilter;
    if (lower == "enex") return EventType::kEnEx;
    if (lower == "alarm") return EventType::kAlarm;
    return EventType::kUnknown;
}

// 문자열을 DetectionPoint로 변환
DetectionPoint ParseDetectionPoint(const std::string& dp_str) {
    if (dp_str == "l:t") return DetectionPoint::kLeftTop;
    if (dp_str == "c:t") return DetectionPoint::kCenterTop;
    if (dp_str == "r:t") return DetectionPoint::kRightTop;
    if (dp_str == "l:c") return DetectionPoint::kLeftCenter;
    if (dp_str == "c:c") return DetectionPoint::kCenter;
    if (dp_str == "r:c") return DetectionPoint::kRightCenter;
    if (dp_str == "l:b") return DetectionPoint::kLeftBottom;
    if (dp_str == "c:b") return DetectionPoint::kCenterBottom;
    if (dp_str == "r:b") return DetectionPoint::kRightBottom;
    return DetectionPoint::kCenterBottom;  // 기본값
}

// 문자열을 LineDirection으로 변환
LineDirection ParseDirection(const std::string& dir_str) {
    if (dir_str == "A2B") return LineDirection::kA2B;
    if (dir_str == "B2A") return LineDirection::kB2A;
    if (dir_str == "BOTH") return LineDirection::kBoth;
    return LineDirection::kBoth;  // 기본값
}

}  // namespace

Result<std::vector<std::string>> EventCompositor::UpdateSettings(
    const std::string& settings_json) {

    std::lock_guard<std::mutex> lock(mutex_);

    // 기존 설정 클리어
    settings_.clear();
    terminal_events_.clear();

    // JSON 파싱
    if (auto result = ParseSettings(settings_json); IsError(result)) {
        return MakeErrorT<std::vector<std::string>>(GetError(result));
    }

    // 이벤트 트리 구성
    BuildEventTree();

    // 터미널 이벤트 식별
    terminal_events_ = FindTerminalEvents();

    LogInfo("EventCompositor: Loaded " + std::to_string(settings_.size()) +
            " events, " + std::to_string(terminal_events_.size()) + " terminals");

    return terminal_events_;
}

void EventCompositor::ClearSettings() {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.clear();
    terminal_events_.clear();
    LogInfo("EventCompositor: Settings cleared");
}

VoidResult EventCompositor::ParseSettings(const std::string& json_str) {
    try {
        auto j = json::parse(json_str);

        // configs 배열 파싱
        if (!j.contains("configs") || !j["configs"].is_array()) {
            return MakeError("Invalid settings: missing configs array");
        }

        for (const auto& config : j["configs"]) {
            EventSetting setting;

            // 필수 필드
            if (!config.contains("eventSettingId")) {
                continue;  // ID 없으면 스킵
            }
            setting.event_setting_id = config["eventSettingId"].get<std::string>();

            if (config.contains("eventSettingName")) {
                setting.event_setting_name = config["eventSettingName"].get<std::string>();
            }

            if (config.contains("eventType")) {
                setting.event_type = ParseEventType(config["eventType"].get<std::string>());
            }

            if (config.contains("parentId")) {
                setting.parent_id = config["parentId"].get<std::string>();
            }

            // points 배열 파싱
            if (config.contains("points") && config["points"].is_array()) {
                for (const auto& point : config["points"]) {
                    if (point.is_array() && point.size() >= 2) {
                        Point2D p;
                        p.x = point[0].get<float>();
                        p.y = point[1].get<float>();
                        setting.points.push_back(p);
                    }
                }
            }

            // target 파싱
            if (config.contains("target") && config["target"].is_object()) {
                const auto& target = config["target"];
                if (target.contains("label") && !target["label"].is_null()) {
                    setting.target.label = target["label"].get<std::string>();
                }
                if (target.contains("classType") && !target["classType"].is_null()) {
                    setting.target.class_type = target["classType"].get<std::string>();
                }
                if (target.contains("resultLabel") && target["resultLabel"].is_array()) {
                    for (const auto& label : target["resultLabel"]) {
                        setting.target.result_label.push_back(label.get<std::string>());
                    }
                }
            }

            // 옵션 필드
            if (config.contains("timeout")) {
                setting.timeout = config["timeout"].get<float>();
            }
            if (config.contains("detectionPoint")) {
                setting.detection_point = ParseDetectionPoint(
                    config["detectionPoint"].get<std::string>());
            }
            if (config.contains("direction")) {
                setting.direction = ParseDirection(config["direction"].get<std::string>());
            }
            if (config.contains("inOrder")) {
                setting.in_order = config["inOrder"].get<bool>();
            }
            if (config.contains("ncond")) {
                setting.ncond = config["ncond"].get<std::string>();
            }
            if (config.contains("turn")) {
                setting.turn = config["turn"].get<int>();
            }
            if (config.contains("regenInterval")) {
                setting.regen_interval = config["regenInterval"].get<float>();
            }
            if (config.contains("ext")) {
                setting.ext = config["ext"].get<std::string>();
            }

            settings_[setting.event_setting_id] = std::move(setting);
        }

        return MakeOk();

    } catch (const json::exception& e) {
        return MakeError("JSON parse error: " + std::string(e.what()));
    }
}

void EventCompositor::BuildEventTree() {
    // parent_id로 자식 관계 설정
    for (auto& [id, setting] : settings_) {
        if (!setting.parent_id.empty()) {
            auto parent_it = settings_.find(setting.parent_id);
            if (parent_it != settings_.end()) {
                parent_it->second.children.push_back(id);
            }
        }
    }
}

std::vector<std::string> EventCompositor::FindTerminalEvents() const {
    std::vector<std::string> terminals;

    for (const auto& [id, setting] : settings_) {
        // 자식이 없는 이벤트 = 터미널
        if (setting.children.empty()) {
            // Filter, HM 등 일부 타입은 터미널로 취급하지 않음
            if (setting.event_type != EventType::kFilter &&
                setting.event_type != EventType::kHM) {
                terminals.push_back(id);
            }
        }
    }

    return terminals;
}

void EventCompositor::CheckEvents(
    std::vector<Detection>& detections,
    int frame_width,
    int frame_height) {

    std::lock_guard<std::mutex> lock(mutex_);

    if (settings_.empty() || detections.empty()) {
        return;
    }

    // 각 detection에 대해 이벤트 체크
    for (auto& det : detections) {
        // 이미 이벤트가 설정되어 있으면 스킵 (첫 번째 매칭만)
        if (!det.event_setting_id.empty()) {
            continue;
        }

        // 모든 이벤트 설정에 대해 체크
        for (const auto& [id, setting] : settings_) {
            bool matched = false;

            switch (setting.event_type) {
                case EventType::kROI: {
                    matched = CheckROIEvent(setting, det, frame_width, frame_height);
                    break;
                }

                case EventType::kLine:
                    // TODO: Line crossing 구현 (트래킹 필요)
                    break;

                case EventType::kAnd:
                case EventType::kOr:
                    // TODO: 복합 이벤트 구현
                    break;

                default:
                    break;
            }

            // 매칭되면 이벤트 ID 태깅하고 다음 detection으로
            if (matched) {
                det.event_setting_id = id;
                break;  // 첫 번째 매칭 이벤트만 설정
            }
        }
    }
}

bool EventCompositor::CheckROIEvent(
    const EventSetting& setting,
    const Detection& det,
    int frame_width,
    int frame_height) const {

    // 타겟 필터 매칭 확인
    if (!MatchesTarget(det, setting.target)) {
        return false;
    }

    // 폴리곤이 없으면 체크 불가
    if (setting.points.size() < 3) {
        return false;
    }

    // detection 기준점 계산 (정규화 좌표)
    Point2D point = GetDetectionPoint(det, setting.detection_point,
                                       frame_width, frame_height);

    // 폴리곤 내부 체크
    return IsPointInPolygon(point, setting.points);
}

bool EventCompositor::MatchesTarget(
    const Detection& det,
    const TargetFilter& target) const {

    // 타겟 라벨이 비어있으면 모든 객체 매칭
    if (target.label.empty()) {
        return true;
    }

    // 라벨 매칭 (대소문자 무시)
    std::string det_label = det.class_name;
    std::string target_label = target.label;

    std::transform(det_label.begin(), det_label.end(), det_label.begin(), ::tolower);
    std::transform(target_label.begin(), target_label.end(), target_label.begin(), ::tolower);

    return det_label == target_label;
}

Point2D EventCompositor::GetDetectionPoint(
    const Detection& det,
    DetectionPoint dp,
    int frame_width,
    int frame_height) const {

    Point2D point;

    float x = static_cast<float>(det.bbox.x);
    float y = static_cast<float>(det.bbox.y);
    float w = static_cast<float>(det.bbox.width);
    float h = static_cast<float>(det.bbox.height);

    switch (dp) {
        case DetectionPoint::kLeftTop:
            point.x = x;
            point.y = y;
            break;
        case DetectionPoint::kCenterTop:
            point.x = x + w / 2;
            point.y = y;
            break;
        case DetectionPoint::kRightTop:
            point.x = x + w;
            point.y = y;
            break;
        case DetectionPoint::kLeftCenter:
            point.x = x;
            point.y = y + h / 2;
            break;
        case DetectionPoint::kCenter:
            point.x = x + w / 2;
            point.y = y + h / 2;
            break;
        case DetectionPoint::kRightCenter:
            point.x = x + w;
            point.y = y + h / 2;
            break;
        case DetectionPoint::kLeftBottom:
            point.x = x;
            point.y = y + h;
            break;
        case DetectionPoint::kCenterBottom:
        default:
            point.x = x + w / 2;
            point.y = y + h;
            break;
        case DetectionPoint::kRightBottom:
            point.x = x + w;
            point.y = y + h;
            break;
    }

    // 정규화 (0.0 ~ 1.0)
    point.x /= static_cast<float>(frame_width);
    point.y /= static_cast<float>(frame_height);

    return point;
}

bool EventCompositor::IsPointInPolygon(
    const Point2D& point,
    const std::vector<Point2D>& polygon) const {

    // Ray casting algorithm
    bool inside = false;
    size_t n = polygon.size();

    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        float xi = polygon[i].x, yi = polygon[i].y;
        float xj = polygon[j].x, yj = polygon[j].y;

        if (((yi > point.y) != (yj > point.y)) &&
            (point.x < (xj - xi) * (point.y - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }

    return inside;
}

size_t EventCompositor::GetSettingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_.size();
}

std::optional<EventSetting> EventCompositor::GetSetting(
    const std::string& event_setting_id) const {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = settings_.find(event_setting_id);
    if (it != settings_.end()) {
        return it->second;
    }
    return std::nullopt;
}

}  // namespace stream_daemon
