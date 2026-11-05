#pragma once
#include <stdint.h>
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct AHardwareBuffer_Desc {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM 43
#define AHARDWAREBUFFER_USAGE_CPU_READ_RARELY 2ULL
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY 32ULL
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE 256ULL
#define AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT 512ULL
#define AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY 2048ULL
struct ARect;
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *, AHardwareBuffer **);
void AHardwareBuffer_describe(const AHardwareBuffer *, AHardwareBuffer_Desc *);
void AHardwareBuffer_acquire(AHardwareBuffer *);
void AHardwareBuffer_release(AHardwareBuffer *);
int AHardwareBuffer_lock(AHardwareBuffer *, uint64_t, int, const struct ARect *, void **);
int AHardwareBuffer_unlock(AHardwareBuffer *, int *);
