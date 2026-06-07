/*
 * rtc_timer_sched.c - Dynamic min-heap timer scheduler.
 */
#include "rtc_timer_sched.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TIMER_INITIAL_HEAP_CAP 16
#define TIMER_INITIAL_SLOT_CAP 16

static rtc_timer_handle_t timer_make_handle(int slot, uint32_t generation) {
    return ((uint64_t)generation << 32) | (uint32_t)slot;
}

static int timer_handle_slot(rtc_timer_handle_t handle) {
    return (int)(handle & 0xFFFFFFFFu);
}

static uint32_t timer_handle_generation(rtc_timer_handle_t handle) {
    return (uint32_t)(handle >> 32);
}

static bool timer_node_active(const rtc_timer_sched_t *sched, const rtc_timer_sched_node_t *node) {
    return node->slot >= 0 && node->slot < sched->slot_count && sched->active[node->slot] &&
           sched->generations[node->slot] == node->generation;
}

static void heap_swap(rtc_timer_sched_node_t *a, rtc_timer_sched_node_t *b) {
    rtc_timer_sched_node_t tmp = *a;
    *a = *b;
    *b = tmp;
}

static void heap_sift_up(rtc_timer_sched_t *sched, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (sched->heap[parent].deadline_ms <= sched->heap[idx].deadline_ms)
            break;
        heap_swap(&sched->heap[parent], &sched->heap[idx]);
        idx = parent;
    }
}

static void heap_sift_down(rtc_timer_sched_t *sched, int idx) {
    for (;;) {
        int left = idx * 2 + 1;
        int right = left + 1;
        int smallest = idx;
        if (left < sched->heap_count &&
            sched->heap[left].deadline_ms < sched->heap[smallest].deadline_ms) {
            smallest = left;
        }
        if (right < sched->heap_count &&
            sched->heap[right].deadline_ms < sched->heap[smallest].deadline_ms) {
            smallest = right;
        }
        if (smallest == idx)
            break;
        heap_swap(&sched->heap[idx], &sched->heap[smallest]);
        idx = smallest;
    }
}

static rtc_timer_sched_node_t heap_pop(rtc_timer_sched_t *sched) {
    rtc_timer_sched_node_t node = sched->heap[0];
    sched->heap_count--;
    if (sched->heap_count > 0) {
        sched->heap[0] = sched->heap[sched->heap_count];
        heap_sift_down(sched, 0);
    }
    return node;
}

static int ensure_heap_cap(rtc_timer_sched_t *sched) {
    if (sched->heap_count < sched->heap_cap)
        return RTC_OK;
    int new_cap = sched->heap_cap ? sched->heap_cap * 2 : TIMER_INITIAL_HEAP_CAP;
    rtc_timer_sched_node_t *new_heap =
        (rtc_timer_sched_node_t *)realloc(sched->heap, (size_t)new_cap * sizeof(*new_heap));
    if (!new_heap)
        return RTC_ERR_NOMEM;
    sched->heap = new_heap;
    sched->heap_cap = new_cap;
    return RTC_OK;
}

static int ensure_slot_cap(rtc_timer_sched_t *sched) {
    if (sched->slot_count < sched->slot_cap)
        return RTC_OK;
    int old_cap = sched->slot_cap;
    int new_cap = old_cap ? old_cap * 2 : TIMER_INITIAL_SLOT_CAP;

    uint32_t *new_generations =
        (uint32_t *)realloc(sched->generations, (size_t)new_cap * sizeof(*new_generations));
    if (!new_generations)
        return RTC_ERR_NOMEM;
    sched->generations = new_generations;

    bool *new_active = (bool *)realloc(sched->active, (size_t)new_cap * sizeof(*new_active));
    if (!new_active)
        return RTC_ERR_NOMEM;
    sched->active = new_active;

    memset(sched->generations + old_cap, 0,
           (size_t)(new_cap - old_cap) * sizeof(*sched->generations));
    memset(sched->active + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(*sched->active));
    sched->slot_cap = new_cap;
    return RTC_OK;
}

static int alloc_slot(rtc_timer_sched_t *sched) {
    for (int i = 0; i < sched->slot_count; i++) {
        if (!sched->active[i])
            return i;
    }
    if (ensure_slot_cap(sched) != RTC_OK)
        return -1;
    return sched->slot_count++;
}

static void prune_stale_roots(rtc_timer_sched_t *sched) {
    while (sched->heap_count > 0 && !timer_node_active(sched, &sched->heap[0])) {
        (void)heap_pop(sched);
    }
}

int rtc_timer_sched_init(rtc_timer_sched_t *sched) {
    if (!sched)
        return RTC_ERR_INVALID;
    memset(sched, 0, sizeof(*sched));
    return RTC_OK;
}

void rtc_timer_sched_close(rtc_timer_sched_t *sched) {
    if (!sched)
        return;
    free(sched->heap);
    free(sched->generations);
    free(sched->active);
    memset(sched, 0, sizeof(*sched));
}

rtc_timer_handle_t rtc_timer_sched_add(rtc_timer_sched_t *sched, uint64_t deadline_ms,
                                       rtc_timer_sched_fn fn, void *user) {
    if (!sched || !fn)
        return RTC_TIMER_HANDLE_INVALID;
    if (ensure_heap_cap(sched) != RTC_OK)
        return RTC_TIMER_HANDLE_INVALID;
    int slot = alloc_slot(sched);
    if (slot < 0)
        return RTC_TIMER_HANDLE_INVALID;

    sched->generations[slot]++;
    if (sched->generations[slot] == 0)
        sched->generations[slot] = 1;
    sched->active[slot] = true;

    rtc_timer_sched_node_t node;
    node.deadline_ms = deadline_ms;
    node.fn = fn;
    node.user = user;
    node.generation = sched->generations[slot];
    node.slot = slot;

    int idx = sched->heap_count++;
    sched->heap[idx] = node;
    heap_sift_up(sched, idx);
    return timer_make_handle(slot, node.generation);
}

void rtc_timer_sched_cancel(rtc_timer_sched_t *sched, rtc_timer_handle_t handle) {
    if (!sched || handle == RTC_TIMER_HANDLE_INVALID)
        return;
    int slot = timer_handle_slot(handle);
    uint32_t generation = timer_handle_generation(handle);
    if (slot < 0 || slot >= sched->slot_count)
        return;
    if (sched->generations[slot] != generation)
        return;
    sched->active[slot] = false;
}

int rtc_timer_sched_next_timeout_ms(rtc_timer_sched_t *sched, uint64_t now_ms,
                                    int default_timeout_ms) {
    if (!sched)
        return default_timeout_ms;
    prune_stale_roots(sched);
    if (sched->heap_count == 0)
        return default_timeout_ms;
    uint64_t deadline = sched->heap[0].deadline_ms;
    if (now_ms >= deadline)
        return 0;
    uint64_t diff = deadline - now_ms;
    return diff > (uint64_t)INT_MAX ? INT_MAX : (int)diff;
}

void rtc_timer_sched_fire_due(rtc_timer_sched_t *sched, uint64_t now_ms) {
    if (!sched)
        return;
    for (;;) {
        prune_stale_roots(sched);
        if (sched->heap_count == 0 || sched->heap[0].deadline_ms > now_ms)
            return;

        rtc_timer_sched_node_t node = heap_pop(sched);
        if (!timer_node_active(sched, &node))
            continue;
        sched->active[node.slot] = false;
        node.fn(node.user);
    }
}

int rtc_timer_sched_pending_count(rtc_timer_sched_t *sched) {
    if (!sched)
        return 0;
    int count = 0;
    for (int i = 0; i < sched->slot_count; i++) {
        if (sched->active[i])
            count++;
    }
    return count;
}