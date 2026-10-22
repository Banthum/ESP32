/* LVGL-geheugen in PSRAM (LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM).
 *
 * Kopie van LVGL's eigen clib-variant (src/stdlib/clib/lv_mem_core_clib.c),
 * alleen met heap_caps_* i.p.v. malloc/free: de EMS-UI maakt honderden kleine
 * objecten, die met gewone malloc allemaal in het interne RAM landden - daarna
 * kon WiFi niet meer starten ("wifi:alloc pp wdev funcs fail"). De
 * tekenbuffers zelf staan los daarvan in intern RAM (zie board.cpp). */
#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <esp_heap_caps.h>

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void * lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, PSRAM_CAPS);
}

void * lv_realloc_core(void * p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, PSRAM_CAPS);
}

void lv_free_core(void * p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif
