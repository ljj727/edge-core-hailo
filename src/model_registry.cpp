#include "model_registry.h"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace stream_daemon {

namespace {

constexpr const char* kModelConfigFile = "model_config.json";
constexpr const char* kModelHefFile = "model.hef";

// Default post-process library path
constexpr const char* kDefaultPostProcessSo = "/usr/lib/hailo-post-processes/libyolo_hailortpp_post.so";

}  // namespace

ModelRegistry::ModelRegistry(std::string models_dir)
    : models_dir_(std::move(models_dir)) {
}

VoidResult ModelRegistry::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Create models directory if it doesn't exist
    try {
        if (!fs::exists(models_dir_)) {
            fs::create_directories(models_dir_);
            LogInfo("Created models directory: " + models_dir_);
        }
    } catch (const fs::filesystem_error& e) {
        return MakeError("Failed to create models directory: " + std::string(e.what()));
    }

    // Scan existing models
    size_t count = 0;
    try {
        for (const auto& entry : fs::directory_iterator(models_dir_)) {
            if (entry.is_directory()) {
                auto result = LoadModelFromDir(entry.path().string());
                if (IsOk(result)) {
                    auto info = GetValue(std::move(result));
                    models_[info.model_id] = std::move(info);
                    ++count;
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        LogWarning("Error scanning models directory: " + std::string(e.what()));
    }

    LogInfo("ModelRegistry initialized with " + std::to_string(count) + " models");
    return MakeOk();
}

StringResult ModelRegistry::UploadModel(const std::vector<uint8_t>& zip_data, bool overwrite) {
    // Create temporary directory for extraction
    std::string temp_dir = models_dir_ + "/.temp_" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());

    try {
        fs::create_directories(temp_dir);
    } catch (const fs::filesystem_error& e) {
        return MakeStringError("Failed to create temp directory: " + std::string(e.what()));
    }

    // Extract and parse ZIP
    auto config_result = ExtractAndParseZip(zip_data, temp_dir);
    if (IsError(config_result)) {
        fs::remove_all(temp_dir);
        return MakeStringError(GetError(config_result));
    }

    auto config = GetValue(std::move(config_result));
    std::string model_id = config.model_id;

    // Check if model already exists
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (models_.find(model_id) != models_.end()) {
            if (!overwrite) {
                fs::remove_all(temp_dir);
                return MakeStringError("Model '" + model_id + "' already exists. Use overwrite=true to replace.");
            }

            // Check if model is in use
            if (models_[model_id].usage_count > 0) {
                fs::remove_all(temp_dir);
                return MakeStringError("Model '" + model_id + "' is in use by " +
                                  std::to_string(models_[model_id].usage_count) + " stream(s)");
            }

            // Remove existing model directory
            try {
                fs::remove_all(models_dir_ + "/" + model_id);
            } catch (const fs::filesystem_error& e) {
                fs::remove_all(temp_dir);
                return MakeStringError("Failed to remove existing model: " + std::string(e.what()));
            }
        }
    }

    // Move temp directory to final location
    std::string model_dir = models_dir_ + "/" + model_id;
    try {
        fs::rename(temp_dir, model_dir);
    } catch (const fs::filesystem_error& e) {
        // rename might fail across filesystems, try copy
        try {
            fs::copy(temp_dir, model_dir, fs::copy_options::recursive);
            fs::remove_all(temp_dir);
        } catch (const fs::filesystem_error& e2) {
            fs::remove_all(temp_dir);
            return MakeStringError("Failed to move model to final location: " + std::string(e2.what()));
        }
    }

    // Create ModelInfo
    ModelInfo info;
    info.model_id = config.model_id;
    info.name = config.name.empty() ? config.model_id : config.name;
    info.version = config.version;
    info.date = config.date;
    info.task = config.task.empty() ? "det" : config.task;
    info.hef_path = model_dir + "/" + kModelHefFile;
    info.post_process_so = config.post_process_so.empty() ? kDefaultPostProcessSo : config.post_process_so;
    info.function_name = config.function_name.empty() ? "yolov8" : config.function_name;
    info.labels = config.labels;
    info.description = config.description;
    info.num_keypoints = config.num_keypoints;
    info.registered_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    info.model_dir = model_dir;
    info.usage_count = 0;

    // Register model
    {
        std::lock_guard<std::mutex> lock(mutex_);
        models_[model_id] = std::move(info);
    }

    LogInfo("Model uploaded: " + model_id + " -> " + model_dir);
    return MakeStringResult(model_id);
}

VoidResult ModelRegistry::DeleteModel(const std::string& model_id, bool force) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = models_.find(model_id);
    if (it == models_.end()) {
        return MakeError("Model '" + model_id + "' not found");
    }

    if (it->second.usage_count > 0 && !force) {
        return MakeError("Model '" + model_id + "' is in use by " +
                        std::to_string(it->second.usage_count) + " stream(s). Use force=true to delete.");
    }

    // Remove model directory
    try {
        fs::remove_all(it->second.model_dir);
    } catch (const fs::filesystem_error& e) {
        return MakeError("Failed to delete model directory: " + std::string(e.what()));
    }

    models_.erase(it);
    LogInfo("Model deleted: " + model_id);

    return MakeOk();
}

std::optional<ModelInfo> ModelRegistry::GetModel(const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = models_.find(model_id);
    if (it != models_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<ModelInfo> ModelRegistry::GetAllModels() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ModelInfo> result;
    result.reserve(models_.size());
    for (const auto& [id, info] : models_) {
        result.push_back(info);
    }
    return result;
}

bool ModelRegistry::HasModel(const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_.find(model_id) != models_.end();
}

size_t ModelRegistry::GetModelCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_.size();
}

std::string ModelRegistry::GetHefPath(const std::string& model_id) const {
    auto model = GetModel(model_id);
    return model ? model->hef_path : "";
}

std::optional<std::tuple<std::string, std::string, std::string>>
ModelRegistry::GetModelPaths(const std::string& model_id) const {
    auto model = GetModel(model_id);
    if (!model) {
        return std::nullopt;
    }
    return std::make_tuple(model->hef_path, model->post_process_so, model->function_name);
}

void ModelRegistry::IncrementUsage(const std::string& model_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = models_.find(model_id);
    if (it != models_.end()) {
        ++it->second.usage_count;
    }
}

void ModelRegistry::DecrementUsage(const std::string& model_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = models_.find(model_id);
    if (it != models_.end() && it->second.usage_count > 0) {
        --it->second.usage_count;
    }
}

size_t ModelRegistry::RescanModels() {
    std::lock_guard<std::mutex> lock(mutex_);

    models_.clear();

    size_t count = 0;
    try {
        for (const auto& entry : fs::directory_iterator(models_dir_)) {
            if (entry.is_directory() && entry.path().filename().string()[0] != '.') {
                auto result = LoadModelFromDir(entry.path().string());
                if (IsOk(result)) {
                    auto info = GetValue(std::move(result));
                    models_[info.model_id] = std::move(info);
                    ++count;
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        LogWarning("Error scanning models directory: " + std::string(e.what()));
    }

    LogInfo("Rescanned models: found " + std::to_string(count) + " models");
    return count;
}

Result<ModelConfig> ModelRegistry::ExtractAndParseZip(
    const std::vector<uint8_t>& zip_data,
    const std::string& temp_dir) const {

    // Open ZIP from memory
    zip_error_t error;
    zip_error_init(&error);

    zip_source_t* src = zip_source_buffer_create(zip_data.data(), zip_data.size(), 0, &error);
    if (!src) {
        std::string err_msg = zip_error_strerror(&error);
        zip_error_fini(&error);
        return std::string("Failed to create ZIP source: " + err_msg);
    }

    zip_t* archive = zip_open_from_source(src, ZIP_RDONLY, &error);
    if (!archive) {
        std::string err_msg = zip_error_strerror(&error);
        zip_source_free(src);
        zip_error_fini(&error);
        return std::string("Failed to open ZIP: " + err_msg);
    }

    bool has_hef = false;
    bool has_config = false;

    // Extract all files
    zip_int64_t num_entries = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < num_entries; ++i) {
        const char* name = zip_get_name(archive, i, 0);
        if (!name) continue;

        std::string filename = name;

        // Skip directories
        if (filename.back() == '/') continue;

        // Get just the filename (ignore directory structure in ZIP)
        size_t last_slash = filename.find_last_of('/');
        std::string basename = (last_slash != std::string::npos) ?
                              filename.substr(last_slash + 1) : filename;

        // Only extract model.hef and model_config.json
        if (basename != kModelHefFile && basename != kModelConfigFile) {
            continue;
        }

        // Open file in archive
        zip_file_t* zf = zip_fopen_index(archive, i, 0);
        if (!zf) {
            zip_close(archive);
            return std::string("Failed to open file in ZIP: " + filename);
        }

        // Get file size
        zip_stat_t stat;
        zip_stat_index(archive, i, 0, &stat);

        // Read file content
        std::vector<char> buffer(stat.size);
        zip_int64_t bytes_read = zip_fread(zf, buffer.data(), stat.size);
        zip_fclose(zf);

        if (bytes_read != static_cast<zip_int64_t>(stat.size)) {
            zip_close(archive);
            return std::string("Failed to read file from ZIP: " + filename);
        }

        // Write to temp directory
        std::string out_path = temp_dir + "/" + basename;
        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            zip_close(archive);
            return std::string("Failed to create output file: " + out_path);
        }
        out.write(buffer.data(), buffer.size());
        out.close();

        if (basename == kModelHefFile) has_hef = true;
        if (basename == kModelConfigFile) has_config = true;
    }

    zip_close(archive);

    // Verify required files exist
    if (!has_hef) {
        return std::string("ZIP must contain 'model.hef'");
    }
    if (!has_config) {
        return std::string("ZIP must contain 'model_config.json'");
    }

    // Parse model config
    return ParseModelConfig(temp_dir + "/" + kModelConfigFile);
}

Result<ModelConfig> ModelRegistry::ParseModelConfig(const std::string& json_path) const {
    try {
        std::ifstream file(json_path);
        if (!file.is_open()) {
            return std::string("Failed to open config file: " + json_path);
        }

        json j = json::parse(file);

        ModelConfig config;

        // id is required
        if (!j.contains("id") || !j["id"].is_string()) {
            return std::string("model_config.json must contain 'id' string");
        }
        config.model_id = j["id"].get<std::string>();

        if (config.model_id.empty()) {
            return std::string("id cannot be empty");
        }

        // Optional fields
        if (j.contains("name") && j["name"].is_string()) {
            config.name = j["name"].get<std::string>();
        }

        if (j.contains("function_name") && j["function_name"].is_string()) {
            config.function_name = j["function_name"].get<std::string>();
        }

        if (j.contains("post_process_so") && j["post_process_so"].is_string()) {
            config.post_process_so = j["post_process_so"].get<std::string>();
        }

        if (j.contains("description") && j["description"].is_string()) {
            config.description = j["description"].get<std::string>();
        }

        if (j.contains("version") && j["version"].is_string()) {
            config.version = j["version"].get<std::string>();
        }

        if (j.contains("date") && j["date"].is_string()) {
            config.date = j["date"].get<std::string>();
        }

        // Task type: "det" or "pose" (default: "det")
        if (j.contains("task") && j["task"].is_string()) {
            config.task = j["task"].get<std::string>();
        } else {
            config.task = "det";
        }

        // Number of keypoints for pose model
        if (j.contains("num_keypoints") && j["num_keypoints"].is_number_integer()) {
            config.num_keypoints = j["num_keypoints"].get<int>();
        }

        if (j.contains("labels") && j["labels"].is_array()) {
            for (const auto& label : j["labels"]) {
                if (label.is_string()) {
                    config.labels.push_back(label.get<std::string>());
                }
            }
        }

        // Parse outputs array
        if (j.contains("outputs") && j["outputs"].is_array()) {
            for (const auto& output : j["outputs"]) {
                if (output.is_object() && output.contains("label") && output["label"].is_string()) {
                    ModelOutput mo;
                    mo.label = output["label"].get<std::string>();
                    if (output.contains("classifiers") && output["classifiers"].is_array()) {
                        for (const auto& c : output["classifiers"]) {
                            if (c.is_string()) {
                                mo.classifiers.push_back(c.get<std::string>());
                            }
                        }
                    }
                    config.outputs.push_back(std::move(mo));
                }
            }
        }

        // If labels not explicitly set, extract from outputs
        if (config.labels.empty() && !config.outputs.empty()) {
            for (const auto& output : config.outputs) {
                config.labels.push_back(output.label);
            }
        }

        return config;

    } catch (const json::exception& e) {
        return std::string("JSON parse error: " + std::string(e.what()));
    } catch (const std::exception& e) {
        return std::string("Error parsing config: " + std::string(e.what()));
    }
}

Result<ModelInfo> ModelRegistry::LoadModelFromDir(const std::string& model_dir) const {
    std::string config_path = model_dir + "/" + kModelConfigFile;
    std::string hef_path = model_dir + "/" + kModelHefFile;

    // Check required files exist
    if (!fs::exists(config_path)) {
        return std::string("Missing " + std::string(kModelConfigFile) + " in " + model_dir);
    }
    if (!fs::exists(hef_path)) {
        return std::string("Missing " + std::string(kModelHefFile) + " in " + model_dir);
    }

    // Parse config
    auto config_result = ParseModelConfig(config_path);
    if (IsError(config_result)) {
        return GetError(config_result);
    }
    auto config = GetValue(std::move(config_result));

    // Get current time as registered_at (C++17 compatible)
    int64_t registered_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Build ModelInfo
    ModelInfo info;
    info.model_id = config.model_id;
    info.name = config.name.empty() ? config.model_id : config.name;
    info.version = config.version;
    info.date = config.date;
    info.task = config.task.empty() ? "det" : config.task;
    info.hef_path = hef_path;
    info.post_process_so = config.post_process_so.empty() ? kDefaultPostProcessSo : config.post_process_so;
    info.function_name = config.function_name.empty() ? "yolov8" : config.function_name;
    info.labels = config.labels;
    info.outputs = std::move(config.outputs);
    info.description = config.description;
    info.num_keypoints = config.num_keypoints;
    info.registered_at = registered_at;
    info.model_dir = model_dir;
    info.usage_count = 0;

    return info;
}

}  // namespace stream_daemon
