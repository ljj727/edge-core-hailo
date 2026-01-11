#include <gtest/gtest.h>

#include "mock_components.h"

namespace stream_daemon {
namespace testing {

// ============================================================================
// MockMessagePublisher Tests
// ============================================================================

class MockMessagePublisherTest : public ::testing::Test {
protected:
    MockMessagePublisher publisher;
};

TEST_F(MockMessagePublisherTest, InitiallyDisconnected) {
    EXPECT_FALSE(publisher.IsConnected());
}

TEST_F(MockMessagePublisherTest, ConnectSucceeds) {
    auto result = publisher.Connect();
    EXPECT_TRUE(IsOk(result));
    EXPECT_TRUE(publisher.IsConnected());
}

TEST_F(MockMessagePublisherTest, DisconnectWorks) {
    publisher.Connect();
    EXPECT_TRUE(publisher.IsConnected());

    publisher.Disconnect();
    EXPECT_FALSE(publisher.IsConnected());
}

TEST_F(MockMessagePublisherTest, PublishFailsWhenDisconnected) {
    DetectionEvent event;
    event.stream_id = "test";

    auto result = publisher.Publish(event);
    EXPECT_TRUE(IsError(result));
}

TEST_F(MockMessagePublisherTest, PublishSucceedsWhenConnected) {
    publisher.Connect();

    DetectionEvent event;
    event.stream_id = "test";
    event.timestamp = 12345;
    event.frame_number = 100;

    auto result = publisher.Publish(event);
    EXPECT_TRUE(IsOk(result));

    auto events = publisher.GetPublishedEvents();
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].stream_id, "test");
    EXPECT_EQ(events[0].timestamp, 12345);
    EXPECT_EQ(events[0].frame_number, 100);
}

TEST_F(MockMessagePublisherTest, PublishRawWorks) {
    publisher.Connect();

    auto result = publisher.PublishRaw("subject.test", R"({"key": "value"})");
    EXPECT_TRUE(IsOk(result));

    auto messages = publisher.GetRawMessages();
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(messages[0].first, "subject.test");
    EXPECT_EQ(messages[0].second, R"({"key": "value"})");
}

TEST_F(MockMessagePublisherTest, ClearRemovesAllMessages) {
    publisher.Connect();

    DetectionEvent event;
    event.stream_id = "test";
    publisher.Publish(event);
    publisher.PublishRaw("subject", "data");

    EXPECT_EQ(publisher.GetEventCount(), 1);
    EXPECT_EQ(publisher.GetRawMessages().size(), 1);

    publisher.Clear();

    EXPECT_EQ(publisher.GetEventCount(), 0);
    EXPECT_EQ(publisher.GetRawMessages().size(), 0);
}

TEST_F(MockMessagePublisherTest, MultiplePublishes) {
    publisher.Connect();

    for (int i = 0; i < 10; ++i) {
        DetectionEvent event;
        event.stream_id = "stream_" + std::to_string(i);
        event.frame_number = static_cast<uint64_t>(i);
        publisher.Publish(event);
    }

    EXPECT_EQ(publisher.GetEventCount(), 10);

    auto events = publisher.GetPublishedEvents();
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(events[i].stream_id, "stream_" + std::to_string(i));
        EXPECT_EQ(events[i].frame_number, static_cast<uint64_t>(i));
    }
}

// ============================================================================
// MockStreamProcessor Tests
// ============================================================================

class MockStreamProcessorTest : public ::testing::Test {
protected:
    void SetUp() override {
        StreamInfo info;
        info.stream_id = "test_stream";
        info.rtsp_url = "rtsp://localhost/test";
        info.hef_path = "/path/to/model.hef";
        info.config.fps = 25;

        processor = std::make_unique<MockStreamProcessor>(info);
    }

    std::unique_ptr<MockStreamProcessor> processor;
};

TEST_F(MockStreamProcessorTest, InitialState) {
    EXPECT_EQ(processor->GetStreamId(), "test_stream");
    EXPECT_EQ(processor->GetState(), StreamState::kStopped);
    EXPECT_FALSE(processor->IsRunning());
}

TEST_F(MockStreamProcessorTest, StartSucceeds) {
    auto result = processor->Start();
    EXPECT_TRUE(IsOk(result));
    EXPECT_EQ(processor->GetState(), StreamState::kRunning);
    EXPECT_TRUE(processor->IsRunning());
}

TEST_F(MockStreamProcessorTest, StartCanFail) {
    processor->SetShouldFailStart(true, "Test failure");

    auto result = processor->Start();
    EXPECT_TRUE(IsError(result));
    EXPECT_EQ(GetError(result), "Test failure");
    EXPECT_NE(processor->GetState(), StreamState::kRunning);
}

TEST_F(MockStreamProcessorTest, StopWorks) {
    processor->Start();
    EXPECT_TRUE(processor->IsRunning());

    processor->Stop();
    EXPECT_FALSE(processor->IsRunning());
    EXPECT_EQ(processor->GetState(), StreamState::kStopped);
}

TEST_F(MockStreamProcessorTest, UpdateWorks) {
    processor->Start();

    StreamInfo new_info;
    new_info.stream_id = "test_stream";
    new_info.rtsp_url = "rtsp://new-url/stream";
    new_info.config.fps = 60;

    auto result = processor->Update(new_info);
    EXPECT_TRUE(IsOk(result));
}

TEST_F(MockStreamProcessorTest, GetStatusReturnsCorrectInfo) {
    processor->Start();

    auto status = processor->GetStatus();
    EXPECT_EQ(status.stream_id, "test_stream");
    EXPECT_EQ(status.rtsp_url, "rtsp://localhost/test");
    EXPECT_EQ(status.state, StreamState::kRunning);
}

TEST_F(MockStreamProcessorTest, SimulateDetectionCallsCallback) {
    bool callback_called = false;
    DetectionEvent received_event;

    processor->SetDetectionCallback([&](const DetectionEvent& event) {
        callback_called = true;
        received_event = event;
    });

    processor->Start();

    DetectionEvent event;
    event.stream_id = "test_stream";
    event.frame_number = 42;
    event.detections.push_back(Detection{"person", 0, 0.95f, {10, 20, 100, 200}});

    processor->SimulateDetection(event);

    EXPECT_TRUE(callback_called);
    EXPECT_EQ(received_event.stream_id, "test_stream");
    EXPECT_EQ(received_event.frame_number, 42);
    EXPECT_EQ(received_event.detections.size(), 1);
}

TEST_F(MockStreamProcessorTest, SimulateErrorCallsCallback) {
    bool error_callback_called = false;
    std::string received_error;

    processor->SetErrorCallback([&](std::string_view stream_id, std::string_view error) {
        error_callback_called = true;
        received_error = std::string(error);
    });

    processor->Start();
    processor->SimulateError("Connection lost");

    EXPECT_TRUE(error_callback_called);
    EXPECT_EQ(received_error, "Connection lost");
    EXPECT_EQ(processor->GetState(), StreamState::kError);
}

TEST_F(MockStreamProcessorTest, SimulateFpsUpdatesStatus) {
    processor->Start();
    processor->SimulateFps(29.5);

    auto status = processor->GetStatus();
    EXPECT_DOUBLE_EQ(status.current_fps, 29.5);
}

// ============================================================================
// MockStreamProcessorFactory Tests
// ============================================================================

TEST(MockStreamProcessorFactoryTest, CreateReturnsProcessor) {
    MockStreamProcessorFactory factory;
    auto mock_publisher = std::make_shared<MockMessagePublisher>();

    StreamInfo info;
    info.stream_id = "factory_test";
    info.rtsp_url = "rtsp://test/stream";
    info.hef_path = "/model.hef";

    auto result = factory.Create(info, mock_publisher);
    EXPECT_TRUE(IsOk(result));

    auto& processor = GetValue(result);
    EXPECT_EQ(processor->GetStreamId(), "factory_test");
}

TEST(MockStreamProcessorFactoryTest, TracksCreatedProcessors) {
    MockStreamProcessorFactory factory;
    auto mock_publisher = std::make_shared<MockMessagePublisher>();

    // Create multiple processors
    for (int i = 0; i < 3; ++i) {
        StreamInfo info;
        info.stream_id = "stream_" + std::to_string(i);
        info.rtsp_url = "rtsp://test/stream" + std::to_string(i);
        info.hef_path = "/model.hef";

        factory.Create(info, mock_publisher);
    }

    auto processors = factory.GetCreatedProcessors();
    EXPECT_EQ(processors.size(), 3);
}

TEST(MockStreamProcessorFactoryTest, ClearRemovesTracking) {
    MockStreamProcessorFactory factory;
    auto mock_publisher = std::make_shared<MockMessagePublisher>();

    StreamInfo info;
    info.stream_id = "test";
    info.rtsp_url = "rtsp://test/stream";
    info.hef_path = "/model.hef";

    factory.Create(info, mock_publisher);
    EXPECT_EQ(factory.GetCreatedProcessors().size(), 1);

    factory.Clear();
    EXPECT_EQ(factory.GetCreatedProcessors().size(), 0);
}

}  // namespace testing
}  // namespace stream_daemon
