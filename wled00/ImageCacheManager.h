#ifndef IMAGE_CACHE_MANAGER_H
#define IMAGE_CACHE_MANAGER_H

#include <string>
#include <map>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <ctime>

#ifndef IMAGECACHE_BG_PRIORITY
#define IMAGECACHE_BG_PRIORITY 1
#endif

// PSRAM allocator for STL containers
template <typename T>
struct PSRAM_Allocator {
  using value_type = T;
  PSRAM_Allocator() = default;
  template <typename U> PSRAM_Allocator(const PSRAM_Allocator<U>&) { }
  T* allocate(size_t n) {
    return static_cast<T*>(heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM));
  }
  void deallocate(T* p, size_t) { free(p); }
  bool operator==(const PSRAM_Allocator&) const { return true; }
  bool operator!=(const PSRAM_Allocator&) const { return false; }
};

using psram_string = std::basic_string<char, std::char_traits<char>, PSRAM_Allocator<char>>;
using psram_string_vector = std::vector<psram_string, PSRAM_Allocator<psram_string>>;

enum class CacheStatus {
  IDLE,
  PRELOADING_BG,
  PRELOADING_BLOCKING
};

struct ImageData {
  uint8_t* buffer;
  size_t size;
  time_t mtime;
};

struct ImageResult {
  uint8_t* buffer;
  size_t size;
  bool from_cache;
  bool needs_free;
};

struct MJPEGFrameInfo {
  size_t offset;
  size_t size;
};

struct CachedMJPEG {
  uint8_t* buffer;
  size_t buffer_size;
  std::vector<MJPEGFrameInfo, PSRAM_Allocator<MJPEGFrameInfo>> frames;
};

using psram_file_map = std::map<psram_string, ImageData, std::less<psram_string>, PSRAM_Allocator<std::pair<const psram_string, ImageData>>>;
using psram_folder_map = std::map<psram_string, psram_file_map, std::less<psram_string>, PSRAM_Allocator<std::pair<const psram_string, psram_file_map>>>;
using psram_filelist_map = std::map<psram_string, psram_string_vector, std::less<psram_string>, PSRAM_Allocator<std::pair<const psram_string, psram_string_vector>>>;
using mjpeg_cache_map = std::map<psram_string, CachedMJPEG, std::less<psram_string>, PSRAM_Allocator<std::pair<const psram_string, CachedMJPEG>>>;

class ImageCacheManager {
public:
  static ImageCacheManager& getInstance();

  // Main API
  size_t getFolderSizeFromDisk(const std::string& folder_path);
  ImageResult getImageStreaming(const std::string& folder_path, size_t index);

  // Preloading
  void startPreload(const std::string& root_path);

  // Status
  CacheStatus getStatus();
  psram_string getCurrentFile();
  size_t getCacheUsedBytes();

  // Management
  void clearCache();

private:
  ImageCacheManager();
  ~ImageCacheManager();
  ImageCacheManager(const ImageCacheManager&) = delete;
  ImageCacheManager& operator=(const ImageCacheManager&) = delete;

  // MJPEG helpers
  bool _isMJPEGFile(const psram_string& path);
  bool _ensureMJPEGCached(const psram_string& file_path);
  size_t _getMJPEGFrameCount(const psram_string& file_path);
  ImageResult _getImageFromMJPEG(const psram_string& file_path, size_t index);

  // Folder helpers
  bool _ensureFileListCached(const psram_string& folder_path);
  psram_string _getFilenameByIndex(const psram_string& folder_path, size_t index);
  ImageData* _getImageByIndex(const psram_string& folder_path, size_t index);

  // Background sync
  void _queueBackgroundSync(const psram_string& path);
  static void _backgroundSyncTask(void* params);
  void _synchronizeFolder(const psram_string& folder_path);

  // Data
  SemaphoreHandle_t cache_mutex;
  TaskHandle_t sync_task_handle;
  CacheStatus current_status;
  psram_string current_loading_file;

  // JPEG folder cache
  psram_folder_map image_cache;
  psram_filelist_map folder_file_lists;
  std::vector<psram_string, PSRAM_Allocator<psram_string>> pending_sync_folders;

  // MJPEG cache
  mjpeg_cache_map mjpeg_cache;

  // Limits
  size_t psram_limit;
  size_t psram_used;
};

#endif // IMAGE_CACHE_MANAGER_H