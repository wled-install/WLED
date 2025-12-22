#pragma once

#include <vector>
#include <string>
#include <map>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "PSRAM_Allocator.h"

#ifndef IMAGECACHE_BG_PRIORITY
#define IMAGECACHE_BG_PRIORITY 5
#endif

enum class CacheStatus {
  IDLE,
  PRELOADING_BG,
  LOADING_DEMAND
};

typedef struct {
  uint8_t* buffer;
  size_t size;
  time_t mtime;
} ImageData;

// Result struct for streaming API
struct ImageResult {
  uint8_t* buffer;
  size_t size;
  bool from_cache;    // true if from PSRAM cache
  bool needs_free;    // caller must free buffer if true
};

using psram_string = std::basic_string<char, std::char_traits<char>, PSRAM_Allocator<char>>;
using psram_file_map = std::map<psram_string, ImageData, std::less<psram_string>, PSRAM_Allocator<std::pair<const psram_string, ImageData>>>;
using psram_image_map = std::map<psram_string, psram_file_map, std::less<psram_string>, PSRAM_Allocator<std::pair<const psram_string, psram_file_map>>>;
using psram_string_vector = std::vector<psram_string, PSRAM_Allocator<psram_string>>;
using psram_folder_list_map = std::map<psram_string, psram_string_vector, std::less<psram_string>, PSRAM_Allocator<std::pair<const psram_string, psram_string_vector>>>;

class ImageCacheManager {
public:
  static ImageCacheManager& getInstance();

  // Original API (blocking)
  void startPreload(const std::string& root_path);
  ImageData* getImage(const std::string& folder_path, size_t index);
  size_t getFolderSize(const std::string& folder_path);
  void clearCache();

  // New streaming API (non-blocking)
  ImageResult getImageStreaming(const std::string& folder_path, size_t index);
  size_t getFolderSizeFromDisk(const std::string& folder_path);

  // Status functions
  CacheStatus getStatus();
  psram_string getCurrentFile();
  size_t getCacheUsedBytes();

private:
  ImageCacheManager();
  ~ImageCacheManager();
  ImageCacheManager(const ImageCacheManager&) = delete;
  void operator=(const ImageCacheManager&) = delete;

  static void _preloadTask(void* params);
  static void _backgroundSyncTask(void* params);
  void _synchronizeFolder(const psram_string& folder_path, bool is_on_demand);
  void _queueBackgroundSync(const psram_string& folder_path);

  // File list management (lightweight, filenames only)
  bool _ensureFileListCached(const psram_string& folder_path);
  psram_string _getFilenameByIndex(const psram_string& folder_path, size_t index);

  // Main cache: folder -> (filename -> ImageData)
  psram_image_map image_cache;

  // Lightweight file list cache: folder -> sorted filenames
  psram_folder_list_map folder_file_lists;

  // Pending folders for background sync
  psram_string_vector pending_sync_folders;

  ImageData* _getImageByIndex(const psram_string& folder_path, size_t index);

  // Synchronization
  SemaphoreHandle_t cache_mutex;
  SemaphoreHandle_t loader_mutex;
  TaskHandle_t preload_task_handle;
  volatile CacheStatus current_status;
  psram_string current_loading_file;

  // Resource management
  size_t psram_limit;
  size_t psram_used;
  psram_string preload_root_path;
};