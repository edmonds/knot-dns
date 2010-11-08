#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "dthreads.h"
#include "common.h"
#include "log.h"

/* Lock thread state for R/W. */
static inline void lock_thread_rw(dthread_t *thread)
{
    pthread_mutex_lock(&thread->_mx);
}

/* Unlock thread state for R/W. */
static inline void unlock_thread_rw(dthread_t *thread)
{
    pthread_mutex_unlock(&thread->_mx);
}

/* Lock unit state for R/W. */
static inline void lock_unit_rw(dt_unit_t *unit)
{
    pthread_mutex_lock(&unit->_mx);
}

/* Unlock unit state for R/W. */
static inline void unlock_unit_rw(dt_unit_t *unit)
{
    pthread_mutex_unlock(&unit->_mx);
}


/* Signalize thread state change. */
static inline void unit_signalize_change(dt_unit_t *unit)
{
    pthread_mutex_lock(&unit->_report_mx);
    pthread_cond_signal(&unit->_report);
    pthread_mutex_unlock(&unit->_report_mx);
}

/*
 * Thread entrypoint interrupt handler.
 */
static void thread_ep_intr(int s)
{
}

/*
 * Thread entrypoint function.
 * This is an Idle state of each thread.
 * Depending on thread state, runnable is run or
 * thread blocks until it is requested.
 */
static void *thread_ep(void *data)
{
    // Check data
    dthread_t *thread = (dthread_t *)data;
    if (thread == 0) {
        return 0;
    }

    // Check if is a member of unit
    dt_unit_t* unit = thread->unit;
    if (unit == 0) {
        return 0;
    }

    // Register service and signal handler
    struct sigaction sa;
    sa.sa_handler = thread_ep_intr;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, 0);

    // Run loop
    for (;;) {

        // Check thread state
        lock_thread_rw(thread);
        if (thread->state & ThreadDead) {
            unlock_thread_rw(thread);
            break;
        }
        unlock_thread_rw(thread);

        // Update data
        lock_thread_rw(thread);
        thread->data = thread->_adata;
        runnable_t _run = thread->run;

        // Start runnable if thread is marked Active
        if ((thread->state == ThreadActive) && (thread->run != 0)) {
            unlock_thread_rw(thread);
            _run(thread);
        } else {
            unlock_thread_rw(thread);
        }

        // If the runnable was cancelled, start new iteration
        lock_thread_rw(thread);
        if (thread->state & ThreadCancelled) {
            thread->state &= ~ThreadCancelled;
            unlock_thread_rw(thread);
            continue;
        }
        unlock_thread_rw(thread);

        // Runnable finished without interruption, mark as Idle
        lock_thread_rw(thread);
        if(thread->state & ThreadActive) {
            thread->state &= ~ThreadActive;
            thread->state |= ThreadIdle;
        }
        unlock_thread_rw(thread);

        // Report thread state change
        unit_signalize_change(unit);

        // Go to sleep if idle
        lock_thread_rw(thread);
        if (thread->state & ThreadIdle) {
            unlock_thread_rw(thread);

            // Wait for notification from unit
            pthread_mutex_lock(&unit->_notify_mx);
            pthread_cond_wait(&unit->_notify, &unit->_notify_mx);
            pthread_mutex_unlock(&unit->_notify_mx);
        } else {
            unlock_thread_rw(thread);
        }
    }

    // Report thread state change
    unit_signalize_change(unit);

    // Return
    return 0;
}

/*!
 * \brief Create single thread.
 * \return New thread instance or 0.
 * \private
 */
dthread_t *dt_create_thread(dt_unit_t *unit)
{
    // Alloc thread
    dthread_t *thread = malloc(sizeof(dthread_t));
    if (thread == 0) {
        return 0;
    }

    memset(thread, 0, sizeof(dthread_t));

    // Blank thread state
    thread->state = ThreadJoined;
    pthread_mutex_init(&thread->_mx, 0);

    // Set membership in unit
    thread->unit = unit;

    // Initialize attribute
    pthread_attr_t *attr = &thread->_attr;
    pthread_attr_init(attr);
    pthread_attr_setinheritsched(attr, PTHREAD_INHERIT_SCHED);
    pthread_attr_setschedpolicy(attr, SCHED_OTHER);
    return thread;
}

/*!
 * \brief Delete single thread.
 * \private
 */
void dt_delete_thread(dthread_t **thread)
{
    // Check
    if (thread == 0)
        return;
    if (*thread == 0)
        return;

    // Delete attribute
    pthread_attr_destroy(&(*thread)->_attr);

    // Delete mutex
    pthread_mutex_destroy(&(*thread)->_mx);

    // Free memory
    free(*thread);
    *thread = 0;
}

dt_unit_t *dt_create (int count)
{
    dt_unit_t *unit = malloc(sizeof(dt_unit_t));
    if (unit == 0)
        return 0;

    // Initialize conditions
    if (pthread_cond_init(&unit->_notify, 0) != 0) {
        free(unit);
        return 0;
    }
    if (pthread_cond_init(&unit->_report, 0) != 0) {
        pthread_cond_destroy(&unit->_notify);
        free(unit);
        return 0;
    }

    // Initialize mutexes
    if (pthread_mutex_init(&unit->_notify_mx, 0) != 0) {
        pthread_cond_destroy(&unit->_notify);
        pthread_cond_destroy(&unit->_report);
        free(unit);
        return 0;
    }
    if (pthread_mutex_init(&unit->_report_mx, 0) != 0) {
        pthread_cond_destroy(&unit->_notify);
        pthread_cond_destroy(&unit->_report);
        pthread_mutex_destroy(&unit->_notify_mx);
        free(unit);
        return 0;
    }
    if (pthread_mutex_init(&unit->_mx, 0) != 0) {
        pthread_cond_destroy(&unit->_notify);
        pthread_cond_destroy(&unit->_report);
        pthread_mutex_destroy(&unit->_notify_mx);
        pthread_mutex_destroy(&unit->_report_mx);
        free(unit);
        return 0;
    }

    // Save unit size
    unit->size = count;

    // Alloc threads
    unit->threads = malloc(count * sizeof(dthread_t*));
    if (unit->threads == 0) {
        pthread_cond_destroy(&unit->_notify);
        pthread_cond_destroy(&unit->_report);
        pthread_mutex_destroy(&unit->_notify_mx);
        pthread_mutex_destroy(&unit->_report_mx);
        pthread_mutex_destroy(&unit->_mx);
        free(unit);
        return 0;
    }

    // Initialize threads
    int init_success = 1;
    for (int i = 0; i < count; ++i) {
        unit->threads[i] = dt_create_thread(unit);
        if (unit->threads[i] == 0) {
            init_success = 0;
            break;
        }
    }

    // Check thread initialization
    if (!init_success) {

        // Delete created threads
        for (int i = 0; i < count; ++i) {
            dt_delete_thread(&unit->threads[i]);
        }

        // Free rest of the unit
        pthread_cond_destroy(&unit->_notify);
        pthread_cond_destroy(&unit->_report);
        pthread_mutex_destroy(&unit->_notify_mx);
        pthread_mutex_destroy(&unit->_report_mx);
        pthread_mutex_destroy(&unit->_mx);
        free(unit->threads);
        free(unit);
        return 0;
    }

    return unit;
}

dt_unit_t *dt_create_coherent (int count, runnable_t runnable, void *data)
{
    // Create unit
    dt_unit_t *unit = dt_create(count);
    if (unit == 0)
        return 0;

    // Set threads common purpose
    for (int i = 0; i < count; ++i) {
        dthread_t *thread = unit->threads[i];
        lock_thread_rw(thread);
        thread->run = runnable;
        thread->_adata = data;
        unlock_thread_rw(thread);
    }

    return unit;
}

void dt_delete (dt_unit_t **unit)
{
    /*
     *  All threads must be stopped or idle at this point,
     *  or else the behavior is undefined.
     *  Sorry.
     */

    // Check
    if (unit == 0)
        return;
    if (*unit == 0)
        return;

    // Compact and reclaim idle threads
    dt_unit_t *d_unit = *unit;
    dt_compact(d_unit);

    // Delete threads
    for (int i = 0; i < d_unit->size; ++i) {
        dt_delete_thread(&d_unit->threads[i]);
    }

    // Deinit mutexes
    pthread_mutex_destroy(&d_unit->_notify_mx);
    pthread_mutex_destroy(&d_unit->_report_mx);

    // Deinit conditions
    pthread_cond_destroy(&d_unit->_notify);
    pthread_cond_destroy(&d_unit->_report);

    // Free memory
    free(d_unit->threads);
    free(d_unit);
    *unit = 0;
}

int dt_resize(dt_unit_t *unit, int size)
{
    // Evaluate delta
    int delta = unit->size - size;

    // Same size
    if (delta == 0)
        return 0;

    // Unit expansion
    if (delta < 0) {

        // Realloc threads
        dthread_t **threads = realloc(unit->threads, size * sizeof(dthread_t*));
        if (threads == 0)
            return -1;

        // Lock unit
        lock_unit_rw(unit);

        // Reassign
        unit->threads = threads;

        // Create new threads
        for (int i = unit->size; i < size; ++i) {
            threads[i] = dt_create_thread(unit);
        }

        // Update unit
        unit->size = size;
        unlock_unit_rw(unit);
        return 0;
    }


    // Unit shrinking
    int remaining = size;

    // New threads vector
    dthread_t **threads = malloc(size * sizeof(dthread_t*));
    if (threads == 0)
        return -1;

    // Lock unit
    lock_unit_rw(unit);

    // Iterate while there is space in new unit
    memset(threads, 0, size * sizeof(dthread_t*));
    int threshold = ThreadActive;
    for(;;) {

        // Find threads matching given criterias
        int inspected = 0;
        for (int i = 0; i < unit->size; ++i) {

            // Get thread
            dthread_t *thread = unit->threads[i];
            if (thread == 0)
                continue;

            // Count thread as inspected
            ++inspected;

            lock_thread_rw(thread);

            // Is there still space?
            if (remaining > 0) {

                // Populate with matching threads
                if (!threshold || (thread->state & threshold)) {

                    // Append to new vector
                    threads[size - remaining] = thread;
                    --remaining;

                    // Invalidate in old vector
                    unit->threads[i] = 0;
                }

                // Unlock current thread
                unlock_thread_rw(thread);

            } else {

                // Signalize thread to stop
                thread->state = ThreadDead | ThreadCancelled;
                dt_signalize(thread, SIGALRM);

                // Unlock current thread
                unlock_thread_rw(thread);

                // Notify idle threads
                pthread_mutex_lock(&unit->_notify_mx);
                pthread_cond_broadcast(&unit->_notify);
                pthread_mutex_unlock(&unit->_notify_mx);

                // Join thread
                pthread_join(thread->_thr, 0);
                thread->state = ThreadJoined;

                // Delete thread
                dt_delete_thread(&thread);
                unit->threads[i] = 0;
            }
        }

        // Finished inspecting running threads
        if (inspected == 0) {
            break;
        }

        // Lower threshold
        switch (threshold) {
        case ThreadActive:
            threshold = ThreadIdle;
            break;
        case ThreadIdle:
            threshold = ThreadDead;
            break;
        default:
            threshold = ThreadJoined;
            break;
        }
    }

    // Reassign unit threads vector
    unit->size = size;
    free(unit->threads);
    unit->threads = threads;

    // Unlock unit
    unlock_unit_rw(unit);

    return 0;
}

int dt_start (dt_unit_t *unit)
{
    // Lock unit
    lock_unit_rw(unit);
    for (int i = 0; i < unit->size; ++i)
    {
        dthread_t* thread = unit->threads[i];
        lock_thread_rw(thread);

        // Update state
        int prev_state = thread->state;
        thread->state |= ThreadActive;
        thread->state &= ~ThreadIdle;
        thread->state &= ~ThreadDead;
        thread->state &= ~ThreadJoined;

        // Do not re-create running threads
        if (prev_state != ThreadJoined) {
            unlock_thread_rw(thread);
            continue;
        }

        // Start thread
        int res = pthread_create(&thread->_thr,  /* pthread_t */
                                 &thread->_attr, /* pthread_attr_t */
                                 thread_ep,      /* routine: thread_ep */
                                 thread);        /* passed object: dthread_t */

        // Unlock thread
        unlock_thread_rw(thread);
        if (res != 0) {
            log_error("%s: failed to create thread %d", __func__, i);
            unlock_unit_rw(unit);
            return res;
        }
    }

    // Unlock unit
    unlock_unit_rw(unit);

    return 0;
}

int dt_signalize (dthread_t *thread, int signum)
{
   return pthread_kill(thread->_thr, signum);
}

int dt_join (dt_unit_t *unit)
{
    for(;;) {

        // Lock threads state
        pthread_mutex_lock(&unit->_report_mx);

        // Lock unit
        lock_unit_rw(unit);

        // Browse threads
        int active_threads = 0;
        for (int i = 0; i < unit->size; ++i) {

            // Count active threads
            dthread_t *thread = unit->threads[i];
            lock_thread_rw(thread);
            if(thread->state & ThreadActive)
                ++active_threads;

            // Reclaim dead threads
            if(thread->state & ThreadDead) {
                pthread_join(thread->_thr, 0);
                thread->state = ThreadJoined;
            }
            unlock_thread_rw(thread);
        }

        // Unlock unit
        unlock_unit_rw(unit);

        // Check result
        if (active_threads == 0) {
            pthread_mutex_unlock(&unit->_report_mx);
            break;
        }

        // Wait for a thread to finish
        pthread_cond_wait(&unit->_report, &unit->_report_mx);
        pthread_mutex_unlock(&unit->_report_mx);
    }

    return 0;
}

int dt_stop_id (dthread_t *thread)
{
    // Signalize active thread to stop
    lock_thread_rw(thread);
    if(thread->state > ThreadDead) {
        thread->state = ThreadDead | ThreadCancelled;
        dt_signalize(thread, SIGALRM);
    }
    unlock_thread_rw(thread);

    // Broadcast notification
    dt_unit_t *unit = thread->unit;
    if(unit != 0) {
        pthread_mutex_lock(&unit->_notify_mx);
        pthread_cond_broadcast(&unit->_notify);
        pthread_mutex_unlock(&unit->_notify_mx);
    }

    return 0;
}

int dt_stop (dt_unit_t *unit)
{
    // Lock unit
    lock_unit_rw(unit);

    // Signalize all threads to stop
    for (int i = 0; i < unit->size; ++i) {

        // Lock thread
        dthread_t *thread = unit->threads[i];
        lock_thread_rw(thread);
        if(thread->state > ThreadDead) {
            thread->state = ThreadDead | ThreadCancelled;
            dt_signalize(thread, SIGALRM);
        }
        unlock_thread_rw(thread);
    }

    // Unlock unit
    unlock_unit_rw(unit);

    // Broadcast notification
    if(unit != 0) {
        pthread_mutex_lock(&unit->_notify_mx);
        pthread_cond_broadcast(&unit->_notify);
        pthread_mutex_unlock(&unit->_notify_mx);
    }

    return 0;
}

int dt_setprio (dthread_t* thread, int prio)
{
    // Clamp priority
    int policy = SCHED_FIFO;
    prio = MIN(MAX(sched_get_priority_min(policy), prio),
               sched_get_priority_max(policy));

    // Update scheduler policy
    int ret = pthread_attr_setschedpolicy(&thread->_attr, policy);
    if (ret < 0) {
        debug_server("%s(%p, %d) failed: %s",
                     __func__, thread, prio, strerror(errno));
    }

    // Update priority
    struct sched_param sp;
    sp.sched_priority = prio;
    ret = pthread_attr_setschedparam(&thread->_attr, &sp);
    if (ret < 0) {
        debug_server("%s(%p, %d) failed: %s",
                     __func__, thread, prio, strerror(errno));
    }

    return ret;
}

int dt_repurpose (dthread_t* thread, runnable_t runnable, void *data)
{
    // Check
    if (thread == 0)
        return -1;

    // Lock thread state changes
    lock_thread_rw(thread);

    // Repurpose it's object and runnable
    thread->run = runnable;
    thread->_adata = data;

    // Stop here if thread isn't a member of a unit
    dt_unit_t *unit = thread->unit;
    if (unit == 0) {
        thread->state = ThreadActive | ThreadCancelled;
        unlock_thread_rw(thread);
        return 0;
    }

    // Cancel current runnable if running
    if (thread->state > ThreadDead) {

        // Update state
        thread->state = ThreadActive | ThreadCancelled;
        unlock_thread_rw(thread);

        // Notify thread
        pthread_mutex_lock(&unit->_notify_mx);
        pthread_cond_broadcast(&unit->_notify);
        pthread_mutex_unlock(&unit->_notify_mx);
    } else {
        unlock_thread_rw(thread);
    }

    return 0;
}

int dt_cancel (dthread_t *thread)
{
    // Check
    if (thread == 0)
        return -1;

    // Cancel with lone thread
    dt_unit_t* unit = thread->unit;
    if (unit == 0)
        return 0;

    // Cancel current runnable if running
    lock_thread_rw(thread);
    if (thread->state > ThreadDead) {

        // Update state
        thread->state = ThreadIdle | ThreadCancelled;
        unlock_thread_rw(thread);

        // Notify thread
        pthread_mutex_lock(&unit->_notify_mx);
        dt_signalize(thread, SIGALRM);
        pthread_cond_broadcast(&unit->_notify);
        pthread_mutex_unlock(&unit->_notify_mx);
    } else {
        unlock_thread_rw(thread);
    }

    return 0;
}

int dt_compact (dt_unit_t *unit)
{
    // Lock unit
    lock_unit_rw(unit);

    // Reclaim all Idle threads
    for (int i = 0; i < unit->size; ++i) {

        // Locked state update
        dthread_t *thread = unit->threads[i];
        lock_thread_rw(thread);
        if(thread->state > ThreadDead && thread->state < ThreadActive) {
            thread->state = ThreadDead;
        }
        unlock_thread_rw(thread);
    }

    // Unlock unit
    unlock_unit_rw(unit);

    // Notify all threads
    pthread_mutex_lock(&unit->_notify_mx);
    pthread_cond_broadcast(&unit->_notify);
    pthread_mutex_unlock(&unit->_notify_mx);

    // Lock unit
    lock_unit_rw(unit);

    // Join all threads
    for (int i = 0; i < unit->size; ++i) {

        // Reclaim all dead threads
        dthread_t *thread = unit->threads[i];
        if(thread->state & ThreadDead) {
            pthread_join(thread->_thr, 0);
            thread->state = ThreadJoined;
        }
    }

    // Unlock unit
    unlock_unit_rw(unit);

    return 0;
}

int dt_optimal_size()
{
#ifdef _SC_NPROCESSORS_ONLN
   int ret = (int) sysconf(_SC_NPROCESSORS_ONLN);
   if(ret >= 1)
      return ret + 1;
#endif
   log_info("server: failed to estimate the number of online CPUs");
   return DEFAULT_THR_COUNT;
}

