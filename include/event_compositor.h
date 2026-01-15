#ifndef STREAM_DAEMON_EVENT_COMPOSITOR_H_
#define STREAM_DAEMON_EVENT_COMPOSITOR_H_

#include "common.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace stream_daemon {

// 2D 점 (정규화된 좌표 0.0~1.0)
struct Point2D {
    float x{0.0f};
    float y{0.0f};
};

// 이벤트 타입
enum class EventType {
    kROI,       // 영역 감지
    kLine,      // 라인 크로싱
    kAnd,       // 논리 AND
    kOr,        // 논리 OR
    kSpeed,     // 속도 감지
    kHM,        // 히트맵
    kFilter,    // 필터
    kEnEx,      // 입출 카운팅
    kAlarm,     // 알람
    kUnknown
};

// Detection Point (객체 기준점)
enum class DetectionPoint {
    kLeftTop,
    kCenterTop,
    kRightTop,
    kLeftCenter,
    kCenter,
    kRightCenter,
    kLeftBottom,
    kCenterBottom,  // 기본값 (발 위치)
    kRightBottom
};

// 라인 방향
enum class LineDirection {
    kA2B,   // A→B
    kB2A,   // B→A
    kBoth   // 양방향
};

// 타겟 필터
struct TargetFilter {
    std::vector<std::string> labels;      // ["RV", "General"] 등 (복수)
    std::string class_type;               // classifier 타입
    std::vector<std::string> result_label; // 세부 라벨
};

// 이벤트 설정
struct EventSetting {
    std::string event_setting_id;
    std::string event_setting_name;
    EventType event_type{EventType::kUnknown};
    std::string parent_id;                // 부모 이벤트 ID

    // ROI/Line/Filter용 폴리곤/라인 좌표
    std::vector<Point2D> points;

    // 타겟 필터
    TargetFilter target;

    // ROI 옵션
    float timeout{0.0f};                  // 체류 시간 조건 (초)
    DetectionPoint detection_point{DetectionPoint::kCenterBottom};

    // Line 옵션
    LineDirection direction{LineDirection::kBoth};
    std::vector<int> keypoints;           // 감지할 키포인트 인덱스 [1, 2] 등
    float warning_distance{0.1f};         // WARNING 영역 거리 (정규화 좌표)

    // And/Or 옵션
    bool in_order{false};
    std::string ncond;                    // ">=2" 등

    // Speed 옵션
    int turn{0};

    // HM 옵션
    float regen_interval{60.0f};

    // Alarm 옵션
    std::string ext;

    // 자식 이벤트 ID 목록 (런타임에 구성)
    std::vector<std::string> children;
};

// Line 이벤트 결과
struct LineEventResult {
    int status{0};                        // 0=SAFE, 1=WARNING, 2=DANGER
    std::vector<std::string> labels;      // 해당 라벨들
};

/**
 * @brief EventCompositor - 이벤트 설정 관리 및 감지
 */
class EventCompositor {
public:
    EventCompositor() = default;
    ~EventCompositor() = default;

    // Non-copyable
    EventCompositor(const EventCompositor&) = delete;
    EventCompositor& operator=(const EventCompositor&) = delete;

    /**
     * @brief 이벤트 설정 업데이트 (JSON 파싱)
     * @return 터미널 이벤트 ID 목록
     */
    [[nodiscard]] Result<std::vector<std::string>> UpdateSettings(
        const std::string& settings_json);

    /**
     * @brief 모든 이벤트 설정 제거
     */
    void ClearSettings();

    /**
     * @brief 감지 결과로 이벤트 체크 (각 detection에 event_setting_id 태깅)
     * @param detections 현재 프레임 감지 결과 (이벤트 발생 시 event_setting_id 설정됨)
     * @param frame_width 프레임 너비
     * @param frame_height 프레임 높이
     */
    void CheckEvents(
        std::vector<Detection>& detections,
        int frame_width,
        int frame_height);

    /**
     * @brief Line 이벤트 체크 (키포인트 기반)
     * @param detections 현재 프레임 감지 결과
     * @param frame_width 프레임 너비
     * @param frame_height 프레임 높이
     * @return event_setting_id -> LineEventResult 맵
     */
    [[nodiscard]] std::unordered_map<std::string, LineEventResult> CheckLineEvents(
        const std::vector<Detection>& detections,
        int frame_width,
        int frame_height);

    /**
     * @brief 이벤트 설정 개수
     */
    [[nodiscard]] size_t GetSettingCount() const;

    /**
     * @brief 특정 이벤트 설정 조회
     */
    [[nodiscard]] std::optional<EventSetting> GetSetting(
        const std::string& event_setting_id) const;

private:
    /**
     * @brief JSON에서 이벤트 설정 파싱
     */
    [[nodiscard]] VoidResult ParseSettings(const std::string& json);

    /**
     * @brief 이벤트 트리 구성 (parent-child 관계 설정)
     */
    void BuildEventTree();

    /**
     * @brief 터미널 이벤트 식별 (자식이 없는 이벤트)
     */
    [[nodiscard]] std::vector<std::string> FindTerminalEvents() const;

    /**
     * @brief 점이 폴리곤 내부에 있는지 확인
     */
    [[nodiscard]] bool IsPointInPolygon(
        const Point2D& point,
        const std::vector<Point2D>& polygon) const;

    /**
     * @brief detection에서 기준점 좌표 계산
     */
    [[nodiscard]] Point2D GetDetectionPoint(
        const Detection& det,
        DetectionPoint dp,
        int frame_width,
        int frame_height) const;

    /**
     * @brief ROI 이벤트 체크
     */
    [[nodiscard]] bool CheckROIEvent(
        const EventSetting& setting,
        const Detection& det,
        int frame_width,
        int frame_height) const;

    /**
     * @brief 타겟 필터 매칭
     */
    [[nodiscard]] bool MatchesTarget(
        const Detection& det,
        const TargetFilter& target) const;

    /**
     * @brief Line 이벤트 체크 (단일 detection)
     * @return 0=SAFE, 1=WARNING, 2=DANGER
     */
    [[nodiscard]] int CheckLineEvent(
        const EventSetting& setting,
        const Detection& det,
        int frame_width,
        int frame_height) const;

    /**
     * @brief 점과 직선 사이의 수직 거리 계산
     */
    [[nodiscard]] float PointToLineDistance(
        const Point2D& point,
        const Point2D& line_a,
        const Point2D& line_b) const;

    /**
     * @brief 점이 직선의 어느 쪽에 있는지 판별
     * @return >0: A->B 방향 기준 왼쪽, <0: 오른쪽, =0: 선 위
     */
    [[nodiscard]] float PointLineSide(
        const Point2D& point,
        const Point2D& line_a,
        const Point2D& line_b) const;

    // 이벤트 설정 저장 (event_setting_id -> EventSetting)
    std::unordered_map<std::string, EventSetting> settings_;
    mutable std::mutex mutex_;

    // 터미널 이벤트 ID 캐시
    std::vector<std::string> terminal_events_;
};

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_EVENT_COMPOSITOR_H_
