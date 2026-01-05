// particle_simd.h
// ESP32-P4 PIE SIMD functions for particle system

#ifndef PARTICLE_SIMD_H
#define PARTICLE_SIMD_H

#include <stdint.h>
#include "wled.h"

#ifdef __cplusplus
extern "C" {
  #endif

  // Check for P4 PIE support
  #if defined(CONFIG_IDF_TARGET_ESP32P4) || defined(ESP32P4)
  #define HAS_PIE_SIMD 1
  #else
  #define HAS_PIE_SIMD 0
  #endif

  #if HAS_PIE_SIMD

  //------------------------------------------------------------------------------
  // Framebuffer operations (work with current AoS CRGB layout)
  //------------------------------------------------------------------------------

  /**
   * @brief Scale all pixels in buffer by scale/256
   * @param buffer Pointer to CRGB buffer (must be 16-byte aligned)
   * @param scale Scale factor 0-255 (255 = no change, 0 = black)
   * @param num_pixels Number of pixels in buffer
   */
  void fast_color_scale_simd(void* buffer, uint8_t scale, uint32_t num_pixels);

  /**
   * @brief Add src to dest with saturation
   * @param dest Destination buffer
   * @param src Source buffer
   * @param num_pixels Number of pixels
   */
  void fast_color_add_simd(void* dest, const void* src, uint32_t num_pixels);

  /**
   * @brief Add scaled src to dest with saturation
   * @param dest Destination buffer
   * @param src Source buffer
   * @param num_pixels Number of pixels
   * @param scale Scale factor for src (0-255)
   */
  void fast_color_add_scaled_simd(void* dest, const void* src, uint32_t num_pixels, uint8_t scale);

  /**
   * @brief Blend two buffers: dest = (dest * (255-alpha) + src * alpha) >> 8
   * @param dest Destination buffer
   * @param src Source buffer
   * @param num_pixels Number of pixels
   * @param alpha Blend factor (0 = keep dest, 255 = use src)
   */
  void blend_buffers_simd(void* dest, const void* src, uint32_t num_pixels, uint8_t alpha);

  /**
   * @brief Clear buffer to zero
   * @param buffer Buffer to clear
   * @param num_bytes Number of bytes to clear
   */
  void clear_buffer_simd(void* buffer, uint32_t num_bytes);

  /**
   * @brief Fast buffer copy
   * @param dest Destination buffer
   * @param src Source buffer
   * @param num_bytes Number of bytes to copy
   */
  void copy_buffer_simd(void* dest, const void* src, uint32_t num_bytes);

  //------------------------------------------------------------------------------
  // Structure of Arrays particle operations
  // These require particle data to be stored in separate contiguous arrays
  //------------------------------------------------------------------------------

  /**
   * @brief Apply gravity to velocity array (vy -= dv with saturation)
   * @param vy_array Pointer to contiguous vy values (int8_t)
   * @param dv Gravity delta (positive = downward)
   * @param num_particles Number of particles
   */
  void apply_gravity_simd(int8_t* vy_array, int8_t dv, uint32_t num_particles);

  /**
   * @brief Apply reverse gravity (vy += dv with saturation)
   * @param vy_array Pointer to contiguous vy values
   * @param dv Gravity delta
   * @param num_particles Number of particles
   */
  void apply_gravity_reverse_simd(int8_t* vy_array, int8_t dv, uint32_t num_particles);

  /**
   * @brief Apply friction to velocity array (v = v * friction >> 8)
   * @param v_array Pointer to contiguous velocity values (vx or vy)
   * @param friction Friction coefficient (255 = no friction, 0 = instant stop)
   * @param num_values Number of values
   */
  void apply_friction_simd(int8_t* v_array, uint8_t friction, uint32_t num_values);

  /**
   * @brief Clamp velocities to [-127, 127]
   * @param v_array Pointer to contiguous velocity values
   * @param num_values Number of values
   */
  void limit_speed_simd(int8_t* v_array, uint32_t num_values);

  /**
   * @brief Add velocity to position (pos += vel)
   * @param pos_array Pointer to positions (int16_t)
   * @param vel_array Pointer to velocities (int8_t)
   * @param num_particles Number of particles
   */
  void add_position_simd(int16_t* pos_array, const int8_t* vel_array, uint32_t num_particles);

  /**
   * @brief Decrement TTL values by 1, stopping at 0
   * @param ttl_array Pointer to TTL values (uint16_t)
   * @param num_particles Number of particles
   */
  void decrement_ttl_simd(uint16_t* ttl_array, uint32_t num_particles);

  #else // !HAS_PIE_SIMD

  //------------------------------------------------------------------------------
  // Fallback scalar implementations for non-P4 targets
  //------------------------------------------------------------------------------

  static inline void fast_color_scale_simd(void* buffer, uint8_t scale, uint32_t num_pixels) {
    uint8_t* p = (uint8_t*)buffer;
    uint32_t num_bytes = num_pixels * 3;
    for (uint32_t i = 0; i < num_bytes; i++) {
      p[i] = (p[i] * scale) >> 8;
    }
  }

  static inline void fast_color_add_simd(void* dest, const void* src, uint32_t num_pixels) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    uint32_t num_bytes = num_pixels * 3;
    for (uint32_t i = 0; i < num_bytes; i++) {
      uint32_t sum = d[i] + s[i];
      d[i] = (sum > 255) ? 255 : sum;
    }
  }

  static inline void fast_color_add_scaled_simd(void* dest, const void* src, uint32_t num_pixels, uint8_t scale) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    uint32_t num_bytes = num_pixels * 3;
    for (uint32_t i = 0; i < num_bytes; i++) {
      uint32_t scaled = (s[i] * scale) >> 8;
      uint32_t sum = d[i] + scaled;
      d[i] = (sum > 255) ? 255 : sum;
    }
  }

  static inline void clear_buffer_simd(void* buffer, uint32_t num_bytes) {
    memset(buffer, 0, num_bytes);
  }

  static inline void copy_buffer_simd(void* dest, const void* src, uint32_t num_bytes) {
    memcpy(dest, src, num_bytes);
  }

  static inline void apply_gravity_simd(int8_t* vy_array, int8_t dv, uint32_t num_particles) {
    for (uint32_t i = 0; i < num_particles; i++) {
      int32_t v = vy_array[i] - dv;
      vy_array[i] = (v < -127) ? -127 : ((v > 127) ? 127 : v);
    }
  }

  static inline void apply_friction_simd(int8_t* v_array, uint8_t friction, uint32_t num_values) {
    for (uint32_t i = 0; i < num_values; i++) {
      v_array[i] = (v_array[i] * friction) >> 8;
    }
  }

  static inline void limit_speed_simd(int8_t* v_array, uint32_t num_values) {
    for (uint32_t i = 0; i < num_values; i++) {
      if (v_array[i] > 127) v_array[i] = 127;
      else if (v_array[i] < -127) v_array[i] = -127;
    }
  }

  #endif // HAS_PIE_SIMD

  #ifdef __cplusplus
}
#endif

#endif // PARTICLE_SIMD_H