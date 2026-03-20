// kbelf platform callbacks for Graceloader
// Adapted from tanmatsu-launcher badge-elf component

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "hal/cache_hal.h"
#include "soc/soc.h"
#include "kbelf.h"
#include "fastopen.h"

static char const TAG[] = "kbelfx";

// String helpers required by kbelf
size_t kbelfq_strlen(char const* str) {
    return strlen(str);
}

void kbelfq_strcpy(char* dst, char const* src) {
    strcpy(dst, src);
}

char const* kbelfq_strrchr(char const* str, char c) {
    return strrchr(str, c);
}

bool kbelfq_streq(char const* a, char const* b) {
    return !strcmp(a, b);
}

void kbelfq_memcpy(void* dst, void const* src, size_t nmemb) {
    memcpy(dst, src, nmemb);
}

void kbelfq_memset(void* dst, uint8_t c, size_t nmemb) {
    memset(dst, c, nmemb);
}

bool kbelfq_memeq(void const* a, void const* b, size_t nmemb) {
    return !memcmp(a, b, nmemb);
}

// Memory allocator for metadata — prefer SPIRAM for large allocations
void* kbelfx_malloc(size_t len) {
    if (len > 1024) {
        void* mem = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (mem) return mem;
    }
    return malloc(len);
}

void* kbelfx_realloc(void* mem, size_t len) {
    return realloc(mem, len);
}

void kbelfx_free(void* mem) {
    free(mem);
}

// Segment allocator for loading program segments into memory
bool kbelfx_seg_alloc(kbelf_inst inst, size_t segs_len, kbelf_segment* segs) {
    size_t min_va = SIZE_MAX, max_va = 0, min_align = 16;
    for (size_t i = 0; i < segs_len; i++) {
        if (min_align < segs[i].alignment) min_align = segs[i].alignment;
        if (min_va > segs[i].vaddr_req) min_va = segs[i].vaddr_req;
        if (max_va < segs[i].vaddr_req + segs[i].size) max_va = segs[i].vaddr_req + segs[i].size;
    }

    size_t alloc_size = max_va - min_va;
    void*  memory     = aligned_alloc(min_align, alloc_size);

    if (!memory) {
        ESP_LOGE(TAG, "seg_alloc: FAILED to allocate %u bytes", (unsigned)alloc_size);
        return false;
    }
    segs[0].alloc_cookie = memory;

    size_t offset = (size_t)memory - min_va;
    for (size_t i = 0; i < segs_len; i++) {
        segs[i].laddr = segs[i].vaddr_real = segs[i].vaddr_req + offset;
    }

    return true;
}

void kbelfx_seg_free(kbelf_inst inst, size_t segs_len, kbelf_segment* segs) {
    (void)inst;
    (void)segs_len;
    free(segs[0].alloc_cookie);
}

// File I/O using fastopen for DMA-optimized reads from /sd and /int
void* kbelfx_open(char const* path) {
    return fastopen(path, "rb");
}

void kbelfx_close(void* fd) {
    fastclose(fd);
}

long kbelfx_read(void* fd, void* buf, long buf_len) {
    return fread(buf, 1, buf_len, fd);
}

long kbelfx_load(kbelf_inst inst, void* fd, kbelf_laddr laddr, kbelf_laddr file_size, kbelf_laddr mem_size) {
    (void)inst;
    ESP_LOGI(TAG, "Loading 0x%x bytes from 0x%x to %p", (int)file_size, (int)ftell(fd), (void*)laddr);
    memset((void*)(laddr + file_size), 0, mem_size - file_size);
    return fread((void*)laddr, 1, file_size, fd);
}

int kbelfx_seek(void* fd, long pos) {
    return fseek(fd, pos, SEEK_SET);
}

// Cache synchronization — CRITICAL for RISC-V I-cache/D-cache coherence
// On ESP32-P4, the I-cache and D-cache are not coherent. When kbelf loads
// ELF code via fread/memset, it writes through the D-cache, but the I-cache
// doesn't see those writes. Without explicit writeback + invalidation,
// the CPU will execute stale/zero data from I-cache, causing crashes.
void kbelfx_cache_sync(kbelf_laddr addr, size_t size) {
#if SOC_CACHE_WRITEBACK_SUPPORTED
    cache_hal_writeback_addr((uint32_t)addr, size);
    cache_hal_invalidate_addr((uint32_t)addr, size);
#endif
}

bool kbelfx_copy_from_user(kbelf_inst inst, void* buf, kbelf_laddr laddr, size_t len) {
    (void)inst;
    memcpy(buf, (void const*)laddr, len);
    return true;
}

bool kbelfx_copy_to_user(kbelf_inst inst, kbelf_laddr laddr, void* buf, size_t len) {
    (void)inst;
    memcpy((void*)laddr, buf, len);
    return true;
}

ptrdiff_t kbelfx_strlen_from_user(kbelf_inst inst, kbelf_laddr laddr) {
    (void)inst;
    return (ptrdiff_t)strlen((char const*)laddr);
}

kbelf_file kbelfx_find_lib(char const* needed) {
    return kbelf_file_open(needed, NULL);
}

// Built-in library — exports ALL graceloader symbols (ROM + own code)
// Weak references mean: zero cost, missing symbols resolve to NULL
extern kbelf_builtin_lib const app_elf_lib_all;

kbelf_builtin_lib const* kbelfx_builtin_libs[] = {
    &app_elf_lib_all,
};

size_t kbelfx_builtin_libs_len = sizeof(kbelfx_builtin_libs) / sizeof(void*);
