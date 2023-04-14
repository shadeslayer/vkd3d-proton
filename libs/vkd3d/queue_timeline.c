/*
 * Copyright 2023 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "vkd3d_private.h"
#include "vkd3d_platform.h"
#include "vkd3d_threads.h"
#include <assert.h>
#include <stdio.h>

#define NUM_ENTRIES (16 * 1024)

HRESULT vkd3d_queue_timeline_trace_init(struct vkd3d_queue_timeline_trace *trace)
{
    char env[VKD3D_PATH_MAX];
    unsigned int i;

    if (!vkd3d_get_env_var("VKD3D_QUEUE_PROFILE", env, sizeof(env)))
        return S_OK;

    trace->file = fopen(env, "w");
    if (trace->file)
    {
        INFO("Creating timeline trace in: \"%s\".\n", env);
        fputs("[\n", trace->file);
    }
    else
        return S_OK;

    if (pthread_mutex_init(&trace->lock, NULL))
    {
        if (trace->file)
        {
            fclose(trace->file);
            trace->file = NULL;
        }
        ERR("Failed to initialize mutex.\n");
        /* Not fatal, just ignore. */
        return S_OK;
    }

    vkd3d_array_reserve((void**)&trace->vacant_indices, &trace->vacant_indices_size,
            NUM_ENTRIES, sizeof(*trace->vacant_indices));

    /* Reserve entry 0 as sentinel. */
    for (i = 1; i < NUM_ENTRIES; i++)
        trace->vacant_indices[trace->vacant_indices_count++] = i;

    trace->state = vkd3d_calloc(NUM_ENTRIES, sizeof(*trace->state));
    trace->base_ts = vkd3d_get_current_time_ns();
    trace->active = true;
    return S_OK;
}

static void vkd3d_queue_timeline_trace_free_index(struct vkd3d_queue_timeline_trace *trace, unsigned int index)
{
    assert(trace->state[index].type != VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_NONE);
    trace->state[index].type = VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_NONE;

    pthread_mutex_lock(&trace->lock);
    assert(trace->vacant_indices_count < trace->vacant_indices_size);
    trace->vacant_indices[trace->vacant_indices_count++] = index;
    pthread_mutex_unlock(&trace->lock);
}

static unsigned int vkd3d_queue_timeline_trace_allocate_index(struct vkd3d_queue_timeline_trace *trace, uint64_t *submit_count)
{
    unsigned int index = 0;
    pthread_mutex_lock(&trace->lock);
    if (trace->vacant_indices_count == 0)
    {
        ERR("Failed to allocate queue timeline index.\n");
        goto unlock;
    }
    index = trace->vacant_indices[--trace->vacant_indices_count];
unlock:
    if (submit_count)
        *submit_count = ++trace->submit_count;
    pthread_mutex_unlock(&trace->lock);
    return index;
}

void vkd3d_queue_timeline_trace_cleanup(struct vkd3d_queue_timeline_trace *trace)
{
    if (!trace->active)
        return;

    pthread_mutex_destroy(&trace->lock);
    if (trace->file)
        fclose(trace->file);

    vkd3d_free(trace->vacant_indices);
    vkd3d_free(trace->state);
}

struct vkd3d_queue_timeline_trace_cookie
vkd3d_queue_timeline_trace_register_event_signal(struct vkd3d_queue_timeline_trace *trace,
        vkd3d_native_sync_handle handle, d3d12_fence_iface *fence, uint64_t value)
{
    struct vkd3d_queue_timeline_trace_cookie cookie = {0};
    struct vkd3d_queue_timeline_trace_state *state;

    if (!trace->active)
        return cookie;

    cookie.index = vkd3d_queue_timeline_trace_allocate_index(trace, NULL);
    if (!cookie.index)
        return cookie;

    state = &trace->state[cookie.index];
    state->type = VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_EVENT;
    state->start_ts = vkd3d_get_current_time_ns();

#ifdef _WIN32
    snprintf(state->desc, sizeof(state->desc), "event: %p, fence: %p, value %"PRIu64,
            handle.handle, (void*)fence, value);
#else
    snprintf(state->desc, sizeof(state->desc), "event: %d, fence: %p, value %"PRIu64,
            handle.fd, (void*)fence, value);
#endif

    return cookie;
}

void vkd3d_queue_timeline_trace_complete_event_signal(struct vkd3d_queue_timeline_trace *trace,
        struct vkd3d_fence_worker *worker,
        struct vkd3d_queue_timeline_trace_cookie cookie)
{
    const struct vkd3d_queue_timeline_trace_state *state;
    double end_ts, start_ts;
    unsigned int pid;

    if (!trace->active || cookie.index == 0)
        return;

    state = &trace->state[cookie.index];
    end_ts = (double)(vkd3d_get_current_time_ns() - trace->base_ts) * 1e-3;
    start_ts = (double)(state->start_ts - trace->base_ts) * 1e-3;

    if (worker)
    {
        pid = worker->queue->submission_thread_tid;
        if (start_ts < worker->timeline.lock_end_event_ts)
            start_ts = worker->timeline.lock_end_event_ts;
        if (end_ts < start_ts)
            end_ts = start_ts;
        worker->timeline.lock_end_event_ts = end_ts;

        fprintf(trace->file, "{ \"name\": \"%s\", \"ph\": \"X\", \"tid\": \"event\", \"pid\": \"%u\", \"ts\": %f, \"dur\": %f },\n",
                state->desc, pid, start_ts, end_ts - start_ts);
    }

    vkd3d_queue_timeline_trace_free_index(trace, cookie.index);
}

void vkd3d_queue_timeline_trace_complete_present_wait(struct vkd3d_queue_timeline_trace *trace,
        struct vkd3d_queue_timeline_trace_cookie cookie)
{
    const struct vkd3d_queue_timeline_trace_state *state;
    double end_ts, start_ts;

    if (!trace->active || cookie.index == 0)
        return;

    state = &trace->state[cookie.index];
    end_ts = (double)(vkd3d_get_current_time_ns() - trace->base_ts) * 1e-3;
    start_ts = (double)(state->start_ts - trace->base_ts) * 1e-3;

    fprintf(trace->file, "{ \"name\": \"%s\", \"ph\": \"X\", \"tid\": \"wait\", \"pid\": \"present\", \"ts\": %f, \"dur\": %f },\n",
            state->desc, start_ts, end_ts - start_ts);

    vkd3d_queue_timeline_trace_free_index(trace, cookie.index);
}

struct vkd3d_queue_timeline_trace_cookie
vkd3d_queue_timeline_trace_register_execute(struct vkd3d_queue_timeline_trace *trace,
        ID3D12CommandList * const *command_lists, unsigned int count)
{
    struct vkd3d_queue_timeline_trace_cookie cookie = {0};
    struct vkd3d_queue_timeline_trace_state *state;
    uint64_t submission_count;
    if (!trace->active)
        return cookie;

    cookie.index = vkd3d_queue_timeline_trace_allocate_index(trace, &submission_count);
    if (!cookie.index)
        return cookie;

    state = &trace->state[cookie.index];
    state->type = VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_SUBMISSION;
    state->start_ts = vkd3d_get_current_time_ns();
    snprintf(state->desc, sizeof(state->desc), "SUBMIT #%"PRIu64" (%u lists)", submission_count, count);

    /* Might be useful later. */
    (void)command_lists;

    return cookie;
}

struct vkd3d_queue_timeline_trace_cookie
vkd3d_queue_timeline_trace_register_signal(struct vkd3d_queue_timeline_trace *trace,
        d3d12_fence_iface *fence, uint64_t value)
{
    struct vkd3d_queue_timeline_trace_cookie cookie = {0};
    struct vkd3d_queue_timeline_trace_state *state;
    if (!trace->active)
        return cookie;

    cookie.index = vkd3d_queue_timeline_trace_allocate_index(trace, NULL);
    if (!cookie.index)
        return cookie;

    state = &trace->state[cookie.index];
    state->type = VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_SIGNAL;
    state->start_ts = vkd3d_get_current_time_ns();
    state->start_submit_ts = state->start_ts;
    snprintf(state->desc, sizeof(state->desc), "SIGNAL %p %"PRIu64, (void*)fence, value);
    return cookie;
}

struct vkd3d_queue_timeline_trace_cookie
vkd3d_queue_timeline_trace_register_wait(struct vkd3d_queue_timeline_trace *trace,
        d3d12_fence_iface *fence, uint64_t value)
{
    struct vkd3d_queue_timeline_trace_cookie cookie = {0};
    struct vkd3d_queue_timeline_trace_state *state;
    if (!trace->active)
        return cookie;

    cookie.index = vkd3d_queue_timeline_trace_allocate_index(trace, NULL);
    if (!cookie.index)
        return cookie;

    state = &trace->state[cookie.index];
    state->type = VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_WAIT;
    state->start_ts = vkd3d_get_current_time_ns();
    state->start_submit_ts = state->start_ts;
    snprintf(state->desc, sizeof(state->desc), "WAIT %p %"PRIu64, (void*)fence, value);
    return cookie;
}

static struct vkd3d_queue_timeline_trace_cookie
vkd3d_queue_timeline_trace_register_generic_op(struct vkd3d_queue_timeline_trace *trace,
        enum vkd3d_queue_timeline_trace_state_type type, const char *tag)
{
    struct vkd3d_queue_timeline_trace_cookie cookie = {0};
    struct vkd3d_queue_timeline_trace_state *state;
    if (!trace->active)
        return cookie;

    cookie.index = vkd3d_queue_timeline_trace_allocate_index(trace, NULL);
    if (!cookie.index)
        return cookie;

    state = &trace->state[cookie.index];
    state->type = type;
    state->start_ts = vkd3d_get_current_time_ns();
    state->start_submit_ts = state->start_ts;
    vkd3d_strlcpy(state->desc, sizeof(state->desc), tag);
    return cookie;
}

struct vkd3d_queue_timeline_trace_cookie
vkd3d_queue_timeline_trace_register_swapchain_blit(struct vkd3d_queue_timeline_trace *trace, uint64_t present_id)
{
    char str[128];
    snprintf(str, sizeof(str), "PRESENT (id = %"PRIu64") (blit)", present_id);
    return vkd3d_queue_timeline_trace_register_generic_op(trace, VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_PRESENT, str);
}

struct vkd3d_queue_timeline_trace_cookie
vkd3d_queue_timeline_trace_register_callback(struct vkd3d_queue_timeline_trace *trace)
{
    return vkd3d_queue_timeline_trace_register_generic_op(trace, VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_CALLBACK, "CALLBACK");
}

struct vkd3d_queue_timeline_trace_cookie
vkd3d_queue_timeline_trace_register_present_wait(struct vkd3d_queue_timeline_trace *trace, uint64_t present_id)
{
    char str[128];
    snprintf(str, sizeof(str), "WAIT (id = %"PRIu64")", present_id);
    return vkd3d_queue_timeline_trace_register_generic_op(trace, VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_CALLBACK, str);
}

void vkd3d_queue_timeline_trace_complete_execute(struct vkd3d_queue_timeline_trace *trace,
        struct vkd3d_fence_worker *worker,
        struct vkd3d_queue_timeline_trace_cookie cookie)
{
    const struct vkd3d_queue_timeline_trace_state *state;
    double end_ts, start_submit_ts, start_ts;
    unsigned int pid;
    const char *tid;
    double *ts_lock;

    if (!trace->active || cookie.index == 0)
        return;

    state = &trace->state[cookie.index];
    start_ts = (double)(state->start_ts - trace->base_ts) * 1e-3;
    start_submit_ts = (double)(state->start_submit_ts - trace->base_ts) * 1e-3;
    end_ts = (double)(vkd3d_get_current_time_ns() - trace->base_ts) * 1e-3;

    if (worker)
    {
        tid = worker->timeline.tid;
        pid = worker->queue->submission_thread_tid;

        if (state->type == VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_SUBMISSION)
        {
            fprintf(trace->file, "{ \"name\": \"%s\", \"ph\": \"i\", \"tid\": \"cpu\", \"pid\": %u, \"ts\": %f, \"s\": \"t\" },\n",
                    state->desc, pid, start_ts);

            if (start_ts < worker->timeline.lock_end_cpu_ts)
                start_ts = worker->timeline.lock_end_cpu_ts;
            if (start_submit_ts < start_ts)
                start_submit_ts = start_ts;
        }

        if (state->type == VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_CALLBACK)
        {
            ts_lock = &worker->timeline.lock_end_callback_ts;
            tid = "callback";
        }
        else
            ts_lock = &worker->timeline.lock_end_gpu_ts;

        if (start_submit_ts < *ts_lock)
            start_submit_ts = *ts_lock;
        if (end_ts < start_submit_ts)
            end_ts = start_submit_ts;
        *ts_lock = end_ts;

        fprintf(trace->file, "{ \"name\": \"%s\", \"ph\": \"X\", \"tid\": \"%s\", \"pid\": %u, \"ts\": %f, \"dur\": %f },\n",
                state->desc, tid, pid, start_submit_ts, end_ts - start_submit_ts);

        if (state->type == VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_SUBMISSION)
        {
            worker->timeline.lock_end_cpu_ts = start_submit_ts;
            fprintf(trace->file,
                    "{ \"name\": \"%s\", \"ph\": \"X\", \"tid\": \"submit\", \"pid\": %u, \"ts\": %f, \"dur\": %f },\n",
                    state->desc, pid, start_ts, start_submit_ts - start_ts);
        }
    }

    vkd3d_queue_timeline_trace_free_index(trace, cookie.index);
}

void vkd3d_queue_timeline_trace_begin_execute(struct vkd3d_queue_timeline_trace *trace,
        struct vkd3d_queue_timeline_trace_cookie cookie)
{
    struct vkd3d_queue_timeline_trace_state *state;
    if (!trace->active || cookie.index == 0)
        return;

    state = &trace->state[cookie.index];
    state->start_submit_ts = vkd3d_get_current_time_ns();
}
