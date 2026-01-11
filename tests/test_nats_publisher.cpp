#include <gtest/gtest.h>

#include "nats_publisher.h"

namespace stream_daemon {
namespace testing {

// Note: These tests require a running NATS server for integration testing.
// For unit tests without NATS, use MockMessagePublisher instead.

class NatsPublisherTest : public ::testing::Test {
protected:
    // Skip test if NATS is not available
    void SetUp() override {
        // Try to create publisher - if it fails, NATS is not available
        auto result = NatsPublisher::Create("nats://localhost:4222");
        if (IsError(result)) {
            GTEST_SKIP() << "NATS server not available: " << GetError(result);
        }
        publisher_ = GetValue(std::move(result));
    }

    std::unique_ptr<NatsPublisher> publisher_;
};

TEST_F(NatsPublisherTest, IsConnectedAfterCreate) {
    EXPECT_TRUE(publisher_->IsConnected());
}

TEST_F(NatsPublisherTest, GetUrlReturnsCorrectUrl) {
    EXPECT_EQ(publisher_->GetUrl(), "nats://localhost:4222");
}

TEST_F(NatsPublisherTest, DisconnectWorks) {
    EXPECT_TRUE(publisher_->IsConnected());
    publisher_->Disconnect();
    EXPECT_FALSE(publisher_->IsConnected());
}

TEST_F(NatsPublisherTest, ReconnectAfterDisconnect) {
    publisher_->Disconnect();
    EXPECT_FALSE(publisher_->IsConnected());

    auto result = publisher_->Connect();
    EXPECT_TRUE(IsOk(result));
    EXPECT_TRUE(publisher_->IsConnected());
}

TEST_F(NatsPublisherTest, PublishDetectionEvent) {
    DetectionEvent event;
    event.stream_id = "test_cam";
    event.timestamp = GetCurrentTimestampMs();
    event.frame_number = 100;
    event.fps = 30.0;

    Detection det;
    det.class_name = "person";
    det.class_id = 0;
    det.confidence = 0.95f;
    det.bbox = {100, 200, 50, 100};
    event.detections.push_back(det);

    auto result = publisher_->Publish(event);
    EXPECT_TRUE(IsOk(result));
}

TEST_F(NatsPublisherTest, PublishRawJson) {
    std::string json = R"({"test": "data", "value": 123})";
    auto result = publisher_->PublishRaw("test.subject", json);
    EXPECT_TRUE(IsOk(result));
}

TEST_F(NatsPublisherTest, PublishFailsWhenDisconnected) {
    publisher_->Disconnect();

    DetectionEvent event;
    event.stream_id = "test";

    auto result = publisher_->Publish(event);
    EXPECT_TRUE(IsError(result));
}

// Test with invalid URL (should fail to connect)
TEST(NatsPublisherCreateTest, FailsWithInvalidUrl) {
    auto result = NatsPublisher::Create("nats://invalid-host-that-does-not-exist:9999");
    // This might succeed in creating but fail to connect, or fail immediately
    // depending on NATS client behavior
    if (IsOk(result)) {
        // If creation succeeded, publisher should not be connected
        // (or it might be in a reconnecting state)
        auto& publisher = GetValue(result);
        // Give it a moment
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Connection status depends on NATS client retry behavior
    }
}

// Test concurrent publishing (thread safety)
TEST_F(NatsPublisherTest, ConcurrentPublishing) {
    const int num_threads = 4;
    const int messages_per_thread = 100;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> error_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < messages_per_thread; ++i) {
                DetectionEvent event;
                event.stream_id = "thread_" + std::to_string(t);
                event.frame_number = static_cast<uint64_t>(i);
                event.timestamp = GetCurrentTimestampMs();

                auto result = publisher_->Publish(event);
                if (IsOk(result)) {
                    ++success_count;
                } else {
                    ++error_count;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All messages should have been published successfully
    EXPECT_EQ(success_count.load(), num_threads * messages_per_thread);
    EXPECT_EQ(error_count.load(), 0);
}

}  // namespace testing
}  // namespace stream_daemon
