#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline bool fakeHeapCapsFail = false;
inline void* heap_caps_malloc(size_t size, int) { return fakeHeapCapsFail ? nullptr : malloc(size); }

inline void heap_caps_free(void* p) { free(p); }

inline size_t fakePsramFree = 8 * 1024 * 1024;
inline size_t fakePsramLargest = 8 * 1024 * 1024;
inline size_t heap_caps_get_free_size(int) { return fakePsramFree; }
inline size_t heap_caps_get_largest_free_block(int) { return fakePsramLargest; }
