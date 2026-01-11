#ifndef STREAM_DAEMON_MODEL_REGISTRY_H_
#define STREAM_DAEMON_MODEL_REGISTRY_H_

#include "common.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace stream_daemon {

/**
 * @brief Model output label with classifiers
 */
struct ModelOutput {
    std::string label;                  // Label (e.g., "person", "car")
    std::vector<std::string> classifiers;  // Classifier list
};

/**
 * @brief Model configuration from model_config.json
 */
struct ModelConfig {
    std::string model_id;               // Unique identifier (required)
    std::string name;                   // Display name (optional)
    std::string version;                // Version (optional, e.g. "0.0.1")
    std::string date;                   // Date (optional, e.g. "26.01.01")
    std::string task;                   // Task type: "det" or "pose" (default: "det")
    std::string function_name;          // Post-process function (default: "yolov8")
    std::string post_process_so;        // Post-process library path (optional)
    std::vector<std::string> labels;    // Class labels
    std::vector<ModelOutput> outputs;   // Output labels with classifiers
    std::string description;            // Description (optional)
    int num_keypoints{0};               // Number of keypoints for pose model
};

/**
 * @brief Model information (stored in registry)
 */
struct ModelInfo {
    std::string model_id;               // Unique identifier
    std::string name;                   // Display name
    std::string version;                // Version
    std::string date;                   // Date
    std::string task;                   // Task type: "det" or "pose"
    std::string hef_path;               // Full path to HEF file
    std::string post_process_so;        // Post-process library path
    std::string function_name;          // Post-process function name
    std::vector<std::string> labels;    // Class labels
    std::vector<ModelOutput> outputs;   // Output labels with classifiers
    std::string description;            // Description
    int num_keypoints{0};               // Number of keypoints for pose model
    int64_t registered_at{0};           // Registration timestamp (ms)
    std::string model_dir;              // Directory containing model files

    // Runtime tracking (not persisted)
    mutable int usage_count{0};         // Number of streams using this model

    bool IsPoseModel() const { return task == "pose"; }
};

/**
 * @brief Model registry for managing HEF models
 *
 * File-based registry that stores models in:
 *   {models_dir}/{model_id}/
 *     ├── model.hef
 *     └── model_config.json
 *
 * Thread-safe with automatic persistence.
 */
class ModelRegistry {
public:
    /**
     * @brief Create a model registry
     * @param models_dir Base directory for model storage (e.g., /var/lib/stream-daemon/models)
     */
    explicit ModelRegistry(std::string models_dir);

    ~ModelRegistry() = default;

    // Non-copyable, non-movable
    ModelRegistry(const ModelRegistry&) = delete;
    ModelRegistry& operator=(const ModelRegistry&) = delete;

    /**
     * @brief Initialize the registry (create dirs, scan existing models)
     * @return Success or error
     */
    [[nodiscard]] VoidResult Initialize();

    /**
     * @brief Upload and register a model from ZIP data
     * @param zip_data Complete ZIP file data
     * @param overwrite Whether to overwrite existing model
     * @return Model ID on success or error
     */
    [[nodiscard]] StringResult UploadModel(const std::vector<uint8_t>& zip_data, bool overwrite = false);

    /**
     * @brief Delete a model
     * @param model_id Model ID to delete
     * @param force Force delete even if in use
     * @return Success or error
     */
    [[nodiscard]] VoidResult DeleteModel(const std::string& model_id, bool force = false);

    /**
     * @brief Get model info by ID
     * @param model_id Model ID
     * @return Model info or nullopt if not found
     */
    [[nodiscard]] std::optional<ModelInfo> GetModel(const std::string& model_id) const;

    /**
     * @brief Get all registered models
     * @return Vector of all model info
     */
    [[nodiscard]] std::vector<ModelInfo> GetAllModels() const;

    /**
     * @brief Check if a model exists
     * @param model_id Model ID
     * @return true if exists
     */
    [[nodiscard]] bool HasModel(const std::string& model_id) const;

    /**
     * @brief Get number of registered models
     */
    [[nodiscard]] size_t GetModelCount() const;

    /**
     * @brief Get HEF path for a model
     * @param model_id Model ID
     * @return HEF path or empty string if not found
     */
    [[nodiscard]] std::string GetHefPath(const std::string& model_id) const;

    /**
     * @brief Get model info needed for stream processor
     * @param model_id Model ID
     * @return tuple of (hef_path, post_process_so, function_name) or nullopt
     */
    [[nodiscard]] std::optional<std::tuple<std::string, std::string, std::string>>
    GetModelPaths(const std::string& model_id) const;

    /**
     * @brief Increment usage count for a model
     */
    void IncrementUsage(const std::string& model_id);

    /**
     * @brief Decrement usage count for a model
     */
    void DecrementUsage(const std::string& model_id);

    /**
     * @brief Rescan models directory
     * @return Number of models found
     */
    [[nodiscard]] size_t RescanModels();

    /**
     * @brief Get models directory path
     */
    [[nodiscard]] const std::string& GetModelsDir() const { return models_dir_; }

private:
    /**
     * @brief Extract ZIP and parse model config
     * @param zip_data ZIP file data
     * @param temp_dir Temporary directory for extraction
     * @return Model config or error
     */
    [[nodiscard]] Result<ModelConfig> ExtractAndParseZip(
        const std::vector<uint8_t>& zip_data,
        const std::string& temp_dir) const;

    /**
     * @brief Parse model_config.json
     * @param json_path Path to JSON file
     * @return Model config or error
     */
    [[nodiscard]] Result<ModelConfig> ParseModelConfig(const std::string& json_path) const;

    /**
     * @brief Load a model from its directory
     * @param model_dir Model directory path
     * @return Model info or error
     */
    [[nodiscard]] Result<ModelInfo> LoadModelFromDir(const std::string& model_dir) const;

    mutable std::mutex mutex_;
    std::string models_dir_;
    std::unordered_map<std::string, ModelInfo> models_;
};

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_MODEL_REGISTRY_H_
