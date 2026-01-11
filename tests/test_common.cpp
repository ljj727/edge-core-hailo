#include <gtest/gtest.h>

#include "common.h"

namespace stream_daemon {
namespace testing {

// ============================================================================
// StreamState Tests
// ============================================================================

TEST(StreamStateTest, StateToStringConversion) {
    EXPECT_EQ(StreamStateToString(StreamState::kStarting), "STARTING");
    EXPECT_EQ(StreamStateToString(StreamState::kRunning), "RUNNING");
    EXPECT_EQ(StreamStateToString(StreamState::kStopped), "STOPPED");
    EXPECT_EQ(StreamStateToString(StreamState::kError), "ERROR");
    EXPECT_EQ(StreamStateToString(StreamState::kReconnecting), "RECONNECTING");
}

TEST(StreamStateTest, StringToStateConversion) {
    EXPECT_EQ(StringToStreamState("STARTING"), StreamState::kStarting);
    EXPECT_EQ(StringToStreamState("RUNNING"), StreamState::kRunning);
    EXPECT_EQ(StringToStreamState("STOPPED"), StreamState::kStopped);
    EXPECT_EQ(StringToStreamState("ERROR"), StreamState::kError);
    EXPECT_EQ(StringToStreamState("RECONNECTING"), StreamState::kReconnecting);
    EXPECT_EQ(StringToStreamState("INVALID"), StreamState::kStopped);  // Default
}

TEST(StreamStateTest, RoundTripConversion) {
    for (auto state : {StreamState::kStarting, StreamState::kRunning,
                       StreamState::kStopped, StreamState::kError,
                       StreamState::kReconnecting}) {
        auto str = StreamStateToString(state);
        EXPECT_EQ(StringToStreamState(str), state);
    }
}

// ============================================================================
// Result Type Tests
// ============================================================================

TEST(ResultTest, OkResult) {
    Result<int> result = 42;

    EXPECT_TRUE(IsOk(result));
    EXPECT_FALSE(IsError(result));
    EXPECT_EQ(GetValue(result), 42);
}

TEST(ResultTest, ErrorResult) {
    Result<int> result = std::string("Something went wrong");

    EXPECT_FALSE(IsOk(result));
    EXPECT_TRUE(IsError(result));
    EXPECT_EQ(GetError(result), "Something went wrong");
}

TEST(ResultTest, VoidResult) {
    VoidResult ok = MakeOk();
    EXPECT_TRUE(IsOk(ok));
    EXPECT_FALSE(IsError(ok));

    VoidResult err = MakeError("Failed");
    EXPECT_FALSE(IsOk(err));
    EXPECT_TRUE(IsError(err));
    EXPECT_EQ(GetError(err), "Failed");
}

TEST(ResultTest, MoveSemantics) {
    Result<std::string> result = std::string("Hello World");

    EXPECT_TRUE(IsOk(result));
    std::string value = GetValue(std::move(result));
    EXPECT_EQ(value, "Hello World");
}

// ============================================================================
// Data Structure Tests
// ============================================================================

TEST(BoundingBoxTest, DefaultConstruction) {
    BoundingBox bbox;
    EXPECT_EQ(bbox.x, 0);
    EXPECT_EQ(bbox.y, 0);
    EXPECT_EQ(bbox.width, 0);
    EXPECT_EQ(bbox.height, 0);
}

TEST(DetectionTest, DefaultConstruction) {
    Detection det;
    EXPECT_TRUE(det.class_name.empty());
    EXPECT_EQ(det.class_id, 0);
    EXPECT_FLOAT_EQ(det.confidence, 0.0f);
}

TEST(StreamConfigTest, DefaultValues) {
    StreamConfig config;
    EXPECT_EQ(config.width, kDefaultWidth);
    EXPECT_EQ(config.height, kDefaultHeight);
    EXPECT_EQ(config.fps, kDefaultFps);
    EXPECT_FLOAT_EQ(config.confidence_threshold, kDefaultConfidenceThreshold);
}

TEST(StreamInfoTest, DefaultConstruction) {
    StreamInfo info;
    EXPECT_TRUE(info.stream_id.empty());
    EXPECT_TRUE(info.rtsp_url.empty());
    EXPECT_TRUE(info.hef_path.empty());
    EXPECT_EQ(info.config.width, kDefaultWidth);
}

TEST(StreamStatusTest, DefaultConstruction) {
    StreamStatus status;
    EXPECT_TRUE(status.stream_id.empty());
    EXPECT_EQ(status.state, StreamState::kStopped);
    EXPECT_EQ(status.frame_count, 0);
    EXPECT_DOUBLE_EQ(status.current_fps, 0.0);
}

TEST(DetectionEventTest, DefaultConstruction) {
    DetectionEvent event;
    EXPECT_TRUE(event.stream_id.empty());
    EXPECT_EQ(event.timestamp, 0);
    EXPECT_EQ(event.frame_number, 0);
    EXPECT_TRUE(event.detections.empty());
}

// ============================================================================
// Time Utility Tests
// ============================================================================

TEST(TimeUtilsTest, TimestampIsPositive) {
    auto ts_ms = GetCurrentTimestampMs();
    EXPECT_GT(ts_ms, 0);

    auto ts_s = GetCurrentTimestampSeconds();
    EXPECT_GT(ts_s, 0u);
}

TEST(TimeUtilsTest, TimestampsAreConsistent) {
    auto ts_ms = GetCurrentTimestampMs();
    auto ts_s = GetCurrentTimestampSeconds();

    // Milliseconds should be roughly 1000x seconds
    auto expected_ms = static_cast<int64_t>(ts_s) * 1000;
    EXPECT_NEAR(ts_ms, expected_ms, 1000);  // Within 1 second tolerance
}

// ============================================================================
// Constants Tests
// ============================================================================

TEST(ConstantsTest, DefaultValues) {
    EXPECT_EQ(kDefaultWidth, 1920);
    EXPECT_EQ(kDefaultHeight, 1080);
    EXPECT_EQ(kDefaultFps, 30);
    EXPECT_FLOAT_EQ(kDefaultConfidenceThreshold, 0.5f);
    EXPECT_EQ(kDefaultGrpcPort, 50051);
    EXPECT_EQ(kMaxStreams, 4);
}

TEST(ConstantsTest, NatsUrl) {
    EXPECT_EQ(kDefaultNatsUrl, "nats://localhost:4222");
}

}  // namespace testing
}  // namespace stream_daemon
