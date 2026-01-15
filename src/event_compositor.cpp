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

            // targets 파싱
            if (config.contains("targets")) {
                if (config["targets"].is_array()) {
                    // 배열: ["RV", "General"] 또는 ["ALL"]
                    for (const auto& t : config["targets"]) {
                        if (t.is_string()) {
                            std::string val = t.get<std::string>();
                            if (val == "ALL" || val == "all") {
                                // "ALL"이면 전체 대상 → labels 비우고 종료
                                setting.target.labels.clear();
                                break;
                            }
                            setting.target.labels.push_back(val);
                        }
                    }
                } else if (config["targets"].is_string()) {
                    // 문자열: "ALL" → 빈 배열 (전체 대상)
                    std::string val = config["targets"].get<std::string>();
                    if (val != "ALL" && val != "all") {
                        setting.target.labels.push_back(val);
                    }
                }
            }
            // 하위 호환: 단일 target 객체도 지원
            if (config.contains("target") && config["target"].is_object()) {
                const auto& target = config["target"];
                if (target.contains("label") && !target["label"].is_null()) {
                    setting.target.labels.push_back(target["label"].get<std::string>());
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
            if (config.contains("keypoints") && config["keypoints"].is_array()) {
                for (const auto& kp : config["keypoints"]) {
                    setting.keypoints.push_back(kp.get<int>());
                }
            }
            if (config.contains("warningDistance")) {
                setting.warning_distance = config["warningDistance"].get<float>();
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
    if (target.labels.empty()) {
        return true;
    }

    // 라벨 매칭 (대소문자 무시)
    std::string det_label = det.class_name;
    std::transform(det_label.begin(), det_label.end(), det_label.begin(), ::tolower);

    for (const auto& target_label : target.labels) {
        std::string lower_target = target_label;
        std::transform(lower_target.begin(), lower_target.end(), lower_target.begin(), ::tolower);
        if (det_label == lower_target) {
            return true;
        }
    }

    return false;
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

// ============================================================================
// Line Event Detection
// ============================================================================

float EventCompositor::PointToLineDistance(
    const Point2D& point,
    const Point2D& line_a,
    const Point2D& line_b) const {

    // 점과 직선 사이의 수직 거리 공식
    // |((y2-y1)*px - (x2-x1)*py + x2*y1 - y2*x1)| / sqrt((y2-y1)² + (x2-x1)²)
    float dx = line_b.x - line_a.x;
    float dy = line_b.y - line_a.y;

    float numerator = std::abs(dy * point.x - dx * point.y +
                               line_b.x * line_a.y - line_b.y * line_a.x);
    float denominator = std::sqrt(dy * dy + dx * dx);

    if (denominator < 1e-6f) {
        // 라인이 점인 경우 (두 점이 같은 위치)
        float px = point.x - line_a.x;
        float py = point.y - line_a.y;
        return std::sqrt(px * px + py * py);
    }

    return numerator / denominator;
}

float EventCompositor::PointLineSide(
    const Point2D& point,
    const Point2D& line_a,
    const Point2D& line_b) const {

    // Cross product로 점이 직선의 어느 쪽에 있는지 판별
    // (B - A) × (P - A) = (bx-ax)(py-ay) - (by-ay)(px-ax)
    // 양수: 왼쪽 (A->B 기준), 음수: 오른쪽
    return (line_b.x - line_a.x) * (point.y - line_a.y) -
           (line_b.y - line_a.y) * (point.x - line_a.x);
}

int EventCompositor::CheckLineEvent(
    const EventSetting& setting,
    const Detection& det,
    int frame_width,
    int frame_height) const {

    // 타겟 필터 매칭 확인
    if (!MatchesTarget(det, setting.target)) {
        return 0;  // SAFE - 대상 아님
    }

    // 라인 좌표 필요 (2점)
    if (setting.points.size() < 2) {
        return 0;
    }

    const Point2D& line_a = setting.points[0];
    const Point2D& line_b = setting.points[1];

    // 키포인트가 없거나 설정된 키포인트 인덱스가 없으면 bbox 기준점 사용
    if (det.keypoints.empty() || setting.keypoints.empty()) {
        Point2D point = GetDetectionPoint(det, setting.detection_point,
                                          frame_width, frame_height);

        float distance = PointToLineDistance(point, line_a, line_b);
        float side = PointLineSide(point, line_a, line_b);

        // direction에 따른 판정 (화면 좌표계: Y축 아래로 증가)
        // A2B: A→B 벡터 기준 오른쪽이 danger (side > 0)
        // B2A: A→B 벡터 기준 왼쪽이 danger (side < 0)
        bool on_danger_side = false;
        if (setting.direction == LineDirection::kA2B) {
            on_danger_side = (side > 0);  // 화면좌표계 보정
        } else if (setting.direction == LineDirection::kB2A) {
            on_danger_side = (side < 0);  // 화면좌표계 보정
        } else {
            // BOTH: 라인 근처면 WARNING, 아무 쪽이든 넘어가면 DANGER
            if (distance < setting.warning_distance) {
                return 1;  // WARNING
            }
            return 0;  // SAFE (BOTH에서는 DANGER 없음)
        }

        if (on_danger_side) {
            return 2;  // DANGER - 라인 넘어감
        } else if (distance < setting.warning_distance) {
            return 1;  // WARNING - 라인 근처
        }
        return 0;  // SAFE
    }

    // 키포인트 기반 판정
    int max_status = 0;

    for (int kp_idx : setting.keypoints) {
        // 키포인트 인덱스 범위 체크 (0-based)
        if (kp_idx < 0 || kp_idx >= static_cast<int>(det.keypoints.size())) {
            continue;
        }
        int idx = kp_idx;

        const auto& kp = det.keypoints[idx];

        // visibility 체크 (너무 낮으면 스킵)
        if (kp.visible < 0.3f) {
            continue;
        }

        // 키포인트 좌표 (이미 정규화된 상태)
        Point2D point{kp.x, kp.y};

        float distance = PointToLineDistance(point, line_a, line_b);
        float side = PointLineSide(point, line_a, line_b);

        // Debug: Line event calculation
        static int line_debug_count = 0;
        if (line_debug_count++ < 30) {
            LogInfo("Line[" + setting.event_setting_id + "] kp" + std::to_string(kp_idx) +
                    ": pos=(" + std::to_string(point.x) + "," + std::to_string(point.y) +
                    ") dist=" + std::to_string(distance) +
                    " warn=" + std::to_string(setting.warning_distance) +
                    " side=" + std::to_string(side));
        }

        int status = 0;

        // direction에 따른 판정 (화면 좌표계: Y축 아래로 증가)
        // A2B: A→B 벡터 기준 오른쪽이 danger (side > 0)
        // B2A: A→B 벡터 기준 왼쪽이 danger (side < 0)
        bool on_danger_side = false;
        if (setting.direction == LineDirection::kA2B) {
            on_danger_side = (side > 0);  // 화면좌표계 보정
        } else if (setting.direction == LineDirection::kB2A) {
            on_danger_side = (side < 0);  // 화면좌표계 보정
        } else {
            // BOTH
            if (distance < setting.warning_distance) {
                status = 1;
            }
            max_status = std::max(max_status, status);
            continue;
        }

        if (on_danger_side) {
            status = 2;  // DANGER
        } else if (distance < setting.warning_distance) {
            status = 1;  // WARNING
        }

        max_status = std::max(max_status, status);

        // DANGER면 더 체크할 필요 없음
        if (max_status == 2) {
            break;
        }
    }

    return max_status;
}

std::unordered_map<std::string, LineEventResult> EventCompositor::CheckLineEvents(
    const std::vector<Detection>& detections,
    int frame_width,
    int frame_height) {

    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_map<std::string, LineEventResult> results;

    if (settings_.empty() || detections.empty()) {
        return results;
    }

    // 모든 Line 이벤트에 대해 체크
    for (const auto& [id, setting] : settings_) {
        if (setting.event_type != EventType::kLine) {
            continue;
        }

        LineEventResult result;
        result.status = 0;  // SAFE

        // 모든 detection에 대해 체크
        for (const auto& det : detections) {
            int status = CheckLineEvent(setting, det, frame_width, frame_height);

            if (status > result.status) {
                result.status = status;
            }

            // 이 detection이 이벤트에 해당하면 라벨 추가
            if (status > 0) {
                // 중복 체크
                auto it = std::find(result.labels.begin(), result.labels.end(), det.class_name);
                if (it == result.labels.end()) {
                    result.labels.push_back(det.class_name);
                }
            }
        }

        results[id] = std::move(result);
    }

    return results;
}

}  // namespace stream_daemon
