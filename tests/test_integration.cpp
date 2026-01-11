#include <gtest/gtest.h>

#include "common.h"
#include "debug_utils.h"
#include "mock_components.h"

#include <gst/gst.h>

#include <chrono>
#include <thread>

namespace stream_daemon {
namespace testing {

/**
 * @brief Test fixture for GStreamer integration tests
 */
class GStreamerIntegrationTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Initialize GStreamer once for all tests
        gst_init(nullptr, nullptr);
    }

    static void TearDownTestSuite() {
        gst_deinit();
    }
};

// ============================================================================
// GStreamer Basic Tests
// ============================================================================

TEST_F(GStreamerIntegrationTest, GStreamerInitialized) {
    EXPECT_TRUE(gst_is_initialized());
}

TEST_F(GStreamerIntegrationTest, RequiredElementsAvailable) {
    const char* required[] = {
        "videotestsrc", "videoconvert", "appsink", "fakesink"
    };

    for (const char* element_name : required) {
        GstElementFactory* factory = gst_element_factory_find(element_name);
        EXPECT_NE(factory, nullptr) << "Element not found: " << element_name;
        if (factory) {
            gst_object_unref(factory);
        }
    }
}

TEST_F(GStreamerIntegrationTest, DISABLED_HailoElementsAvailable) {
    // Disabled by default - only run on systems with Hailo
    bool hailo_available = debug::GStreamerDebug::CheckHailoPlugins();
    EXPECT_TRUE(hailo_available);
}

// ============================================================================
// Pipeline Validation Tests
// ============================================================================

TEST_F(GStreamerIntegrationTest, TestPipelineIsValid) {
    auto pipeline_str = debug::TestPipelineBuilder::BuildTestPipeline(true);
    auto result = debug::TestPipelineBuilder::ValidatePipeline(pipeline_str);
    EXPECT_TRUE(IsOk(result)) << "Failed: " << (IsError(result) ? GetError(result) : "");
}

TEST_F(GStreamerIntegrationTest, InvalidPipelineFails) {
    auto result = debug::TestPipelineBuilder::ValidatePipeline(
        "nonexistent_element ! fakesink");
    EXPECT_TRUE(IsError(result));
}

TEST_F(GStreamerIntegrationTest, TestPipelineRuns) {
    auto pipeline_str = debug::TestPipelineBuilder::BuildTestPipeline(true);

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_EQ(error, nullptr);

    // Set to playing
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    EXPECT_NE(ret, GST_STATE_CHANGE_FAILURE);

    // Let it run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Check state
    GstState state;
    ret = gst_element_get_state(pipeline, &state, nullptr, GST_SECOND);
    EXPECT_EQ(ret, GST_STATE_CHANGE_SUCCESS);
    EXPECT_EQ(state, GST_STATE_PLAYING);

    // Cleanup
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// ============================================================================
// Debug Utility Tests
// ============================================================================

TEST_F(GStreamerIntegrationTest, GetPipelineState) {
    auto pipeline_str = debug::TestPipelineBuilder::BuildTestPipeline(true);
    GstElement* pipeline = gst_parse_launch(pipeline_str.c_str(), nullptr);
    ASSERT_NE(pipeline, nullptr);

    // Initially NULL
    auto state_str = debug::GStreamerDebug::GetPipelineState(pipeline);
    EXPECT_FALSE(state_str.empty());

    // Set to ready
    gst_element_set_state(pipeline, GST_STATE_READY);
    gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND);

    state_str = debug::GStreamerDebug::GetPipelineState(pipeline);
    EXPECT_NE(state_str.find("READY"), std::string::npos);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

TEST_F(GStreamerIntegrationTest, ListPipelineElements) {
    auto pipeline_str = debug::TestPipelineBuilder::BuildTestPipeline(true);
    GstElement* pipeline = gst_parse_launch(pipeline_str.c_str(), nullptr);
    ASSERT_NE(pipeline, nullptr);

    auto elements_str = debug::GStreamerDebug::ListPipelineElements(pipeline);
    EXPECT_FALSE(elements_str.empty());
    EXPECT_NE(elements_str.find("videotestsrc"), std::string::npos);
    EXPECT_NE(elements_str.find("fakesink"), std::string::npos);

    gst_object_unref(pipeline);
}

// ============================================================================
// Performance Profiler Tests
// ============================================================================

TEST(PerformanceProfilerTest, RecordsStats) {
    debug::PerformanceProfiler profiler;

    // Record some frames
    for (int i = 0; i < 100; ++i) {
        double fps = 28.0 + (i % 5);  // 28-32 fps
        double latency = 10.0 + (i % 10);  // 10-19 ms
        profiler.RecordFrame(fps, latency, i % 20 == 0);  // 5% dropped
    }

    auto stats = profiler.GetStats();
    EXPECT_EQ(stats.total_frames, 100u);
    EXPECT_EQ(stats.dropped_frames, 5u);
    EXPECT_GE(stats.avg_fps, 28.0);
    EXPECT_LE(stats.avg_fps, 32.0);
    EXPECT_GE(stats.min_fps, 28.0);
    EXPECT_LE(stats.max_fps, 32.0);
    EXPECT_GE(stats.avg_latency_ms, 10.0);
    EXPECT_LE(stats.avg_latency_ms, 19.0);
}

TEST(PerformanceProfilerTest, ResetClearsStats) {
    debug::PerformanceProfiler profiler;

    profiler.RecordFrame(30.0, 10.0, false);
    profiler.RecordFrame(30.0, 10.0, false);

    auto stats = profiler.GetStats();
    EXPECT_EQ(stats.total_frames, 2u);

    profiler.Reset();

    stats = profiler.GetStats();
    EXPECT_EQ(stats.total_frames, 0u);
}

TEST(PerformanceProfilerTest, GeneratesReport) {
    debug::PerformanceProfiler profiler;

    profiler.RecordFrame(30.0, 15.0, false);
    profiler.RecordFrame(29.5, 16.0, false);

    auto report = profiler.GetReport();
    EXPECT_FALSE(report.empty());
    EXPECT_NE(report.find("Total frames"), std::string::npos);
    EXPECT_NE(report.find("FPS"), std::string::npos);
    EXPECT_NE(report.find("latency"), std::string::npos);
}

// ============================================================================
// End-to-End Mock Test
// ============================================================================

TEST(EndToEndMockTest, FullPipelineMock) {
    // Create mock components
    auto mock_publisher = std::make_shared<MockMessagePublisher>();
    mock_publisher->Connect();

    MockStreamProcessorFactory factory;

    StreamInfo info;
    info.stream_id = "e2e_test";
    info.rtsp_url = "rtsp://mock/stream";
    info.hef_path = "/mock/model.hef";
    info.config.fps = 30;

    // Create processor via factory
    auto result = factory.Create(info, mock_publisher);
    ASSERT_TRUE(IsOk(result));

    auto processor = std::move(GetValue(result));

    // Start processor
    auto start_result = processor->Start();
    ASSERT_TRUE(IsOk(start_result));
    EXPECT_TRUE(processor->IsRunning());

    // Get the mock processor for simulation
    auto created = factory.GetCreatedProcessors();
    ASSERT_EQ(created.size(), 1);
    auto* mock_processor = created[0];

    // Set up detection callback that publishes to NATS
    mock_processor->SetDetectionCallback([&](const DetectionEvent& event) {
        mock_publisher->Publish(event);
    });

    // Simulate some detections
    for (int i = 0; i < 10; ++i) {
        DetectionEvent event;
        event.stream_id = "e2e_test";
        event.frame_number = static_cast<uint64_t>(i);
        event.timestamp = GetCurrentTimestampMs();

        if (i % 3 == 0) {
            Detection det;
            det.class_name = "person";
            det.class_id = 0;
            det.confidence = 0.9f;
            det.bbox = {100, 100, 50, 100};
            event.detections.push_back(det);
        }

        mock_processor->SimulateDetection(event);
    }

    // Verify published events
    auto events = mock_publisher->GetPublishedEvents();
    EXPECT_EQ(events.size(), 10);  // All frames published

    // Count frames with detections
    int frames_with_detections = 0;
    for (const auto& event : events) {
        if (!event.detections.empty()) {
            ++frames_with_detections;
        }
    }
    EXPECT_EQ(frames_with_detections, 4);  // 0, 3, 6, 9

    // Stop processor
    processor->Stop();
    EXPECT_FALSE(processor->IsRunning());
}

// ============================================================================
// Stress Test
// ============================================================================

TEST(StressTest, HighVolumeDetections) {
    auto mock_publisher = std::make_shared<MockMessagePublisher>();
    mock_publisher->Connect();

    StreamInfo info;
    info.stream_id = "stress_test";
    info.rtsp_url = "rtsp://mock/stream";
    info.hef_path = "/mock/model.hef";

    MockStreamProcessor processor(info);
    processor.SetDetectionCallback([&](const DetectionEvent& event) {
        mock_publisher->Publish(event);
    });

    processor.Start();

    // Simulate high volume - 1000 frames at 30fps = ~33 seconds worth
    const int num_frames = 1000;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < num_frames; ++i) {
        DetectionEvent event;
        event.stream_id = "stress_test";
        event.frame_number = static_cast<uint64_t>(i);
        event.timestamp = GetCurrentTimestampMs();

        // Add multiple detections per frame
        for (int d = 0; d < 5; ++d) {
            Detection det;
            det.class_name = "object_" + std::to_string(d);
            det.class_id = d;
            det.confidence = 0.8f + (d * 0.02f);
            det.bbox = {d * 50, d * 50, 40, 40};
            event.detections.push_back(det);
        }

        processor.SimulateDetection(event);
    }

    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    processor.Stop();

    // Verify all events were processed
    EXPECT_EQ(mock_publisher->GetEventCount(), static_cast<size_t>(num_frames));

    // Should complete in reasonable time (< 1 second for mock processing)
    EXPECT_LT(duration_ms, 1000) << "Processing took too long: " << duration_ms << "ms";

    std::cout << "Processed " << num_frames << " frames in " << duration_ms << "ms\n";
    std::cout << "Throughput: " << (num_frames * 1000.0 / duration_ms) << " fps\n";
}

}  // namespace testing
}  // namespace stream_daemon
