#ifndef HOST_SEMPHR_H
#define HOST_SEMPHR_H
#include <windows.h>
typedef CRITICAL_SECTION *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t handle);
int xSemaphoreTake(SemaphoreHandle_t handle, unsigned timeout);
int xSemaphoreGive(SemaphoreHandle_t handle);
#endif
