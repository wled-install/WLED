# particle_simd.S
# ESP32-P4 PIE SIMD functions for particle system
# Compile with: -march=rv32imafc_zicsr_zifencei_xesppie -mabi=ilp32f

.text

# ==============================================================================
# Configuration macro - call at start of SIMD functions
# ==============================================================================
.macro PIE_INIT
    esp.movx.r.cfg  t6
    or              t6, t6, 2           # Enable aligned data access
    esp.movx.w.cfg  t6
.endm

.macro PIE_INIT_MUL8
    PIE_INIT
    li              t6, 8
    esp.movx.w.sar  t6                  # Set shift amount to 8 for multiply
.endm

# ==============================================================================
# fast_color_scale_simd
# Scales all pixels in buffer by scale/256
# 
# void fast_color_scale_simd(CRGB* buffer, uint8_t scale, uint32_t num_pixels);
# a0 = buffer pointer
# a1 = scale (0-255)
# a2 = number of pixels
# ==============================================================================
.align 4
.global fast_color_scale_simd
.type   fast_color_scale_simd,@function
fast_color_scale_simd:
    # Calculate num_chunks = (num_pixels * 3) / 16
    li              t0, 3
    mul             a2, a2, t0          # a2 = num_pixels * 3 (bytes)
    srli            a2, a2, 4           # a2 = num_bytes / 16
    beqz            a2, .Lscale_exit
    
    # Check for scale == 255 (no-op) or scale == 0 (zero fill)
    li              t0, 255
    beq             a1, t0, .Lscale_exit
    beqz            a1, .Lscale_zero
    
    PIE_INIT_MUL8
    
    # Create broadcast vector by filling stack and loading
    addi            sp, sp, -16
    sb              a1, 0(sp)
    sb              a1, 1(sp)
    sb              a1, 2(sp)
    sb              a1, 3(sp)
    sb              a1, 4(sp)
    sb              a1, 5(sp)
    sb              a1, 6(sp)
    sb              a1, 7(sp)
    sb              a1, 8(sp)
    sb              a1, 9(sp)
    sb              a1, 10(sp)
    sb              a1, 11(sp)
    sb              a1, 12(sp)
    sb              a1, 13(sp)
    sb              a1, 14(sp)
    sb              a1, 15(sp)
    # Use a3 as temp pointer (a register for PIE load)
    mv              a3, sp
    esp.vld.128.ip  q1, a3, 0           # Load the broadcast vector
    
.Lscale_loop:
    esp.vld.128.ip  q0, a0, 0           # Load 16 bytes (don't advance yet)
    esp.vmul.u8     q2, q0, q1          # q2 = pixels * scale >> 8
    esp.vst.128.ip  q2, a0, 16          # Store back, advance pointer
    addi            a2, a2, -1
    bnez            a2, .Lscale_loop
    addi            sp, sp, 16
    ret

.Lscale_zero:
    PIE_INIT
    esp.zero.q      q0                  # Zero register
.Lscale_zero_loop:
    esp.vst.128.ip  q0, a0, 16          # Store zeros, advance
    addi            a2, a2, -1
    bnez            a2, .Lscale_zero_loop
    
.Lscale_exit:
    ret


# ==============================================================================
# fast_color_add_simd
# Adds src to dest with saturation: dest = min(255, dest + src)
#
# void fast_color_add_simd(CRGB* dest, const CRGB* src, uint32_t num_pixels);
# a0 = dest buffer
# a1 = src buffer  
# a2 = number of pixels
# ==============================================================================
.align 4
.global fast_color_add_simd
.type   fast_color_add_simd,@function
fast_color_add_simd:
    # Calculate num_chunks = (num_pixels * 3) / 16
    li              t0, 3
    mul             a2, a2, t0
    srli            a2, a2, 4
    beqz            a2, .Ladd_exit
    
    PIE_INIT

.Ladd_loop:
    esp.vld.128.ip  q0, a0, 0           # Load dest (don't advance yet)
    esp.vld.128.ip  q1, a1, 16          # Load src, advance src pointer
    esp.vadd.u8     q2, q0, q1          # q2 = dest + src (wrapping)
    # Saturate: if result < either operand, it overflowed
    esp.vcmp.lt.u8  q4, q2, q0          # q4[i] = 0xFF if q2[i] < q0[i] (overflow)
    esp.orq         q2, q2, q4          # Set overflowed bytes to 0xFF
    esp.vst.128.ip  q2, a0, 16          # Store to dest, advance dest pointer
    addi            a2, a2, -1
    bnez            a2, .Ladd_loop
    
.Ladd_exit:
    ret


# ==============================================================================
# fast_color_add_scaled_simd
# Adds scaled src to dest: dest = min(255, dest + (src * scale >> 8))
#
# void fast_color_add_scaled_simd(CRGB* dest, const CRGB* src, uint32_t num_pixels, uint8_t scale);
# a0 = dest buffer
# a1 = src buffer
# a2 = number of pixels
# a3 = scale (0-255)
# ==============================================================================
.align 4
.global fast_color_add_scaled_simd
.type   fast_color_add_scaled_simd,@function
fast_color_add_scaled_simd:
    # Calculate num_chunks
    li              t0, 3
    mul             a2, a2, t0
    srli            a2, a2, 4
    beqz            a2, .Ladd_scaled_exit
    
    # Check for scale == 0 (nothing to add)
    beqz            a3, .Ladd_scaled_exit
    
    # Check for scale == 255 (no scaling needed)
    li              t0, 255
    beq             a3, t0, .Ladd_scaled_noscale
    
    PIE_INIT_MUL8
    
    # Save a1 (src) to t1, use a3 for scale vector address, then restore
    mv              t1, a1              # Save src pointer
    
    # Create broadcast scale vector on stack
    addi            sp, sp, -16
    sb              a3, 0(sp)
    sb              a3, 1(sp)
    sb              a3, 2(sp)
    sb              a3, 3(sp)
    sb              a3, 4(sp)
    sb              a3, 5(sp)
    sb              a3, 6(sp)
    sb              a3, 7(sp)
    sb              a3, 8(sp)
    sb              a3, 9(sp)
    sb              a3, 10(sp)
    sb              a3, 11(sp)
    sb              a3, 12(sp)
    sb              a3, 13(sp)
    sb              a3, 14(sp)
    sb              a3, 15(sp)
    mv              a3, sp
    esp.vld.128.ip  q3, a3, 0           # q3 = scale broadcast
    mv              a1, t1              # Restore src pointer

.Ladd_scaled_loop:
    esp.vld.128.ip  q0, a0, 0           # Load dest
    esp.vld.128.ip  q1, a1, 16          # Load src, advance
    esp.vmul.u8     q2, q1, q3          # q2 = src * scale >> 8
    esp.vadd.u8     q2, q0, q2          # q2 = dest + scaled_src (may wrap)
    # Saturate: if q2 < q0, overflow occurred
    esp.vcmp.lt.u8  q4, q2, q0          # q4 = mask where overflow
    esp.orq         q2, q2, q4          # Set overflowed bytes to 0xFF
    esp.vst.128.ip  q2, a0, 16          # Store to dest, advance
    addi            a2, a2, -1
    bnez            a2, .Ladd_scaled_loop
    addi            sp, sp, 16
    ret

.Ladd_scaled_noscale:
    PIE_INIT
.Ladd_scaled_noscale_loop:
    esp.vld.128.ip  q0, a0, 0           # Load dest
    esp.vld.128.ip  q1, a1, 16          # Load src, advance
    esp.vadd.u8     q2, q0, q1          # q2 = dest + src
    esp.vcmp.lt.u8  q4, q2, q0          # Detect overflow
    esp.orq         q2, q2, q4          # Saturate to 255
    esp.vst.128.ip  q2, a0, 16          # Store, advance
    addi            a2, a2, -1
    bnez            a2, .Ladd_scaled_noscale_loop
    
.Ladd_scaled_exit:
    ret


# ==============================================================================
# apply_gravity_simd
# Subtracts dv from all velocity values, clamped to [-127, 127]
# For Structure of Arrays layout
#
# void apply_gravity_simd(int8_t* vy_array, int8_t dv, uint32_t num_particles);
# a0 = vy array pointer
# a1 = dv (gravity delta)
# a2 = number of particles
# ==============================================================================
.align 4
.global apply_gravity_simd
.type   apply_gravity_simd,@function
apply_gravity_simd:
    srli            a2, a2, 4           # num_chunks = num_particles / 16
    beqz            a2, .Lgrav_exit
    beqz            a1, .Lgrav_exit     # Nothing to do if dv == 0
    
    PIE_INIT
    
    # Create broadcast vectors on stack: dv, 127, -127
    addi            sp, sp, -48
    
    # Fill dv vector at sp+0
    sb              a1, 0(sp)
    sb              a1, 1(sp)
    sb              a1, 2(sp)
    sb              a1, 3(sp)
    sb              a1, 4(sp)
    sb              a1, 5(sp)
    sb              a1, 6(sp)
    sb              a1, 7(sp)
    sb              a1, 8(sp)
    sb              a1, 9(sp)
    sb              a1, 10(sp)
    sb              a1, 11(sp)
    sb              a1, 12(sp)
    sb              a1, 13(sp)
    sb              a1, 14(sp)
    sb              a1, 15(sp)
    
    # Fill 127 vector at sp+16
    li              t0, 127
    sb              t0, 16(sp)
    sb              t0, 17(sp)
    sb              t0, 18(sp)
    sb              t0, 19(sp)
    sb              t0, 20(sp)
    sb              t0, 21(sp)
    sb              t0, 22(sp)
    sb              t0, 23(sp)
    sb              t0, 24(sp)
    sb              t0, 25(sp)
    sb              t0, 26(sp)
    sb              t0, 27(sp)
    sb              t0, 28(sp)
    sb              t0, 29(sp)
    sb              t0, 30(sp)
    sb              t0, 31(sp)
    
    # Fill -127 vector at sp+32
    li              t0, -127
    sb              t0, 32(sp)
    sb              t0, 33(sp)
    sb              t0, 34(sp)
    sb              t0, 35(sp)
    sb              t0, 36(sp)
    sb              t0, 37(sp)
    sb              t0, 38(sp)
    sb              t0, 39(sp)
    sb              t0, 40(sp)
    sb              t0, 41(sp)
    sb              t0, 42(sp)
    sb              t0, 43(sp)
    sb              t0, 44(sp)
    sb              t0, 45(sp)
    sb              t0, 46(sp)
    sb              t0, 47(sp)
    
    # Load vectors using a3-a5 as address registers
    mv              a3, sp
    esp.vld.128.ip  q1, a3, 16          # q1 = dv broadcast, a3 advances to 127 vector
    esp.vld.128.ip  q4, a3, 16          # q4 = 127, a3 advances to -127 vector
    esp.vld.128.ip  q5, a3, 0           # q5 = -127

.Lgrav_loop:
    esp.vld.128.ip  q0, a0, 0           # Load 16 velocities
    esp.vsub.s8     q2, q0, q1          # q2 = vy - dv (wrapping)
    esp.vmin.s8     q2, q2, q4          # Clamp to max 127
    esp.vmax.s8     q2, q2, q5          # Clamp to min -127
    esp.vst.128.ip  q2, a0, 16          # Store back, advance
    addi            a2, a2, -1
    bnez            a2, .Lgrav_loop
    addi            sp, sp, 48
    
.Lgrav_exit:
    ret


# ==============================================================================
# apply_friction_simd
# Multiplies all velocities by friction/256 (for SoA layout)
#
# void apply_friction_simd(int8_t* v_array, uint8_t friction, uint32_t num_values);
# a0 = velocity array pointer
# a1 = friction coefficient (255 = no friction)
# a2 = number of values
# ==============================================================================
.align 4
.global apply_friction_simd
.type   apply_friction_simd,@function
apply_friction_simd:
    srli            a2, a2, 4
    beqz            a2, .Lfriction_exit
    
    # Check for friction == 255 (no friction)
    li              t0, 255
    beq             a1, t0, .Lfriction_exit
    
    PIE_INIT_MUL8
    
    # Create broadcast friction vector
    addi            sp, sp, -16
    sb              a1, 0(sp)
    sb              a1, 1(sp)
    sb              a1, 2(sp)
    sb              a1, 3(sp)
    sb              a1, 4(sp)
    sb              a1, 5(sp)
    sb              a1, 6(sp)
    sb              a1, 7(sp)
    sb              a1, 8(sp)
    sb              a1, 9(sp)
    sb              a1, 10(sp)
    sb              a1, 11(sp)
    sb              a1, 12(sp)
    sb              a1, 13(sp)
    sb              a1, 14(sp)
    sb              a1, 15(sp)
    mv              a3, sp
    esp.vld.128.ip  q1, a3, 0

.Lfriction_loop:
    esp.vld.128.ip  q0, a0, 0
    esp.vmul.s8     q2, q0, q1          # Signed multiply for velocities
    esp.vst.128.ip  q2, a0, 16
    addi            a2, a2, -1
    bnez            a2, .Lfriction_loop
    addi            sp, sp, 16
    
.Lfriction_exit:
    ret


# ==============================================================================
# limit_speed_simd
# Clamps all velocities to range [-127, 127]
#
# void limit_speed_simd(int8_t* v_array, uint32_t num_values);
# a0 = velocity array pointer
# a1 = number of values
# ==============================================================================
.align 4
.global limit_speed_simd
.type   limit_speed_simd,@function
limit_speed_simd:
    srli            a1, a1, 4
    beqz            a1, .Llimit_exit
    
    PIE_INIT
    
    # Create 127 and -127 vectors on stack
    addi            sp, sp, -32
    li              t0, 127
    sb              t0, 0(sp)
    sb              t0, 1(sp)
    sb              t0, 2(sp)
    sb              t0, 3(sp)
    sb              t0, 4(sp)
    sb              t0, 5(sp)
    sb              t0, 6(sp)
    sb              t0, 7(sp)
    sb              t0, 8(sp)
    sb              t0, 9(sp)
    sb              t0, 10(sp)
    sb              t0, 11(sp)
    sb              t0, 12(sp)
    sb              t0, 13(sp)
    sb              t0, 14(sp)
    sb              t0, 15(sp)
    
    li              t0, -127
    sb              t0, 16(sp)
    sb              t0, 17(sp)
    sb              t0, 18(sp)
    sb              t0, 19(sp)
    sb              t0, 20(sp)
    sb              t0, 21(sp)
    sb              t0, 22(sp)
    sb              t0, 23(sp)
    sb              t0, 24(sp)
    sb              t0, 25(sp)
    sb              t0, 26(sp)
    sb              t0, 27(sp)
    sb              t0, 28(sp)
    sb              t0, 29(sp)
    sb              t0, 30(sp)
    sb              t0, 31(sp)
    
    mv              a3, sp
    esp.vld.128.ip  q2, a3, 16          # q2 = all 127s
    esp.vld.128.ip  q3, a3, 0           # q3 = all -127s

.Llimit_loop:
    esp.vld.128.ip  q0, a0, 0           # Load 16 velocities
    esp.vmin.s8     q1, q0, q2          # Clamp to max 127
    esp.vmax.s8     q1, q1, q3          # Clamp to min -127
    esp.vst.128.ip  q1, a0, 16          # Store back, advance
    addi            a1, a1, -1
    bnez            a1, .Llimit_loop
    addi            sp, sp, 32
    
.Llimit_exit:
    ret


# ==============================================================================
# clear_buffer_simd
# Zeros a buffer quickly
#
# void clear_buffer_simd(void* buffer, uint32_t num_bytes);
# a0 = buffer pointer
# a1 = number of bytes
# ==============================================================================
.align 4
.global clear_buffer_simd
.type   clear_buffer_simd,@function
clear_buffer_simd:
    srli            a1, a1, 4
    beqz            a1, .Lclear_exit
    
    PIE_INIT
    esp.zero.q      q0

.Lclear_loop:
    esp.vst.128.ip  q0, a0, 16
    addi            a1, a1, -1
    bnez            a1, .Lclear_loop
    
.Lclear_exit:
    ret


# ==============================================================================
# copy_buffer_simd
# Fast buffer copy
#
# void copy_buffer_simd(void* dest, const void* src, uint32_t num_bytes);
# a0 = dest pointer
# a1 = src pointer
# a2 = number of bytes
# ==============================================================================
.align 4
.global copy_buffer_simd
.type   copy_buffer_simd,@function
copy_buffer_simd:
    srli            a2, a2, 4
    beqz            a2, .Lcopy_exit
    
    PIE_INIT

.Lcopy_loop:
    esp.vld.128.ip  q0, a1, 16
    esp.vst.128.ip  q0, a0, 16
    addi            a2, a2, -1
    bnez            a2, .Lcopy_loop
    
.Lcopy_exit:
    ret


# ==============================================================================
# blend_buffers_simd
# Blends two buffers: dest = (dest * (255-alpha) + src * alpha) >> 8
#
# void blend_buffers_simd(CRGB* dest, const CRGB* src, uint32_t num_pixels, uint8_t alpha);
# a0 = dest buffer
# a1 = src buffer
# a2 = number of pixels
# a3 = alpha (0-255)
# ==============================================================================
.align 4
.global blend_buffers_simd
.type   blend_buffers_simd,@function
blend_buffers_simd:
    li              t0, 3
    mul             a2, a2, t0
    srli            a2, a2, 4
    beqz            a2, .Lblend_exit
    
    # Check edge cases
    beqz            a3, .Lblend_exit        # alpha=0: keep dest unchanged
    li              t0, 255
    beq             a3, t0, .Lblend_copy    # alpha=255: just copy src
    
    PIE_INIT_MUL8
    
    # Save src pointer
    mv              t1, a1
    
    # Create broadcast vectors for alpha and (255-alpha)
    addi            sp, sp, -32
    li              t0, 255
    sub             t2, t0, a3              # t2 = 255 - alpha
    
    # Fill alpha vector at sp+0
    sb              a3, 0(sp)
    sb              a3, 1(sp)
    sb              a3, 2(sp)
    sb              a3, 3(sp)
    sb              a3, 4(sp)
    sb              a3, 5(sp)
    sb              a3, 6(sp)
    sb              a3, 7(sp)
    sb              a3, 8(sp)
    sb              a3, 9(sp)
    sb              a3, 10(sp)
    sb              a3, 11(sp)
    sb              a3, 12(sp)
    sb              a3, 13(sp)
    sb              a3, 14(sp)
    sb              a3, 15(sp)
    
    # Fill inverse alpha vector at sp+16
    sb              t2, 16(sp)
    sb              t2, 17(sp)
    sb              t2, 18(sp)
    sb              t2, 19(sp)
    sb              t2, 20(sp)
    sb              t2, 21(sp)
    sb              t2, 22(sp)
    sb              t2, 23(sp)
    sb              t2, 24(sp)
    sb              t2, 25(sp)
    sb              t2, 26(sp)
    sb              t2, 27(sp)
    sb              t2, 28(sp)
    sb              t2, 29(sp)
    sb              t2, 30(sp)
    sb              t2, 31(sp)
    
    mv              a3, sp
    esp.vld.128.ip  q4, a3, 16              # q4 = alpha
    esp.vld.128.ip  q5, a3, 0               # q5 = 255 - alpha
    mv              a1, t1                  # Restore src pointer

.Lblend_loop:
    esp.vld.128.ip  q0, a0, 0               # Load dest
    esp.vld.128.ip  q1, a1, 16              # Load src, advance
    esp.vmul.u8     q2, q0, q5              # q2 = dest * (255-alpha) >> 8
    esp.vmul.u8     q3, q1, q4              # q3 = src * alpha >> 8
    esp.vadd.u8     q2, q2, q3              # q2 = blended (won't overflow due to scaling)
    esp.vst.128.ip  q2, a0, 16              # Store, advance
    addi            a2, a2, -1
    bnez            a2, .Lblend_loop
    addi            sp, sp, 32
    ret

.Lblend_copy:
    PIE_INIT
.Lblend_copy_loop:
    esp.vld.128.ip  q0, a1, 16
    esp.vst.128.ip  q0, a0, 16
    addi            a2, a2, -1
    bnez            a2, .Lblend_copy_loop
    
.Lblend_exit:
    ret