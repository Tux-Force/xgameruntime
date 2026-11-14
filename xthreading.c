/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XAsync, XTaskQueue and XThread
 *
 * Written by Weather
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

#include <wine/list.h>

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_threading
{
    IXThreadingImpl IXThreadingImpl_iface;
    LONG ref;
};

static inline struct x_threading *impl_from_IXThreadingImpl( IXThreadingImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_threading, IXThreadingImpl_iface );
}

static HRESULT WINAPI x_threading_QueryInterface( IXThreadingImpl *iface, REFIID iid, void **out )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown        ) ||
        IsEqualGUID( iid, &IID_IXThreadingImpl ))
    {
        IXThreadingImpl_AddRef( *out = &impl->IXThreadingImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_threading_AddRef( IXThreadingImpl *iface )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_threading_Release( IXThreadingImpl *iface )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/*
 * XTaskQueue implementation.
 *
 * A task queue owns two ports (work and completion). Every port keeps a list
 * of pending callbacks (optionally delayed) and is serviced according to its
 * dispatch mode: manually through XTaskQueueDispatch(), by a growing pool of
 * worker threads, by a single worker thread, or immediately in the submitter.
 */

#define MAX_POOL_THREADS 8

struct task_item
{
    struct list entry;
    ULONGLONG due;
    void *context;
    XTaskQueueCallback *callback;
};

struct XTaskQueuePortObject
{
    LONG ref;
    XTaskQueuePort type;
    XTaskQueueDispatchMode mode;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    struct list items;
    LONG pending;
    LONG threads;
    LONG idle;
    LONG max_threads;
    BOOL stopping;
};

struct monitor
{
    struct list entry;
    UINT64 token;
    void *context;
    XTaskQueueMonitorCallback *callback;
};

struct waiter
{
    struct list entry;
    UINT64 token;
    HANDLE wait;
    XTaskQueueHandle queue;
    XTaskQueuePort port;
    void *context;
    XTaskQueueCallback *callback;
};

struct XTaskQueueObject
{
    LONG ref;
    struct XTaskQueuePortObject *work;
    struct XTaskQueuePortObject *completion;
    CRITICAL_SECTION cs;
    struct list monitors;
    struct list waiters;
    BOOL terminated;
};

static LONG64 next_token = 0;
static SRWLOCK queue_lock = SRWLOCK_INIT;
static XTaskQueueHandle process_queue;
static XTaskQueueHandle default_queue;

static void port_addref( struct XTaskQueuePortObject *port )
{
    InterlockedIncrement( &port->ref );
}

static void port_release( struct XTaskQueuePortObject *port )
{
    struct task_item *item, *next;

    if (InterlockedDecrement( &port->ref )) return;

    LIST_FOR_EACH_ENTRY_SAFE( item, next, &port->items, struct task_item, entry )
    {
        list_remove( &item->entry );
        HeapFree( GetProcessHeap(), 0, item );
    }
    DeleteCriticalSection( &port->cs );
    HeapFree( GetProcessHeap(), 0, port );
}

static HRESULT port_create( XTaskQueuePort type, XTaskQueueDispatchMode mode, struct XTaskQueuePortObject **out )
{
    struct XTaskQueuePortObject *port;

    if (mode > XTaskQueueDispatchMode_Immediate) return E_INVALIDARG;
    if (!(port = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*port) ))) return E_OUTOFMEMORY;

    port->ref = 1;
    port->type = type;
    port->mode = mode;
    port->max_threads = (mode == XTaskQueueDispatchMode_SerializedThreadPool) ? 1 : MAX_POOL_THREADS;
    InitializeCriticalSection( &port->cs );
    InitializeConditionVariable( &port->cv );
    list_init( &port->items );

    *out = port;
    return S_OK;
}

/* Runs a single ready callback. Returns FALSE on timeout, or when a worker has to exit. */
static BOOL port_dispatch_one( struct XTaskQueuePortObject *port, DWORD timeout, BOOL worker )
{
    ULONGLONG start = GetTickCount64();

    EnterCriticalSection( &port->cs );
    for (;;)
    {
        ULONGLONG now = GetTickCount64();
        struct task_item *item, *best = NULL;
        DWORD wait = INFINITE;

        LIST_FOR_EACH_ENTRY( item, &port->items, struct task_item, entry )
            if (!best || item->due < best->due) best = item;

        if (best && best->due <= now)
        {
            XTaskQueueCallback *callback = best->callback;
            void *context = best->context;

            list_remove( &best->entry );
            port->pending--;
            HeapFree( GetProcessHeap(), 0, best );
            LeaveCriticalSection( &port->cs );

            callback( context, FALSE );
            return TRUE;
        }

        if (worker && port->stopping && !best) break;

        if (best) wait = (DWORD)min( best->due - now, (ULONGLONG)INFINITE - 1 );

        if (timeout != INFINITE)
        {
            ULONGLONG elapsed = now - start;
            if (elapsed >= timeout) break;
            wait = min( wait, (DWORD)(timeout - elapsed) );
        }

        if (worker) port->idle++;
        SleepConditionVariableCS( &port->cv, &port->cs, wait );
        if (worker) port->idle--;
    }
    LeaveCriticalSection( &port->cs );
    return FALSE;
}

static DWORD WINAPI port_worker( void *arg )
{
    struct XTaskQueuePortObject *port = arg;

    while (port_dispatch_one( port, INFINITE, TRUE )) {}

    EnterCriticalSection( &port->cs );
    port->threads--;
    LeaveCriticalSection( &port->cs );
    port_release( port );
    return 0;
}

static void port_stop( struct XTaskQueuePortObject *port )
{
    EnterCriticalSection( &port->cs );
    port->stopping = TRUE;
    WakeAllConditionVariable( &port->cv );
    LeaveCriticalSection( &port->cs );
}

/* Cancels all pending callbacks of a port, invoking them with canceled = TRUE. */
static void port_cancel_all( struct XTaskQueuePortObject *port )
{
    struct list cancelled = LIST_INIT( cancelled );
    struct task_item *item, *next;

    EnterCriticalSection( &port->cs );
    LIST_FOR_EACH_ENTRY_SAFE( item, next, &port->items, struct task_item, entry )
    {
        list_remove( &item->entry );
        list_add_tail( &cancelled, &item->entry );
    }
    port->pending = 0;
    WakeAllConditionVariable( &port->cv );
    LeaveCriticalSection( &port->cs );

    LIST_FOR_EACH_ENTRY_SAFE( item, next, &cancelled, struct task_item, entry )
    {
        list_remove( &item->entry );
        item->callback( item->context, TRUE );
        HeapFree( GetProcessHeap(), 0, item );
    }
}

static HRESULT port_submit( struct XTaskQueuePortObject *port, UINT32 delay, void *context, XTaskQueueCallback *callback )
{
    struct task_item *item;
    BOOL spawn = FALSE;

    if (!callback) return E_INVALIDARG;

    if (port->mode == XTaskQueueDispatchMode_Immediate && !delay)
    {
        callback( context, FALSE );
        return S_OK;
    }

    if (!(item = HeapAlloc( GetProcessHeap(), 0, sizeof(*item) ))) return E_OUTOFMEMORY;
    item->due = GetTickCount64() + delay;
    item->context = context;
    item->callback = callback;

    EnterCriticalSection( &port->cs );
    list_add_tail( &port->items, &item->entry );
    port->pending++;
    if (port->mode != XTaskQueueDispatchMode_Manual &&
        port->pending > port->idle && port->threads < port->max_threads)
    {
        port->threads++;
        spawn = TRUE;
    }
    WakeConditionVariable( &port->cv );
    LeaveCriticalSection( &port->cs );

    if (spawn)
    {
        HANDLE thread;
        port_addref( port );
        if ((thread = CreateThread( NULL, 0, port_worker, port, 0, NULL ))) CloseHandle( thread );
        else
        {
            EnterCriticalSection( &port->cs );
            port->threads--;
            LeaveCriticalSection( &port->cs );
            port_release( port );
        }
    }
    return S_OK;
}

static struct XTaskQueuePortObject *queue_get_port( XTaskQueueHandle queue, XTaskQueuePort port )
{
    if (port == XTaskQueuePort_Work) return queue->work;
    if (port == XTaskQueuePort_Completion) return queue->completion;
    return NULL;
}

static HRESULT queue_create( struct XTaskQueuePortObject *work, struct XTaskQueuePortObject *completion, XTaskQueueHandle *out )
{
    XTaskQueueHandle queue;

    if (!(queue = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*queue) ))) return E_OUTOFMEMORY;
    queue->ref = 1;
    queue->work = work;
    queue->completion = completion;
    InitializeCriticalSection( &queue->cs );
    list_init( &queue->monitors );
    list_init( &queue->waiters );

    *out = queue;
    return S_OK;
}

static void queue_addref( XTaskQueueHandle queue )
{
    InterlockedIncrement( &queue->ref );
}

static void queue_release( XTaskQueueHandle queue )
{
    struct monitor *monitor, *next_monitor;
    struct waiter *waiter, *next_waiter;

    if (InterlockedDecrement( &queue->ref )) return;

    LIST_FOR_EACH_ENTRY_SAFE( waiter, next_waiter, &queue->waiters, struct waiter, entry )
    {
        list_remove( &waiter->entry );
        UnregisterWaitEx( waiter->wait, NULL );
        /* The waiter may still be referenced by a running wait callback; leak it on purpose. */
    }
    LIST_FOR_EACH_ENTRY_SAFE( monitor, next_monitor, &queue->monitors, struct monitor, entry )
    {
        list_remove( &monitor->entry );
        HeapFree( GetProcessHeap(), 0, monitor );
    }

    port_stop( queue->work );
    if (queue->completion != queue->work) port_stop( queue->completion );
    port_release( queue->work );
    port_release( queue->completion );

    DeleteCriticalSection( &queue->cs );
    HeapFree( GetProcessHeap(), 0, queue );
}

static void queue_notify_monitors( XTaskQueueHandle queue, XTaskQueuePort port )
{
    struct monitor *monitor, *copy;
    UINT count = 0, i = 0;

    EnterCriticalSection( &queue->cs );
    LIST_FOR_EACH_ENTRY( monitor, &queue->monitors, struct monitor, entry ) count++;
    if (!count || !(copy = HeapAlloc( GetProcessHeap(), 0, count * sizeof(*copy) )))
    {
        LeaveCriticalSection( &queue->cs );
        return;
    }
    LIST_FOR_EACH_ENTRY( monitor, &queue->monitors, struct monitor, entry ) copy[i++] = *monitor;
    LeaveCriticalSection( &queue->cs );

    for (i = 0; i < count; i++) copy[i].callback( copy[i].context, queue, port );
    HeapFree( GetProcessHeap(), 0, copy );
}

static HRESULT queue_submit( XTaskQueueHandle queue, XTaskQueuePort port_type, UINT32 delay, void *context, XTaskQueueCallback *callback )
{
    struct XTaskQueuePortObject *port;
    HRESULT hr;

    if (!queue || !(port = queue_get_port( queue, port_type ))) return E_INVALIDARG;
    if (queue->terminated) return E_ABORT;

    if (SUCCEEDED(hr = port_submit( port, delay, context, callback ))) queue_notify_monitors( queue, port_type );
    return hr;
}

/* Resolves the queue to use for an async operation: explicit, process-wide, or the default one. */
static XTaskQueueHandle queue_resolve( XTaskQueueHandle queue )
{
    struct XTaskQueuePortObject *work, *completion;

    if (queue) return queue;

    AcquireSRWLockExclusive( &queue_lock );
    if (process_queue) queue = process_queue;
    else
    {
        if (!default_queue)
        {
            if (SUCCEEDED(port_create( XTaskQueuePort_Work, XTaskQueueDispatchMode_ThreadPool, &work )))
            {
                if (SUCCEEDED(port_create( XTaskQueuePort_Completion, XTaskQueueDispatchMode_ThreadPool, &completion )))
                {
                    if (FAILED(queue_create( work, completion, &default_queue )))
                    {
                        port_release( work );
                        port_release( completion );
                    }
                }
                else port_release( work );
            }
        }
        queue = default_queue;
    }
    ReleaseSRWLockExclusive( &queue_lock );
    return queue;
}

static VOID CALLBACK waiter_callback( void *arg, BOOLEAN timed_out )
{
    struct waiter *waiter = arg;
    queue_submit( waiter->queue, waiter->port, 0, waiter->context, waiter->callback );
}

struct terminate_data
{
    void *context;
    XTaskQueueTerminatedCallback *callback;
};

static DWORD WINAPI terminate_thread( void *arg )
{
    struct terminate_data *data = arg;
    data->callback( data->context );
    HeapFree( GetProcessHeap(), 0, data );
    return 0;
}

/*
 * XAsync implementation, built on top of the task queues above.
 * XAsyncBlock::internal[0] holds the operation state, internal[1] a magic value.
 */

#define ASYNC_MAGIC ((void *)(UINT_PTR)0x58415359)

struct async_state
{
    LONG ref;
    XAsyncProvider *provider;
    XAsyncProviderData data;
    const void *identity;
    HANDLE event;
    LONG completed;
    HRESULT status;
    SIZE_T required;
};

static SRWLOCK async_lock = SRWLOCK_INIT;

/* Looks up the state of an async block and takes a reference that the caller must release. */
static struct async_state *async_acquire( XAsyncBlock *block )
{
    struct async_state *state = NULL;

    if (!block) return NULL;
    AcquireSRWLockExclusive( &async_lock );
    if (block->internal[1] == ASYNC_MAGIC && (state = block->internal[0])) InterlockedIncrement( &state->ref );
    ReleaseSRWLockExclusive( &async_lock );
    return state;
}

static void async_release( struct async_state *state )
{
    if (InterlockedDecrement( &state->ref )) return;
    CloseHandle( state->event );
    HeapFree( GetProcessHeap(), 0, state );
}

static void WINAPI async_completion_callback( void *context, BOOLEAN canceled )
{
    struct async_state *state = context;
    XAsyncBlock *block = state->data.async;

    if (block->callback) block->callback( block );
    async_release( state );
}

static void async_complete( struct async_state *state, HRESULT result, SIZE_T required )
{
    XAsyncBlock *block = state->data.async;

    /* The game may fetch the result and free the operation as soon as it is marked completed. */
    InterlockedIncrement( &state->ref );
    if (InterlockedCompareExchange( &state->completed, 1, 0 ))
    {
        async_release( state );
        return;
    }

    state->required = required;
    state->status = result;
    SetEvent( state->event );

    if (block->callback)
    {
        InterlockedIncrement( &state->ref );
        if (FAILED(queue_submit( queue_resolve( block->queue ), XTaskQueuePort_Completion, 0, state, async_completion_callback )))
            async_release( state );
    }
    async_release( state );
}

static void WINAPI async_work_callback( void *context, BOOLEAN canceled )
{
    struct async_state *state = context;

    if (canceled) async_complete( state, E_ABORT, 0 );
    else
    {
        HRESULT hr = state->provider( XAsyncOp_DoWork, &state->data );
        if (hr != E_PENDING) async_complete( state, hr, 0 );
    }
    async_release( state );
}

static HRESULT async_schedule( XAsyncBlock *block, UINT32 delay )
{
    struct async_state *state = async_acquire( block );
    HRESULT hr;

    if (!state) return E_INVALIDARG;

    /* The reference taken by async_acquire is handed to the work callback. */
    if (FAILED(hr = queue_submit( queue_resolve( block->queue ), XTaskQueuePort_Work, delay, state, async_work_callback )))
        async_release( state );
    return hr;
}

static HRESULT async_begin( XAsyncBlock *block, void *context, const void *identity, XAsyncProvider *provider )
{
    struct async_state *state;
    HRESULT hr;

    if (!block || !provider) return E_INVALIDARG;
    if (!(state = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state) ))) return E_OUTOFMEMORY;
    if (!(state->event = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        HeapFree( GetProcessHeap(), 0, state );
        return E_OUTOFMEMORY;
    }

    state->ref = 1;
    state->provider = provider;
    state->identity = identity;
    state->status = E_PENDING;
    state->data.async = block;
    state->data.context = context;

    AcquireSRWLockExclusive( &async_lock );
    block->internal[0] = state;
    block->internal[1] = ASYNC_MAGIC;
    block->internal[2] = NULL;
    block->internal[3] = NULL;
    ReleaseSRWLockExclusive( &async_lock );

    if (FAILED(hr = provider( XAsyncOp_Begin, &state->data )))
    {
        AcquireSRWLockExclusive( &async_lock );
        block->internal[0] = NULL;
        block->internal[1] = NULL;
        ReleaseSRWLockExclusive( &async_lock );
        async_release( state );
        return hr;
    }
    return S_OK;
}

static HRESULT WINAPI async_run_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    switch (op)
    {
        case XAsyncOp_Begin:
            return async_schedule( data->async, 0 );
        case XAsyncOp_DoWork:
            return ((XAsyncWork *)data->context)( data->async );
        default:
            return S_OK;
    }
}

void *gdkc_async_context( XAsyncBlock *block )
{
    struct async_state *state = async_acquire( block );
    void *context = state ? state->data.context : NULL;

    if (state) async_release( state );
    return context;
}

static HRESULT WINAPI x_threading_XAsyncGetStatus( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, BOOLEAN wait )
{
    struct async_state *state = async_acquire( asyncBlock );
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, wait %d.\n", iface, asyncBlock, wait );

    if (!state) return E_INVALIDARG;
    if (wait) WaitForSingleObject( state->event, INFINITE );
    hr = state->completed ? state->status : E_PENDING;
    async_release( state );
    return hr;
}

static HRESULT WINAPI x_threading_XAsyncGetResultSize( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, SIZE_T *bufferSize )
{
    struct async_state *state = async_acquire( asyncBlock );
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, bufferSize %p.\n", iface, asyncBlock, bufferSize );

    if (!state) return E_INVALIDARG;
    if (!bufferSize) hr = E_INVALIDARG;
    else if (!state->completed) hr = E_ILLEGAL_METHOD_CALL;
    else if (FAILED(state->status)) hr = state->status;
    else
    {
        *bufferSize = state->required;
        hr = S_OK;
    }
    async_release( state );
    return hr;
}

static void WINAPI x_threading_XAsyncCancel( IXThreadingImpl *iface, XAsyncBlock *asyncBlock )
{
    struct async_state *state = async_acquire( asyncBlock );

    TRACE( "iface %p, asyncBlock %p.\n", iface, asyncBlock );

    if (!state) return;
    if (!state->completed)
    {
        state->provider( XAsyncOp_Cancel, &state->data );
        async_complete( state, E_ABORT, 0 );
    }
    async_release( state );
}

static HRESULT WINAPI x_threading_XAsyncRun( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, XAsyncWork *work )
{
    TRACE( "iface %p, asyncBlock %p, work %p.\n", iface, asyncBlock, work );
    return async_begin( asyncBlock, work, work, async_run_provider );
}

static HRESULT WINAPI x_threading_XAsyncBegin( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, void *context, const void *identity, const char *identityName, XAsyncProvider *provider )
{
    TRACE( "iface %p, asyncBlock %p, context %p, identity %p, identityName %s, provider %p.\n", iface, asyncBlock, context, identity, debugstr_a( identityName ), provider );
    return async_begin( asyncBlock, context, identity, provider );
}

static HRESULT WINAPI __PADDING__( IXThreadingImpl *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_threading_XAsyncSchedule( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, UINT32 delayInMs )
{
    TRACE( "iface %p, asyncBlock %p, delayInMs %d.\n", iface, asyncBlock, delayInMs );
    return async_schedule( asyncBlock, delayInMs );
}

static void WINAPI x_threading_XAsyncComplete( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, HRESULT result, SIZE_T requiredBufferSize )
{
    struct async_state *state = async_acquire( asyncBlock );

    TRACE( "iface %p, asyncBlock %p, result %#lx, requiredBufferSize %Iu.\n", iface, asyncBlock, result, requiredBufferSize );

    if (!state) return;
    async_complete( state, result, requiredBufferSize );
    async_release( state );
}

static HRESULT WINAPI x_threading_XAsyncGetResult( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, const void *identity, SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed )
{
    struct async_state *state = async_acquire( asyncBlock );
    HRESULT hr;

    TRACE( "iface %p asyncBlock %p, identity %p, bufferSize %Iu, buffer %p, bufferUsed %p.\n", iface, asyncBlock, identity, bufferSize, buffer, bufferUsed );

    if (!state) return E_INVALIDARG;
    if (identity && state->identity && identity != state->identity)
    {
        async_release( state );
        return E_INVALIDARG;
    }
    if (!state->completed)
    {
        async_release( state );
        return E_PENDING;
    }

    hr = state->status;
    if (SUCCEEDED(hr))
    {
        if (state->required > bufferSize)
        {
            async_release( state );
            return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
        }
        if (state->required)
        {
            state->data.buffer = buffer;
            state->data.bufferSize = bufferSize;
            hr = state->provider( XAsyncOp_GetResult, &state->data );
        }
        if (bufferUsed) *bufferUsed = state->required;
    }

    /* Only the first caller releases the operation. */
    AcquireSRWLockExclusive( &async_lock );
    if (asyncBlock->internal[0] == state)
    {
        asyncBlock->internal[0] = NULL;
        asyncBlock->internal[1] = NULL;
        ReleaseSRWLockExclusive( &async_lock );
        state->provider( XAsyncOp_Cleanup, &state->data );
        async_release( state );
    }
    else ReleaseSRWLockExclusive( &async_lock );

    async_release( state );
    return hr;
}

static HRESULT WINAPI x_threading_XTaskQueueCreate( IXThreadingImpl *iface, XTaskQueueDispatchMode workDispatchMode, XTaskQueueDispatchMode completionDispatchMode, XTaskQueueHandle *queue )
{
    struct XTaskQueuePortObject *work, *completion;
    HRESULT hr;

    TRACE( "iface %p, workDispatchMode %d, completionDispatchMode %d, queue %p.\n", iface, workDispatchMode, completionDispatchMode, queue );

    if (!queue) return E_INVALIDARG;
    if (FAILED(hr = port_create( XTaskQueuePort_Work, workDispatchMode, &work ))) return hr;
    if (FAILED(hr = port_create( XTaskQueuePort_Completion, completionDispatchMode, &completion )))
    {
        port_release( work );
        return hr;
    }
    if (FAILED(hr = queue_create( work, completion, queue )))
    {
        port_release( work );
        port_release( completion );
    }
    return hr;
}

static HRESULT WINAPI x_threading_XTaskQueueCreateComposite( IXThreadingImpl *iface, XTaskQueuePortHandle workPort, XTaskQueuePortHandle completionPort, XTaskQueueHandle *queue )
{
    HRESULT hr;

    TRACE( "iface %p, workPort %p, completionPort %p, queue %p.\n", iface, workPort, completionPort, queue );

    if (!workPort || !completionPort || !queue) return E_INVALIDARG;
    port_addref( workPort );
    port_addref( completionPort );
    if (FAILED(hr = queue_create( workPort, completionPort, queue )))
    {
        port_release( workPort );
        port_release( completionPort );
    }
    return hr;
}

static HRESULT WINAPI x_threading_XTaskQueueGetPort( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, XTaskQueuePortHandle *portHandle )
{
    TRACE( "iface %p, queue %p, port %d, portHandle %p.\n", iface, queue, port, portHandle );

    if (!queue || !portHandle || !(*portHandle = queue_get_port( queue, port ))) return E_INVALIDARG;
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueDuplicateHandle( IXThreadingImpl *iface, XTaskQueueHandle queueHandle, XTaskQueueHandle *duplicatedHandle )
{
    TRACE( "iface %p, queueHandle %p, duplicatedHandle %p.\n", iface, queueHandle, duplicatedHandle );

    if (!queueHandle || !duplicatedHandle) return E_INVALIDARG;
    queue_addref( queueHandle );
    *duplicatedHandle = queueHandle;
    return S_OK;
}

static BOOLEAN WINAPI x_threading_XTaskQueueDispatch( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, UINT32 timeoutInMs )
{
    struct XTaskQueuePortObject *object;

    TRACE( "iface %p, queue %p, port %d, timeoutInMs %d.\n", iface, queue, port, timeoutInMs );

    if (!queue || !(object = queue_get_port( queue, port ))) return FALSE;
    if (object->mode != XTaskQueueDispatchMode_Manual) return FALSE;
    return port_dispatch_one( object, timeoutInMs, FALSE );
}

static void WINAPI x_threading_XTaskQueueCloseHandle( IXThreadingImpl *iface, XTaskQueueHandle queue )
{
    TRACE( "iface %p, queue %p.\n", iface, queue );
    if (queue) queue_release( queue );
}

static HRESULT WINAPI x_threading_XTaskQueueSubmitCallback( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, void *callbackContext, XTaskQueueCallback *callback )
{
    TRACE( "iface %p, queue %p, port %d, callbackContext %p, callback %p.\n", iface, queue, port, callbackContext, callback );
    return queue_submit( queue_resolve( queue ), port, 0, callbackContext, callback );
}

static HRESULT WINAPI x_threading_XTaskQueueSubmitDelayedCallback( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, UINT32 delayMs, void *callbackContext, XTaskQueueCallback *callback )
{
    TRACE( "iface %p, queue %p, port %d, delayMs %d, callbackContext %p, callback %p.\n", iface, queue, port, delayMs, callbackContext, callback );
    return queue_submit( queue_resolve( queue ), port, delayMs, callbackContext, callback );
}

static HRESULT WINAPI x_threading_XTaskQueueRegisterWaiter( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, HANDLE waitHandle, void *callbackContext, XTaskQueueCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct waiter *waiter;

    TRACE( "iface %p, queue %p, port %d, waitHandle %p, callbackContext %p, callback %p, token %p.\n", iface, queue, port, waitHandle, callbackContext, callback, token );

    if (!(queue = queue_resolve( queue )) || !queue_get_port( queue, port ) || !waitHandle || !callback) return E_INVALIDARG;
    if (!(waiter = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*waiter) ))) return E_OUTOFMEMORY;

    waiter->token = InterlockedIncrement64( &next_token );
    waiter->queue = queue;
    waiter->port = port;
    waiter->context = callbackContext;
    waiter->callback = callback;

    EnterCriticalSection( &queue->cs );
    list_add_tail( &queue->waiters, &waiter->entry );
    LeaveCriticalSection( &queue->cs );

    if (!RegisterWaitForSingleObject( &waiter->wait, waitHandle, waiter_callback, waiter, INFINITE, WT_EXECUTEDEFAULT ))
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        EnterCriticalSection( &queue->cs );
        list_remove( &waiter->entry );
        LeaveCriticalSection( &queue->cs );
        HeapFree( GetProcessHeap(), 0, waiter );
        return hr;
    }

    if (token) token->token = waiter->token;
    return S_OK;
}

static void WINAPI x_threading_XTaskQueueUnregisterWaiter( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueueRegistrationToken token )
{
    struct waiter *waiter;

    TRACE( "iface %p, queue %p, token %I64u.\n", iface, queue, token.token );

    if (!(queue = queue_resolve( queue ))) return;

    EnterCriticalSection( &queue->cs );
    LIST_FOR_EACH_ENTRY( waiter, &queue->waiters, struct waiter, entry )
    {
        if (waiter->token != token.token) continue;
        list_remove( &waiter->entry );
        LeaveCriticalSection( &queue->cs );
        UnregisterWaitEx( waiter->wait, NULL );
        /* A wait callback may still be running with this waiter; it is leaked on purpose. */
        return;
    }
    LeaveCriticalSection( &queue->cs );
}

static HRESULT WINAPI x_threading_XTaskQueueTerminate( IXThreadingImpl *iface, XTaskQueueHandle queue, BOOLEAN wait, void *callbackContext, XTaskQueueTerminatedCallback *callback )
{
    TRACE( "iface %p, queue %p, wait %d, callbackContext %p, callback %p.\n", iface, queue, wait, callbackContext, callback );

    if (!queue) return E_INVALIDARG;

    queue->terminated = TRUE;
    port_cancel_all( queue->work );
    if (queue->completion != queue->work) port_cancel_all( queue->completion );

    if (!callback) return S_OK;

    if (wait) callback( callbackContext );
    else
    {
        struct terminate_data *data;
        HANDLE thread;

        if (!(data = HeapAlloc( GetProcessHeap(), 0, sizeof(*data) ))) return E_OUTOFMEMORY;
        data->context = callbackContext;
        data->callback = callback;
        if ((thread = CreateThread( NULL, 0, terminate_thread, data, 0, NULL ))) CloseHandle( thread );
        else
        {
            HeapFree( GetProcessHeap(), 0, data );
            callback( callbackContext );
        }
    }
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueRegisterMonitor( IXThreadingImpl *iface, XTaskQueueHandle queue, void *callbackContext, XTaskQueueMonitorCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct monitor *monitor;

    TRACE( "iface %p, queue %p, callbackContext %p, callback %p, token %p.\n", iface, queue, callbackContext, callback, token );

    if (!queue || !callback || !token) return E_INVALIDARG;
    if (!(monitor = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*monitor) ))) return E_OUTOFMEMORY;

    monitor->token = InterlockedIncrement64( &next_token );
    monitor->context = callbackContext;
    monitor->callback = callback;

    EnterCriticalSection( &queue->cs );
    list_add_tail( &queue->monitors, &monitor->entry );
    LeaveCriticalSection( &queue->cs );

    token->token = monitor->token;
    return S_OK;
}

static void WINAPI x_threading_XTaskQueueUnregisterMonitor( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueueRegistrationToken token )
{
    struct monitor *monitor;

    TRACE( "iface %p, queue %p, token %I64u.\n", iface, queue, token.token );

    if (!queue) return;

    EnterCriticalSection( &queue->cs );
    LIST_FOR_EACH_ENTRY( monitor, &queue->monitors, struct monitor, entry )
    {
        if (monitor->token != token.token) continue;
        list_remove( &monitor->entry );
        HeapFree( GetProcessHeap(), 0, monitor );
        break;
    }
    LeaveCriticalSection( &queue->cs );
}

static BOOLEAN WINAPI x_threading_XTaskQueueGetCurrentProcessTaskQueue( IXThreadingImpl *iface, XTaskQueueHandle *queue )
{
    BOOLEAN found = FALSE;

    TRACE( "iface %p, queue %p.\n", iface, queue );

    if (!queue) return FALSE;

    AcquireSRWLockExclusive( &queue_lock );
    if ((*queue = process_queue))
    {
        queue_addref( process_queue );
        found = TRUE;
    }
    ReleaseSRWLockExclusive( &queue_lock );
    return found;
}

static void WINAPI x_threading_XTaskQueueSetCurrentProcessTaskQueue( IXThreadingImpl *iface, XTaskQueueHandle queue )
{
    XTaskQueueHandle old;

    TRACE( "iface %p, queue %p.\n", iface, queue );

    if (queue) queue_addref( queue );
    AcquireSRWLockExclusive( &queue_lock );
    old = process_queue;
    process_queue = queue;
    ReleaseSRWLockExclusive( &queue_lock );
    if (old) queue_release( old );
}

static HRESULT WINAPI x_threading_XThreadSetTimeSensitive( IXThreadingImpl *iface, BOOLEAN isTimeSensitiveThread )
{
    TRACE( "iface %p, isTimeSensitiveThread %d.\n", iface, isTimeSensitiveThread );
    if (!TlsSetValue( tlsIndex, (void *)(UINT_PTR)isTimeSensitiveThread )) return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

static void WINAPI x_threading_XThreadAssertNotTimeSensitive( IXThreadingImpl *iface )
{
    TRACE( "iface %p.\n", iface );
    if (TlsGetValue( tlsIndex )) DebugBreak();
}

static BOOLEAN WINAPI x_threading_XThreadIsTimeSensitive( IXThreadingImpl *iface )
{
    TRACE( "iface %p.\n", iface );
    return TlsGetValue( tlsIndex ) ? 1 : 0;
}

static const struct IXThreadingImplVtbl x_threading_vtbl =
{
    x_threading_QueryInterface,
    x_threading_AddRef,
    x_threading_Release,
    /* IXThreadingImpl methods */
    x_threading_XAsyncGetStatus,
    x_threading_XAsyncGetResultSize,
    x_threading_XAsyncCancel,
    x_threading_XAsyncRun,
    x_threading_XAsyncBegin,
    __PADDING__,
    x_threading_XAsyncSchedule,
    x_threading_XAsyncComplete,
    x_threading_XAsyncGetResult,
    x_threading_XTaskQueueCreate,
    x_threading_XTaskQueueCreateComposite,
    x_threading_XTaskQueueGetPort,
    x_threading_XTaskQueueDuplicateHandle,
    x_threading_XTaskQueueDispatch,
    x_threading_XTaskQueueCloseHandle,
    x_threading_XTaskQueueSubmitCallback,
    x_threading_XTaskQueueSubmitDelayedCallback,
    x_threading_XTaskQueueRegisterWaiter,
    x_threading_XTaskQueueUnregisterWaiter,
    x_threading_XTaskQueueTerminate,
    x_threading_XTaskQueueRegisterMonitor,
    x_threading_XTaskQueueUnregisterMonitor,
    x_threading_XTaskQueueGetCurrentProcessTaskQueue,
    x_threading_XTaskQueueSetCurrentProcessTaskQueue,
    x_threading_XThreadSetTimeSensitive,
    __PADDING__,
    x_threading_XThreadAssertNotTimeSensitive,
    x_threading_XThreadIsTimeSensitive
};

static struct x_threading x_threading =
{
    {&x_threading_vtbl},
    0,
};

IXThreadingImpl *x_threading_impl = &x_threading.IXThreadingImpl_iface;
