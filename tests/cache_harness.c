#include "telemetry_cache.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

static int64_t clock_us;
typedef union { max_align_t alignment; size_t size; } allocation_header_t;
static volatile LONG allocation_index, fail_at, live_allocations;
static void *tracked_malloc(size_t size)
{
    LONG index = InterlockedIncrement(&allocation_index);
    if (fail_at > 0 && index == fail_at) return NULL;
    allocation_header_t *header = malloc(sizeof(*header) + size);
    if (!header) return NULL;
    header->size = size;
    InterlockedIncrement(&live_allocations);
    return header + 1;
}
static void tracked_free(void *pointer)
{
    if (!pointer) return;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    memset(pointer, 0xdd, header->size);
    InterlockedDecrement(&live_allocations);
    free(header);
}
int64_t esp_timer_get_time(void) { return clock_us; }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    CRITICAL_SECTION *lock = malloc(sizeof(*lock));
    if (lock) InitializeCriticalSection(lock);
    return lock;
}
void vSemaphoreDelete(SemaphoreHandle_t lock) { DeleteCriticalSection(lock); free(lock); }
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout) { (void)timeout; EnterCriticalSection(lock); return 1; }
int xSemaphoreGive(SemaphoreHandle_t lock) { LeaveCriticalSection(lock); return 1; }

__declspec(dllexport) int host_init(void)
{
    cJSON_Hooks hooks = {.malloc_fn = tracked_malloc, .free_fn = tracked_free};
    cJSON_InitHooks(&hooks);
    return telemetry_cache_init();
}
__declspec(dllexport) void host_fail_at(long index) { allocation_index = 0; fail_at = index; }
__declspec(dllexport) long host_live_allocations(void) { return live_allocations; }
__declspec(dllexport) long host_allocation_calls(void) { return allocation_index; }
__declspec(dllexport) void host_reset(uint64_t session, int64_t now)
{
    clock_us = now;
    telemetry_cache_reset(session);
}
__declspec(dllexport) int host_accept(unsigned kind, uint64_t session,
                                     const uint8_t *bytes, unsigned length, int64_t received)
{
    lp_frame_t frame = {.kind = kind, .src = 1, .dst = 2, .session = session,
                        .payload = bytes, .payload_len = length};
    return telemetry_cache_accept(&frame, received);
}
__declspec(dllexport) char *host_snapshot(int64_t now)
{
    cJSON *snapshot = telemetry_cache_snapshot(now);
    if (!snapshot) return NULL;
    char *text = cJSON_PrintUnformatted(snapshot);
    cJSON_Delete(snapshot);
    return text;
}
__declspec(dllexport) char *host_live_snapshot(int64_t now)
{
    cJSON *snapshot = telemetry_cache_live_snapshot(now);
    if (!snapshot) return NULL;
    char *text = cJSON_PrintUnformatted(snapshot);
    cJSON_Delete(snapshot);
    return text;
}
__declspec(dllexport) void host_free(void *value) { cJSON_free(value); }
