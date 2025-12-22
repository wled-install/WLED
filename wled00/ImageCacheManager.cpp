#include "ImageCacheManager.h"
#include "esp_psram.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <fstream>
#include <set>

static const char* TAG = "ImageCache";

ImageCacheManager& ImageCacheManager::getInstance() {
  static ImageCacheManager instance;
  return instance;
}

ImageCacheManager::ImageCacheManager() :
  preload_task_handle(NULL),
  current_status(CacheStatus::IDLE),
  psram_limit(0),
  psram_used(0) {
  cache_mutex = xSemaphoreCreateMutex();
  loader_mutex = xSemaphoreCreateMutex();
  size_t total_psram = esp_psram_get_size();
  psram_limit = static_cast<size_t>(total_psram * 0.8);
  ESP_LOGI(TAG, "Total PSRAM: %u bytes, Cache Limit (80%%): %u bytes", total_psram, psram_limit);
}

ImageCacheManager::~ImageCacheManager() {
  clearCache();
  vSemaphoreDelete(cache_mutex);
  vSemaphoreDelete(loader_mutex);
}

// ============================================================================
// File List Management (lightweight, non-blocking)
// ============================================================================

bool ImageCacheManager::_ensureFileListCached(const psram_string& folder_path) {
  // Quick check if already cached
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  if (folder_file_lists.find(folder_path) != folder_file_lists.end()) {
    xSemaphoreGive(cache_mutex);
    return true;
  }
  xSemaphoreGive(cache_mutex);

  // Scan directory for filenames only (no file content loading)
  DIR* dir = opendir(folder_path.c_str());
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open directory: %s", folder_path.c_str());
    return false;
  }

  psram_string_vector filenames;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    psram_string name = entry->d_name;
    if (name.find(".jpg") != psram_string::npos || name.find(".jpeg") != psram_string::npos) {
      filenames.push_back(name);
    }
  }
  closedir(dir);

  std::sort(filenames.begin(), filenames.end());

  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  folder_file_lists[folder_path] = std::move(filenames);
  xSemaphoreGive(cache_mutex);

  ESP_LOGI(TAG, "Cached file list for %s: %d files", folder_path.c_str(), filenames.size());
  return true;
}

psram_string ImageCacheManager::_getFilenameByIndex(const psram_string& folder_path, size_t index) {
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  auto it = folder_file_lists.find(folder_path);
  if (it != folder_file_lists.end() && index < it->second.size()) {
    psram_string result = it->second[index];
    xSemaphoreGive(cache_mutex);
    return result;
  }
  xSemaphoreGive(cache_mutex);
  return "";
}

size_t ImageCacheManager::getFolderSizeFromDisk(const std::string& folder_path) {
  psram_string ps_path = folder_path.c_str();
  _ensureFileListCached(ps_path);

  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  size_t size = 0;
  auto it = folder_file_lists.find(ps_path);
  if (it != folder_file_lists.end()) {
    size = it->second.size();
  }
  xSemaphoreGive(cache_mutex);
  return size;
}

// ============================================================================
// Streaming API (non-blocking, returns immediately)
// ============================================================================

ImageResult ImageCacheManager::getImageStreaming(const std::string& folder_path, size_t index) {
  psram_string ps_folder_path = folder_path.c_str();
  ImageResult result = { nullptr, 0, false, false };

  // Ensure we have the file list
  if (!_ensureFileListCached(ps_folder_path)) {
    return result;
  }

  // Fast path: check if already in cache
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  ImageData* cached = _getImageByIndex(ps_folder_path, index);
  if (cached) {
    result.buffer = cached->buffer;
    result.size = cached->size;
    result.from_cache = true;
    result.needs_free = false;
    xSemaphoreGive(cache_mutex);
    return result;
  }
  xSemaphoreGive(cache_mutex);

  // Not cached - get filename and load from disk
  psram_string filename = _getFilenameByIndex(ps_folder_path, index);
  if (filename.empty()) {
    ESP_LOGW(TAG, "No filename at index %d for %s", index, folder_path.c_str());
    return result;
  }

  psram_string full_path = ps_folder_path + "/" + filename;

  struct stat st;
  if (stat(full_path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "Failed to stat: %s", full_path.c_str());
    return result;
  }

  size_t file_size = st.st_size;

  // Allocate buffer for streaming
  uint8_t* buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
  if (!buffer) {
    // Fallback to internal RAM for small files
    if (file_size <= 8192) {
      buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_DEFAULT);
    }
    if (!buffer) {
      ESP_LOGE(TAG, "Failed to allocate %u bytes for %s", file_size, filename.c_str());
      return result;
    }
  }

  FILE* file = fopen(full_path.c_str(), "rb");
  if (!file) {
    free(buffer);
    ESP_LOGE(TAG, "Failed to open: %s", full_path.c_str());
    return result;
  }

  size_t bytes_read = fread(buffer, 1, file_size, file);
  fclose(file);

  if (bytes_read != file_size) {
    free(buffer);
    ESP_LOGE(TAG, "Incomplete read: %s (%u/%u)", filename.c_str(), bytes_read, file_size);
    return result;
  }

  result.buffer = buffer;
  result.size = file_size;
  result.from_cache = false;
  result.needs_free = true;

  ESP_LOGI(TAG, "Streamed from disk: %s (%u bytes)", filename.c_str(), file_size);

  // Add to cache if space available (so next access is fast)
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  if (psram_used + file_size <= psram_limit) {
    // Check if another thread already cached it
    psram_file_map& cached_files = image_cache[ps_folder_path];
    if (cached_files.find(filename) == cached_files.end()) {
      // Allocate separate cache buffer
      uint8_t* cache_buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
      if (cache_buffer) {
        memcpy(cache_buffer, buffer, file_size);
        cached_files[filename] = { cache_buffer, file_size, st.st_mtime };
        psram_used += file_size;
        ESP_LOGI(TAG, "Added to cache while streaming: %s", filename.c_str());
      }
    }
  }
  xSemaphoreGive(cache_mutex);

  // Queue background sync for rest of folder
  _queueBackgroundSync(ps_folder_path);

  return result;
}

// ============================================================================
// Background Sync Management
// ============================================================================

void ImageCacheManager::_queueBackgroundSync(const psram_string& folder_path) {
  xSemaphoreTake(cache_mutex, portMAX_DELAY);

  // Check if already queued or fully cached
  bool already_queued = std::find(pending_sync_folders.begin(),
    pending_sync_folders.end(),
    folder_path) != pending_sync_folders.end();

  if (!already_queued) {
    // Check if folder is already fully cached
    auto file_list_it = folder_file_lists.find(folder_path);
    auto cache_it = image_cache.find(folder_path);

    bool fully_cached = false;
    if (file_list_it != folder_file_lists.end() && cache_it != image_cache.end()) {
      fully_cached = (cache_it->second.size() >= file_list_it->second.size());
    }

    if (!fully_cached) {
      pending_sync_folders.push_back(folder_path);
      ESP_LOGI(TAG, "Queued for background sync: %s", folder_path.c_str());
    }
  }
  xSemaphoreGive(cache_mutex);

  // Start background task if not running
  if (preload_task_handle == NULL) {
    xTaskCreatePinnedToCore(_backgroundSyncTask, "cache_sync", 4096, this,
      IMAGECACHE_BG_PRIORITY, &preload_task_handle, 0);
  }
}

void ImageCacheManager::_backgroundSyncTask(void* params) {
  ImageCacheManager* manager = static_cast<ImageCacheManager*>(params);
  manager->current_status = CacheStatus::PRELOADING_BG;

  ESP_LOGI(TAG, "Background sync task started");

  while (true) {
    psram_string folder_to_sync;

    xSemaphoreTake(manager->cache_mutex, portMAX_DELAY);
    if (manager->pending_sync_folders.empty()) {
      xSemaphoreGive(manager->cache_mutex);
      break;
    }
    folder_to_sync = manager->pending_sync_folders.front();
    manager->pending_sync_folders.erase(manager->pending_sync_folders.begin());
    xSemaphoreGive(manager->cache_mutex);

    ESP_LOGI(TAG, "Background syncing: %s", folder_to_sync.c_str());
    manager->_synchronizeFolder(folder_to_sync, false);

    vTaskDelay(pdMS_TO_TICKS(10));
  }

  manager->current_status = CacheStatus::IDLE;
  xSemaphoreTake(manager->cache_mutex, portMAX_DELAY);
  manager->current_loading_file = "";
  xSemaphoreGive(manager->cache_mutex);

  ESP_LOGI(TAG, "Background sync task finished");
  manager->preload_task_handle = NULL;
  vTaskDelete(NULL);
}

// ============================================================================
// Original API (maintained for compatibility)
// ============================================================================

void ImageCacheManager::startPreload(const std::string& root_path) {
  if (preload_task_handle != NULL) {
    ESP_LOGW(TAG, "Preload task is already running.");
    return;
  }
  preload_root_path = root_path.c_str();
  xTaskCreatePinnedToCore(_preloadTask, "preload_task", 4096, this,
    IMAGECACHE_BG_PRIORITY, &preload_task_handle, 0);
}

ImageData* ImageCacheManager::getImage(const std::string& folder_path, size_t index) {
  psram_string ps_folder_path = folder_path.c_str();

  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  ImageData* data = _getImageByIndex(ps_folder_path, index);
  xSemaphoreGive(cache_mutex);

  if (data) {
    return data;
  }

  ESP_LOGI(TAG, "On-demand loading folder: %s", folder_path.c_str());
  _synchronizeFolder(ps_folder_path, true);

  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  data = _getImageByIndex(ps_folder_path, index);
  xSemaphoreGive(cache_mutex);

  return data;
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

CacheStatus ImageCacheManager::getStatus() {
  return current_status;
}

psram_string ImageCacheManager::getCurrentFile() {
  psram_string file;
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  file = current_loading_file;
  xSemaphoreGive(cache_mutex);
  return file;
}

size_t ImageCacheManager::getCacheUsedBytes() {
  size_t used_bytes = 0;
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  used_bytes = psram_used;
  xSemaphoreGive(cache_mutex);
  return used_bytes;
}

// ============================================================================
// Core Synchronization Logic
// ============================================================================

void ImageCacheManager::_preloadTask(void* params) {
  ImageCacheManager* manager = static_cast<ImageCacheManager*>(params);
  manager->current_status = CacheStatus::PRELOADING_BG;

  DIR* dir = opendir(manager->preload_root_path.c_str());
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open root directory: %s", manager->preload_root_path.c_str());
    manager->preload_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  psram_string_vector hot_folders;
  psram_string_vector cold_folders;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    psram_string name = entry->d_name;
    if (entry->d_type == DT_DIR && name.rfind("sequence", 0) == 0) {
      if (name.length() > 4 && name.substr(name.length() - 4) == "_hot") {
        hot_folders.push_back(name);
      } else {
        cold_folders.push_back(name);
      }
    }
  }
  closedir(dir);

  std::sort(hot_folders.begin(), hot_folders.end());
  std::sort(cold_folders.begin(), cold_folders.end());

  ESP_LOGI(TAG, "--- Processing %d high-priority (_hot) folders ---", hot_folders.size());
  for (const auto& folder_name : hot_folders) {
    psram_string full_path = manager->preload_root_path + "/" + folder_name;
    manager->_synchronizeFolder(full_path, false);
  }

  ESP_LOGI(TAG, "--- Processing %d standard-priority folders ---", cold_folders.size());
  for (const auto& folder_name : cold_folders) {
    psram_string full_path = manager->preload_root_path + "/" + folder_name;
    manager->_synchronizeFolder(full_path, false);
  }

  manager->current_status = CacheStatus::IDLE;
  xSemaphoreTake(manager->cache_mutex, portMAX_DELAY);
  manager->current_loading_file = "";
  xSemaphoreGive(manager->cache_mutex);

  ESP_LOGI(TAG, "Background preload finished.");
  manager->preload_task_handle = NULL;
  vTaskDelete(NULL);
}

void ImageCacheManager::_synchronizeFolder(const psram_string& folder_path, bool is_on_demand) {
  xSemaphoreTake(loader_mutex, portMAX_DELAY);

  CacheStatus old_status = current_status;
  if (is_on_demand) {
    current_status = CacheStatus::LOADING_DEMAND;
  }

  DIR* dir = opendir(folder_path.c_str());
  if (!dir) {
    xSemaphoreGive(loader_mutex);
    return;
  }

  // Build map of files on disk with modification times
  std::map<psram_string, time_t, std::less<psram_string>,
    PSRAM_Allocator<std::pair<const psram_string, time_t>>> files_on_disk;
  struct dirent* entry;
  struct stat st;
  while ((entry = readdir(dir)) != nullptr) {
    psram_string name = entry->d_name;
    if (name.find(".jpg") != psram_string::npos || name.find(".jpeg") != psram_string::npos) {
      psram_string full_path = folder_path + "/" + name;
      if (stat(full_path.c_str(), &st) == 0) {
        files_on_disk[name] = st.st_mtime;
      }
    }
  }
  closedir(dir);

  xSemaphoreTake(cache_mutex, portMAX_DELAY);

  psram_file_map& cached_files = image_cache[folder_path];

  // Remove files from cache that are no longer on disk
  for (auto it = cached_files.begin(); it != cached_files.end();) {
    if (files_on_disk.find(it->first) == files_on_disk.end()) {
      ESP_LOGI(TAG, "Removing deleted file: %s", it->first.c_str());
      psram_used -= it->second.size;
      free(it->second.buffer);
      it = cached_files.erase(it);
    } else {
      ++it;
    }
  }

  // Load new or modified files
  for (const auto& disk_file_pair : files_on_disk) {
    const psram_string& filename = disk_file_pair.first;
    const time_t& disk_mtime = disk_file_pair.second;
    current_loading_file = folder_path + "/" + filename;

    auto cache_it = cached_files.find(filename);
    bool needs_load = false;

    if (cache_it == cached_files.end()) {
      needs_load = true;
    } else if (disk_mtime > cache_it->second.mtime) {
      ESP_LOGI(TAG, "Updating modified file: %s", filename.c_str());
      psram_used -= cache_it->second.size;
      free(cache_it->second.buffer);
      cached_files.erase(cache_it);
      needs_load = true;
    }

    if (needs_load) {
      xSemaphoreGive(cache_mutex);

      psram_string full_path = folder_path + "/" + filename;
      stat(full_path.c_str(), &st);
      size_t size = st.st_size;

      if (psram_used + size > psram_limit) {
        ESP_LOGW(TAG, "PSRAM limit reached, cannot load %s", filename.c_str());
        xSemaphoreTake(cache_mutex, portMAX_DELAY);
        continue;
      }

      FILE* file = fopen(full_path.c_str(), "rb");
      uint8_t* buffer = nullptr;
      if (file) {
        buffer = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (buffer) {
          fread(buffer, 1, size, file);
        }
        fclose(file);
      }

      xSemaphoreTake(cache_mutex, portMAX_DELAY);
      if (buffer) {
        cached_files[filename] = { buffer, size, disk_mtime };
        psram_used += size;
        ESP_LOGI(TAG, "Loaded: %s (%u bytes)", filename.c_str(), size);
      } else {
        ESP_LOGE(TAG, "Failed to load file: %s", filename.c_str());
      }

      // Yield to other tasks - shorter delay for on-demand
      if (!is_on_demand) {
        xSemaphoreGive(cache_mutex);
        vTaskDelay(pdMS_TO_TICKS(20));
        xSemaphoreTake(cache_mutex, portMAX_DELAY);
      }
    }
  }

  // Update file list cache to stay in sync
  psram_string_vector sorted_names;
  for (const auto& pair : files_on_disk) {
    sorted_names.push_back(pair.first);
  }
  std::sort(sorted_names.begin(), sorted_names.end());
  folder_file_lists[folder_path] = std::move(sorted_names);

  xSemaphoreGive(cache_mutex);

  if (is_on_demand) {
    current_status = old_status;
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    current_loading_file = "";
    xSemaphoreGive(cache_mutex);
  }

  xSemaphoreGive(loader_mutex);
}

ImageData* ImageCacheManager::_getImageByIndex(const psram_string& folder_path, size_t index) {
  // Must be called with cache_mutex held

  // First try to use file list for consistent ordering
  auto list_it = folder_file_lists.find(folder_path);
  auto cache_it = image_cache.find(folder_path);

  if (list_it != folder_file_lists.end() && cache_it != image_cache.end()) {
    if (index < list_it->second.size()) {
      const psram_string& filename = list_it->second[index];
      auto file_it = cache_it->second.find(filename);
      if (file_it != cache_it->second.end()) {
        return &file_it->second;
      }
    }
  }

  // Fallback to map iteration (original behavior)
  if (cache_it != image_cache.end()) {
    if (index < cache_it->second.size()) {
      auto map_it = cache_it->second.begin();
      std::advance(map_it, index);
      return &map_it->second;
    }
  }

  return nullptr;
}

void ImageCacheManager::clearCache() {
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  for (auto& pair : image_cache) {
    for (auto& file_pair : pair.second) {
      free(file_pair.second.buffer);
    }
  }
  image_cache.clear();
  folder_file_lists.clear();
  pending_sync_folders.clear();
  psram_used = 0;
  xSemaphoreGive(cache_mutex);
  ESP_LOGI(TAG, "Image cache cleared.");
}