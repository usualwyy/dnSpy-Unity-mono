/*
 * sgen-thread-pool.c: Threadpool for all concurrent GC work.
 *
 * Copyright (C) 2015 Xamarin Inc
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "config.h"
#ifdef HAVE_SGEN_GC

#include "mono/sgen/sgen-gc.h"
#include "mono/sgen/sgen-thread-pool.h"
#include "mono/sgen/sgen-pointer-queue.h"
#include "mono/utils/mono-os-mutex.h"
#ifndef SGEN_WITHOUT_MONO
#include "mono/utils/mono-threads.h"
#endif

static mono_mutex_t lock;
static mono_cond_t work_cond;
static mono_cond_t done_cond;

static MonoNativeThreadId thread;

/* Only accessed with the lock held. */
static SgenPointerQueue job_queue;

static SgenThreadPoolThreadInitFunc thread_init_func;
static SgenThreadPoolIdleJobFunc idle_job_func;
static SgenThreadPoolContinueIdleJobFunc continue_idle_job_func;

static volatile gboolean threadpool_shutdown;
static volatile gboolean thread_finished;

enum {
	STATE_WAITING,
	STATE_IN_PROGRESS,
	STATE_DONE
};

/* Assumes that the lock is held. */
static SgenThreadPoolJob*
get_job_and_set_in_progress (void)
{
	for (size_t i = 0; i < job_queue.next_slot; ++i) {
		SgenThreadPoolJob *job = (SgenThreadPoolJob *)job_queue.data [i];
		if (job->state == STATE_WAITING) {
			job->state = STATE_IN_PROGRESS;
			return job;
		}
	}
	return NULL;
}

/* Assumes that the lock is held. */
static ssize_t
find_job_in_queue (SgenThreadPoolJob *job)
{
	for (ssize_t i = 0; i < job_queue.next_slot; ++i) {
		if (job_queue.data [i] == job)
			return i;
	}
	return -1;
}

/* Assumes that the lock is held. */
static void
remove_job (SgenThreadPoolJob *job)
{
	ssize_t index;
	SGEN_ASSERT (0, job->state == STATE_DONE, "Why are we removing a job that's not done?");
	index = find_job_in_queue (job);
	SGEN_ASSERT (0, index >= 0, "Why is the job we're trying to remove not in the queue?");
	job_queue.data [index] = NULL;
	sgen_pointer_queue_remove_nulls (&job_queue);
	sgen_thread_pool_job_free (job);
}

static gboolean
continue_idle_job (void)
{
	if (!continue_idle_job_func)
		return FALSE;
	return continue_idle_job_func ();
}

static mono_native_thread_return_t
thread_func (void *thread_data)
{
	thread_init_func (thread_data);

	mono_os_mutex_lock (&lock);
	for (;;) {
		/*
		 * It's important that we check the continue idle flag with the lock held.
		 * Suppose we didn't check with the lock held, and the result is FALSE.  The
		 * main thread might then set continue idle and signal us before we can take
		 * the lock, and we'd lose the signal.
		 */
		gboolean do_idle = continue_idle_job ();
		SgenThreadPoolJob *job = get_job_and_set_in_progress ();

		if (!job && !do_idle && !threadpool_shutdown) {
			/*
			 * pthread_cond_wait() can return successfully despite the condition
			 * not being signalled, so we have to run this in a loop until we
			 * really have work to do.
			 */
			mono_os_cond_wait (&work_cond, &lock);
			continue;
		}

		mono_os_mutex_unlock (&lock);

		if (job) {
			job->func (thread_data, job);

			mono_os_mutex_lock (&lock);

			SGEN_ASSERT (0, job->state == STATE_IN_PROGRESS, "The job should still be in progress.");
			job->state = STATE_DONE;
			remove_job (job);
			/*
			 * Only the main GC thread will ever wait on the done condition, so we don't
			 * have to broadcast.
			 */
			mono_os_cond_signal (&done_cond);
		} else if (do_idle) {
			SGEN_ASSERT (0, idle_job_func, "Why do we have idle work when there's no idle job function?");
			do {
				idle_job_func (thread_data);
				do_idle = continue_idle_job ();
			} while (do_idle && !job_queue.next_slot);

			mono_os_mutex_lock (&lock);

			if (!do_idle)
				mono_os_cond_signal (&done_cond);
		} else {
			SGEN_ASSERT (0, threadpool_shutdown, "Why did we unlock if no jobs and not shutting down?");
			mono_os_mutex_lock (&lock);
			thread_finished = TRUE;
			mono_os_cond_signal (&done_cond);
			mono_os_mutex_unlock (&lock);
			return 0;
		}
	}

	return (mono_native_thread_return_t)0;
}

void
sgen_thread_pool_init (int num_threads, SgenThreadPoolThreadInitFunc init_func, SgenThreadPoolIdleJobFunc idle_func, SgenThreadPoolContinueIdleJobFunc continue_idle_func, void **thread_datas)
{
	SGEN_ASSERT (0, num_threads == 1, "We only support 1 thread pool thread for now.");

	mono_os_mutex_init (&lock);
	mono_os_cond_init (&work_cond);
	mono_os_cond_init (&done_cond);

	thread_init_func = init_func;
	idle_job_func = idle_func;
	continue_idle_job_func = continue_idle_func;

	mono_native_thread_create (&thread, thread_func, thread_datas ? thread_datas [0] : NULL);
}

void
sgen_thread_pool_shutdown (void)
{
	if (!thread)
		return;

	mono_os_mutex_lock (&lock);
	threadpool_shutdown = TRUE;
	mono_os_cond_signal (&work_cond);
	while (!thread_finished)
		mono_os_cond_wait (&done_cond, &lock);
	mono_os_mutex_unlock (&lock);

	mono_os_mutex_destroy (&lock);
	mono_os_cond_destroy (&work_cond);
	mono_os_cond_destroy (&done_cond);
}

SgenThreadPoolJob*
sgen_thread_pool_job_alloc (const char *name, SgenThreadPoolJobFunc func, size_t size)
{
	SgenThreadPoolJob *job = (SgenThreadPoolJob *)sgen_alloc_internal_dynamic (size, INTERNAL_MEM_THREAD_POOL_JOB, TRUE);
	job->name = name;
	job->size = size;
	job->state = STATE_WAITING;
	job->func = func;
	return job;
}

void
sgen_thread_pool_job_free (SgenThreadPoolJob *job)
{
	sgen_free_internal_dynamic (job, job->size, INTERNAL_MEM_THREAD_POOL_JOB);
}

void
sgen_thread_pool_job_enqueue (SgenThreadPoolJob *job)
{
	mono_os_mutex_lock (&lock);

	sgen_pointer_queue_add (&job_queue, job);
	/*
	 * FIXME: We could check whether there is a job in progress.  If there is, there's
	 * no need to signal the condition, at least as long as we have only one thread.
	 */
	mono_os_cond_signal (&work_cond);

	mono_os_mutex_unlock (&lock);
}

void
sgen_thread_pool_job_wait (SgenThreadPoolJob *job)
{
	SGEN_ASSERT (0, job, "Where's the job?");

	mono_os_mutex_lock (&lock);

	while (find_job_in_queue (job) >= 0)
		mono_os_cond_wait (&done_cond, &lock);

	mono_os_mutex_unlock (&lock);
}

void
sgen_thread_pool_idle_signal (void)
{
	SGEN_ASSERT (0, idle_job_func, "Why are we signaling idle without an idle function?");

	mono_os_mutex_lock (&lock);

	if (continue_idle_job_func ())
		mono_os_cond_signal (&work_cond);

	mono_os_mutex_unlock (&lock);
}

void
sgen_thread_pool_idle_wait (void)
{
	SGEN_ASSERT (0, idle_job_func, "Why are we waiting for idle without an idle function?");

	mono_os_mutex_lock (&lock);

	while (continue_idle_job_func ())
		mono_os_cond_wait (&done_cond, &lock);

	mono_os_mutex_unlock (&lock);
}

void
sgen_thread_pool_wait_for_all_jobs (void)
{
	mono_os_mutex_lock (&lock);

	while (!sgen_pointer_queue_is_empty (&job_queue))
		mono_os_cond_wait (&done_cond, &lock);

	mono_os_mutex_unlock (&lock);
}

gboolean
sgen_thread_pool_is_thread_pool_thread (MonoNativeThreadId some_thread)
{
	return some_thread == thread;
}

#endif
