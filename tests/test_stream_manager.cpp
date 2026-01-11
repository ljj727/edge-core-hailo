#include <gtest/gtest.h>

#include "mock_components.h"
#include "stream_manager.h"

namespace stream_daemon {
namespace testing {

// Note: Full StreamManager tests require GStreamer.
// These tests focus on the manager logic using mock components where possible.

class StreamManagerBasicTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Skip if NATS is not available (required for StreamManager creation)
        auto result = StreamManager::Create("nats://localhost:4222");
        if (IsError(result)) {
            GTEST_SKIP() << "Cannot create StreamManager: " << GetError(result);
        }
        manager_ = GetValue(std::move(result));
    }

    void TearDown() override {
        if (manager_) {
            manager_->Stop();
        }
    }

    std::unique_ptr<StreamManager> manager_;
};

TEST_F(StreamManagerBasicTest, InitialState) {
    EXPECT_FALSE(manager_->IsRunning());
    EXPECT_EQ(manager_->GetStreamCount(), 0);
}

TEST_F(StreamManagerBasicTest, StartAndStop) {
    manager_->Start();
    EXPECT_TRUE(manager_->IsRunning());

    manager_->Stop();
    EXPECT_FALSE(manager_->IsRunning());
}

TEST_F(StreamManagerBasicTest, GetNatsPublisher) {
    auto publisher = manager_->GetNatsPublisher();
    EXPECT_NE(publisher, nullptr);
    EXPECT_TRUE(publisher->IsConnected());
}

TEST_F(StreamManagerBasicTest, AddStreamValidation) {
    manager_->Start();

    // Empty stream ID should fail
    StreamInfo info;
    info.rtsp_url = "rtsp://localhost/test";
    info.hef_path = "/model.hef";

    auto result = manager_->AddStream(info);
    EXPECT_TRUE(IsError(result));
}

TEST_F(StreamManagerBasicTest, HasStreamReturnsFalseForNonExistent) {
    EXPECT_FALSE(manager_->HasStream("nonexistent"));
}

TEST_F(StreamManagerBasicTest, GetStatusReturnsNulloptForNonExistent) {
    auto status = manager_->GetStreamStatus("nonexistent");
    EXPECT_FALSE(status.has_value());
}

TEST_F(StreamManagerBasicTest, GetAllStatusReturnsEmptyInitially) {
    auto statuses = manager_->GetAllStreamStatus();
    EXPECT_TRUE(statuses.empty());
}

TEST_F(StreamManagerBasicTest, RemoveNonExistentStreamFails) {
    auto result = manager_->RemoveStream("nonexistent");
    EXPECT_TRUE(IsError(result));
}

TEST_F(StreamManagerBasicTest, UpdateNonExistentStreamFails) {
    StreamInfo info;
    info.stream_id = "nonexistent";
    info.rtsp_url = "rtsp://localhost/test";

    auto result = manager_->UpdateStream(info);
    EXPECT_TRUE(IsError(result));
}

// Tests with actual stream operations (may fail without proper RTSP source)
class StreamManagerIntegrationTest : public StreamManagerBasicTest {
protected:
    // These tests require a working RTSP server and Hailo setup
    // They are more integration tests than unit tests
};

TEST_F(StreamManagerIntegrationTest, DISABLED_AddAndRemoveStream) {
    // Disabled by default as it requires actual RTSP source
    manager_->Start();

    StreamInfo info;
    info.stream_id = "test_cam";
    info.rtsp_url = "rtsp://localhost:8554/test";
    info.hef_path = "/path/to/model.hef";

    auto add_result = manager_->AddStream(info);
    if (IsError(add_result)) {
        GTEST_SKIP() << "Failed to add stream: " << GetError(add_result);
    }

    EXPECT_TRUE(manager_->HasStream("test_cam"));
    EXPECT_EQ(manager_->GetStreamCount(), 1);

    auto status = manager_->GetStreamStatus("test_cam");
    EXPECT_TRUE(status.has_value());
    EXPECT_EQ(status->stream_id, "test_cam");

    auto remove_result = manager_->RemoveStream("test_cam");
    EXPECT_TRUE(IsOk(remove_result));
    EXPECT_FALSE(manager_->HasStream("test_cam"));
    EXPECT_EQ(manager_->GetStreamCount(), 0);
}

TEST_F(StreamManagerIntegrationTest, DISABLED_MaxStreamsLimit) {
    // Disabled by default as it requires actual RTSP source
    manager_->Start();

    // Try to add more than max streams
    for (int i = 0; i <= kMaxStreams; ++i) {
        StreamInfo info;
        info.stream_id = "cam_" + std::to_string(i);
        info.rtsp_url = "rtsp://localhost:8554/stream" + std::to_string(i);
        info.hef_path = "/path/to/model.hef";

        auto result = manager_->AddStream(info);

        if (i < kMaxStreams) {
            // First kMaxStreams should succeed
            if (IsError(result)) {
                // May fail due to actual stream issues, not limit
                break;
            }
        } else {
            // Should fail due to limit
            EXPECT_TRUE(IsError(result));
        }
    }
}

// Callback tests
TEST_F(StreamManagerBasicTest, GlobalCallbacksAreSet) {
    bool detection_called = false;
    bool state_called = false;
    bool error_called = false;

    manager_->SetGlobalDetectionCallback([&](const DetectionEvent&) {
        detection_called = true;
    });

    manager_->SetGlobalStateChangeCallback([&](std::string_view, StreamState) {
        state_called = true;
    });

    manager_->SetGlobalErrorCallback([&](std::string_view, std::string_view) {
        error_called = true;
    });

    // Callbacks are set, but won't be called until streams are added
    // This just verifies the setters don't crash
    SUCCEED();
}

// Mock-based tests for testing manager logic without GStreamer
class MockStreamManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_publisher_ = std::make_shared<MockMessagePublisher>();
        mock_publisher_->Connect();
    }

    std::shared_ptr<MockMessagePublisher> mock_publisher_;
};

TEST_F(MockStreamManagerTest, MockPublisherReceivesEvents) {
    // Create a mock processor
    StreamInfo info;
    info.stream_id = "mock_stream";
    info.rtsp_url = "rtsp://mock/test";
    info.hef_path = "/mock/model.hef";

    MockStreamProcessor processor(info);
    processor.Start();

    // Simulate detection
    DetectionEvent event;
    event.stream_id = "mock_stream";
    event.frame_number = 1;
    event.timestamp = GetCurrentTimestampMs();

    Detection det;
    det.class_name = "car";
    det.confidence = 0.9f;
    event.detections.push_back(det);

    // Manually publish (simulating what StreamProcessor would do)
    mock_publisher_->Publish(event);

    auto events = mock_publisher_->GetPublishedEvents();
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].stream_id, "mock_stream");
    EXPECT_EQ(events[0].detections.size(), 1);
    EXPECT_EQ(events[0].detections[0].class_name, "car");
}

}  // namespace testing
}  // namespace stream_daemon
