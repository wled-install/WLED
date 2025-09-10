#include "ImageCacheManager.h"
#include "esp_psram.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include "Arduino.h"

static const char* TAG = "ImageCache";

// Singleton instance definition
ImageCacheManager& ImageCacheManager::getInstance() {
    static ImageCacheManager instance;
    return instance;
}

// Constructor: Initialize resources
ImageCacheManager::ImageCacheManager() :
    preload_task_handle(NULL),
    psram_limit(0),
    psram_used(0)
{
    cache_mutex = xSemaphoreCreateMutex();
    size_t total_psram = esp_psram_get_size();
    psram_limit = static_cast<size_t>(total_psram * 0.8);
    ESP_LOGI(TAG, "Total PSRAM: %u bytes, Cache Limit (80%%): %u bytes", total_psram, psram_limit);
}

// Destructor: Clean up resources
ImageCacheManager::~ImageCacheManager() {
    clearCache();
    vSemaphoreDelete(cache_mutex);
}

// Start the non-blocking preload task
void ImageCacheManager::startPreload(const std::string& root_path) {
    if (preload_task_handle != NULL) {
        ESP_LOGW(TAG, "Preload task is already running.");
        return;
    }
    clearCache();
    preload_root_path = root_path.c_str(); // Convert std::string to psram_string
    xTaskCreate(_preloadTask, "preload_task", 4096, this, IMAGECACHE_BG_PRIORITY, &preload_task_handle);
}

// The main 'get' function with on-demand loading
ImageData* ImageCacheManager::getImage(const std::string& folder_path, size_t index) {
    psram_string ps_folder_path = folder_path.c_str();
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    auto it = image_cache.find(ps_folder_path);

    if (it != image_cache.end() && index < it->second.size()) {
        ImageData* data = &it->second[index];
        xSemaphoreGive(cache_mutex);
        return data;
    }
    xSemaphoreGive(cache_mutex);

    ESP_LOGI(TAG, "On-demand loading folder: %s", folder_path.c_str());
    if (_loadFolderSync(ps_folder_path)) {
        xSemaphoreTake(cache_mutex, portMAX_DELAY);
        it = image_cache.find(ps_folder_path);
        if (it != image_cache.end() && index < it->second.size()) {
            ImageData* data = &it->second[index];
            xSemaphoreGive(cache_mutex);
            return data;
        }
        xSemaphoreGive(cache_mutex);
    }
    return nullptr;
}

size_t ImageCacheManager::getFolderSize(const std::string& folder_path) {
    size_t size = 0;
    psram_string ps_folder_path = folder_path.c_str();
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    auto it = image_cache.find(ps_folder_path);
    if (it != image_cache.end()) {
        size = it->second.size();
    }
    xSemaphoreGive(cache_mutex);
    return size;
}

// The background task implementation
void ImageCacheManager::_preloadTask(void* params) {
    ImageCacheManager* manager = static_cast<ImageCacheManager*>(params);
    DIR* dir = opendir(manager->preload_root_path.c_str());
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open root directory: %s", manager->preload_root_path.c_str());
        manager->preload_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        psram_string name = entry->d_name;
        if (entry->d_type == DT_DIR && name.rfind("sequence", 0) == 0) {
            psram_string full_path = manager->preload_root_path + "/" + name;
            ESP_LOGI(TAG, "Preloading folder: %s", full_path.c_str());
            manager->_loadFolderSync(full_path);
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "Background preloading finished.");
    manager->preload_task_handle = NULL;
    vTaskDelete(NULL);
}

// The core synchronous loading logic, now using PSRAM for the temporary filename list
bool ImageCacheManager::_loadFolderSync(const psram_string& folder_path) {
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    if (image_cache.count(folder_path)) {
        xSemaphoreGive(cache_mutex);
        return true;
    }
    xSemaphoreGive(cache_mutex);

    DIR* dir = opendir(folder_path.c_str());
    if (!dir) return false;

    // This temporary vector and its strings are now allocated in PSRAM
    using psram_string_vector = std::vector<psram_string, PSRAM_Allocator<psram_string>>;
    psram_string_vector jpeg_files;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        psram_string name = entry->d_name;
        if (name.find(".jpg") != psram_string::npos || name.find(".jpeg") != psram_string::npos) {
            jpeg_files.push_back(folder_path + "/" + name);
        }
    }
    closedir(dir);
    
    std::sort(jpeg_files.begin(), jpeg_files.end());

    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    image_cache[folder_path].reserve(jpeg_files.size());
    xSemaphoreGive(cache_mutex);

    bool loaded_any = false;
    for (const auto& path : jpeg_files) {
        struct stat st;
        stat(path.c_str(), &st);
        size_t size = st.st_size;

        if (psram_used + size > psram_limit) {
            ESP_LOGW(TAG, "PSRAM limit reached. Stopping load at %s", path.c_str());
            break;
        }

        FILE* file = fopen(path.c_str(), "rb");
        if (!file) continue;

        uint8_t* buffer = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM for %s", size, path.c_str());
            fclose(file);
            continue;
        }

        fread(buffer, 1, size, file);
        fclose(file);

        xSemaphoreTake(cache_mutex, portMAX_DELAY);
        image_cache[folder_path].push_back({buffer, size});
        psram_used += size;
        xSemaphoreGive(cache_mutex);
        loaded_any = true;
    }
    return loaded_any;
}

void ImageCacheManager::clearCache() {
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    for (auto& pair : image_cache) {
        for (auto& img : pair.second) {
            free(img.buffer);
        }
    }
    image_cache.clear();
    psram_used = 0;
    xSemaphoreGive(cache_mutex);
    ESP_LOGI(TAG, "Image cache cleared.");
}
