#ifndef STREAM_DAEMON_DEBUG_UTILS_H_
#define STREAM_DAEMON_DEBUG_UTILS_H_

#include "common.h"

#include <gst/gst.h>

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace stream_daemon {
namespace debug {

/**
 * @brief GStreamer debug utilities
 */
class GStreamerDebug {
public:
    /**
     * @brief Set GStreamer debug level
     * @param level Debug level (0-9, where 0=none, 9=maximum)
     * @param categories Comma-separated list of categories (empty = all)
     *
     * Example categories: "hailonet:5,rtspsrc:4,basesrc:3"
     */
    static void SetDebugLevel(int level, std::string_view categories = "") {
        if (categories.empty()) {
            gst_debug_set_default_threshold(static_cast<GstDebugLevel>(level));
        } else {
            gst_debug_set_threshold_from_string(
                std::string(categories).c_str(), TRUE);
        }

        LogInfo("GStreamer debug level set to " + std::to_string(level));
    }

    /**
     * @brief Enable dot file generation for pipeline debugging
     * @param output_dir Directory to save .dot files
     *
     * Generate pipeline graphs that can be converted to images:
     * dot -Tpng pipeline.dot > pipeline.png
     */
    static void EnableDotFileGeneration(std::string_view output_dir) {
        setenv("GST_DEBUG_DUMP_DOT_DIR", std::string(output_dir).c_str(), 1);
        LogInfo("Pipeline DOT files will be saved to: " + std::string(output_dir));
    }

    /**
     * @brief Dump pipeline to dot file
     * @param pipeline GStreamer pipeline
     * @param name Name for the dot file
     */
    static void DumpPipelineToDot(GstElement* pipeline, std::string_view name) {
        if (!pipeline) return;

        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(
            GST_BIN(pipeline),
            GST_DEBUG_GRAPH_SHOW_ALL,
            std::string(name).c_str());
    }

    /**
     * @brief Get pipeline state as string
     */
    [[nodiscard]] static std::string GetPipelineState(GstElement* pipeline) {
        if (!pipeline) return "NULL";

        GstState state, pending;
        GstStateChangeReturn ret = gst_element_get_state(
            pipeline, &state, &pending, GST_CLOCK_TIME_NONE);

        std::string result;
        switch (ret) {
            case GST_STATE_CHANGE_SUCCESS:
                result = "SUCCESS: ";
                break;
            case GST_STATE_CHANGE_ASYNC:
                result = "ASYNC: ";
                break;
            case GST_STATE_CHANGE_FAILURE:
                result = "FAILURE: ";
                break;
            case GST_STATE_CHANGE_NO_PREROLL:
                result = "NO_PREROLL: ";
                break;
        }

        result += gst_element_state_get_name(state);
        if (pending != GST_STATE_VOID_PENDING) {
            result += " -> " + std::string(gst_element_state_get_name(pending));
        }

        return result;
    }

    /**
     * @brief List all elements in pipeline
     */
    [[nodiscard]] static std::string ListPipelineElements(GstElement* pipeline) {
        if (!pipeline || !GST_IS_BIN(pipeline)) {
            return "Not a valid pipeline";
        }

        std::string result = "Pipeline elements:\n";

        GstIterator* it = gst_bin_iterate_elements(GST_BIN(pipeline));
        GValue item = G_VALUE_INIT;
        gboolean done = FALSE;

        while (!done) {
            switch (gst_iterator_next(it, &item)) {
                case GST_ITERATOR_OK: {
                    GstElement* element = GST_ELEMENT(g_value_get_object(&item));
                    result += "  - " + std::string(GST_ELEMENT_NAME(element));
                    result += " (" + std::string(G_OBJECT_TYPE_NAME(element)) + ")\n";
                    g_value_reset(&item);
                    break;
                }
                case GST_ITERATOR_RESYNC:
                    gst_iterator_resync(it);
                    break;
                case GST_ITERATOR_ERROR:
                case GST_ITERATOR_DONE:
                    done = TRUE;
                    break;
            }
        }

        g_value_unset(&item);
        gst_iterator_free(it);

        return result;
    }

    /**
     * @brief Check if Hailo plugins are available
     */
    [[nodiscard]] static bool CheckHailoPlugins() {
        GstElementFactory* hailonet = gst_element_factory_find("hailonet");
        GstElementFactory* hailofilter = gst_element_factory_find("hailofilter");

        bool available = (hailonet != nullptr) && (hailofilter != nullptr);

        if (hailonet) gst_object_unref(hailonet);
        if (hailofilter) gst_object_unref(hailofilter);

        if (available) {
            LogInfo("Hailo GStreamer plugins are available");
        } else {
            LogWarning("Hailo GStreamer plugins NOT found");
        }

        return available;
    }

    /**
     * @brief List all available GStreamer plugins
     */
    static void ListAvailablePlugins() {
        GstRegistry* registry = gst_registry_get();
        GList* plugins = gst_registry_get_plugin_list(registry);

        LogInfo("Available GStreamer plugins:");
        for (GList* l = plugins; l != nullptr; l = l->next) {
            GstPlugin* plugin = GST_PLUGIN(l->data);
            const gchar* name = gst_plugin_get_name(plugin);
            const gchar* desc = gst_plugin_get_description(plugin);
            LogInfo("  - " + std::string(name) + ": " +
                   (desc ? std::string(desc) : ""));
        }

        gst_plugin_list_free(plugins);
    }
};

/**
 * @brief Pipeline builder for testing without full Hailo setup
 */
class TestPipelineBuilder {
public:
    /**
     * @brief Build a test pipeline using videotestsrc instead of RTSP
     * @param use_fakesink Use fakesink instead of display
     */
    [[nodiscard]] static std::string BuildTestPipeline(bool use_fakesink = true) {
        std::string pipeline =
            "videotestsrc pattern=ball "
            "! video/x-raw,width=640,height=480,framerate=30/1 "
            "! videoconvert ";

        if (use_fakesink) {
            pipeline += "! fakesink sync=false";
        } else {
            pipeline += "! autovideosink";
        }

        return pipeline;
    }

    /**
     * @brief Build a test pipeline with RTSP but without Hailo
     * @param rtsp_url RTSP URL to test
     */
    [[nodiscard]] static std::string BuildRtspTestPipeline(
        std::string_view rtsp_url, bool use_fakesink = true) {

        std::string pipeline =
            "rtspsrc location=\"" + std::string(rtsp_url) + "\" "
            "latency=0 "
            "! rtph264depay "
            "! h264parse "
            "! avdec_h264 "
            "! videoconvert ";

        if (use_fakesink) {
            pipeline += "! fakesink sync=false";
        } else {
            pipeline += "! autovideosink";
        }

        return pipeline;
    }

    /**
     * @brief Build a minimal Hailo test pipeline
     * @param hef_path Path to HEF model file
     */
    [[nodiscard]] static std::string BuildHailoTestPipeline(
        std::string_view hef_path) {

        return "videotestsrc pattern=ball "
               "! video/x-raw,width=640,height=480,framerate=30/1,format=RGB "
               "! hailonet hef-path=\"" + std::string(hef_path) + "\" "
               "! hailofilter "
               "! fakesink sync=false";
    }

    /**
     * @brief Test if a pipeline string is valid
     */
    [[nodiscard]] static VoidResult ValidatePipeline(std::string_view pipeline_str) {
        GError* error = nullptr;
        GstElement* pipeline = gst_parse_launch(
            std::string(pipeline_str).c_str(), &error);

        if (error) {
            std::string err_msg = error->message;
            g_error_free(error);
            return MakeError("Pipeline validation failed: " + err_msg);
        }

        if (!pipeline) {
            return MakeError("Pipeline creation failed");
        }

        // Try to set to READY state
        GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_READY);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);

        if (ret == GST_STATE_CHANGE_FAILURE) {
            return MakeError("Pipeline failed to reach READY state");
        }

        return MakeOk();
    }
};

/**
 * @brief Performance profiler for stream processing
 */
class PerformanceProfiler {
public:
    struct Stats {
        uint64_t total_frames{0};
        double avg_fps{0.0};
        double min_fps{std::numeric_limits<double>::max()};
        double max_fps{0.0};
        double avg_latency_ms{0.0};
        uint64_t dropped_frames{0};
    };

    void RecordFrame(double fps, double latency_ms, bool dropped = false) {
        ++total_frames_;
        fps_sum_ += fps;

        if (fps < min_fps_) min_fps_ = fps;
        if (fps > max_fps_) max_fps_ = fps;

        latency_sum_ += latency_ms;

        if (dropped) ++dropped_frames_;
    }

    [[nodiscard]] Stats GetStats() const {
        Stats stats;
        stats.total_frames = total_frames_;
        stats.dropped_frames = dropped_frames_;

        if (total_frames_ > 0) {
            stats.avg_fps = fps_sum_ / static_cast<double>(total_frames_);
            stats.min_fps = min_fps_;
            stats.max_fps = max_fps_;
            stats.avg_latency_ms = latency_sum_ / static_cast<double>(total_frames_);
        }

        return stats;
    }

    void Reset() {
        total_frames_ = 0;
        dropped_frames_ = 0;
        fps_sum_ = 0.0;
        min_fps_ = std::numeric_limits<double>::max();
        max_fps_ = 0.0;
        latency_sum_ = 0.0;
    }

    [[nodiscard]] std::string GetReport() const {
        auto stats = GetStats();
        std::ostringstream oss;
        oss << "Performance Report:\n"
            << "  Total frames: " << stats.total_frames << "\n"
            << "  Dropped frames: " << stats.dropped_frames << "\n"
            << "  FPS (avg/min/max): " << stats.avg_fps << "/"
            << stats.min_fps << "/" << stats.max_fps << "\n"
            << "  Avg latency: " << stats.avg_latency_ms << " ms\n";
        return oss.str();
    }

private:
    uint64_t total_frames_{0};
    uint64_t dropped_frames_{0};
    double fps_sum_{0.0};
    double min_fps_{std::numeric_limits<double>::max()};
    double max_fps_{0.0};
    double latency_sum_{0.0};
};

}  // namespace debug
}  // namespace stream_daemon

#endif  // STREAM_DAEMON_DEBUG_UTILS_H_
