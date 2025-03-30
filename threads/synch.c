/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

static bool lock_waiter_has_lower_priority(const struct list_elem *, const struct list_elem *, void *);
static bool lock_with_lower_waiter_priority(const struct list_elem *, const struct list_elem *, void *);
static struct thread *find_most_important_waiter(struct list *);
static struct lock *find_most_contended_lock(struct list *);
static int get_dynamic_priority_safely(const struct thread *);
static void propagate_priority_to_holder(struct thread *);
static bool condvar_waiter_has_lower_priority(const struct list_elem *, const struct list_elem *, void *);


/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		list_insert_ordered(&sema->waiters, &thread_current()->elem, is_higher_priority, NULL);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
   void
   sema_up(struct semaphore *sema) {
	   enum intr_level previous_interrupt_state;
   
	   ASSERT(sema != NULL);
   
	   previous_interrupt_state = intr_disable();
   
	   struct thread *thread_to_wake = NULL;
   
	   if (!list_empty(&sema->waiters)) {
		   struct list_elem *max_priority_elem = NULL;
		   max_priority_elem = list_max(&sema->waiters, is_higher_priority, NULL);
		   thread_to_wake = list_entry(max_priority_elem, struct thread, elem);
		   list_remove(max_priority_elem);
		   thread_unblock(thread_to_wake);
	   }
   
	   sema->value++;
   
	   if (!intr_context() && thread_to_wake != NULL) {
		   struct thread *current_running_thread = thread_current();
		   if (thread_to_wake->dynamic_priority > current_running_thread->dynamic_priority) {
			   thread_yield();
		   }
	   }
   
	   intr_set_level(previous_interrupt_state);
   }
   

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);
	list_init (&lock->waiting_threads);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}


static int
get_dynamic_priority_safely(const struct thread *t) {
	if (t == NULL)
		return PRI_MIN;
	return t->dynamic_priority;
}

static struct thread *
find_most_important_waiter(struct list *waiting_threads_list) {
	if (waiting_threads_list == NULL || list_empty(waiting_threads_list))
		return NULL;

	struct list_elem *top_elem = list_max(waiting_threads_list, lock_waiter_has_lower_priority, NULL);
	return list_entry(top_elem, struct thread, waiting_list_elem);
}

static struct lock *
find_most_contended_lock(struct list *held_locks_list) {
	if (held_locks_list == NULL || list_empty(held_locks_list))
		return NULL;

	struct list_elem *target_elem = list_max(held_locks_list, lock_with_lower_waiter_priority, NULL);
	return list_entry(target_elem, struct lock, lock_list_elem);
}

static bool
lock_waiter_has_lower_priority(const struct list_elem *first_waiter_elem, const struct list_elem *second_waiter_elem, void *unused_context UNUSED) {
	const struct thread *waiter_one = list_entry(first_waiter_elem, struct thread, waiting_list_elem);
	const struct thread *waiter_two = list_entry(second_waiter_elem, struct thread, waiting_list_elem);

	int priority_one = get_dynamic_priority_safely(waiter_one);
	int priority_two = get_dynamic_priority_safely(waiter_two);

	return priority_one < priority_two;
}

static bool
lock_with_lower_waiter_priority(const struct list_elem *first_lock_elem, const struct list_elem *second_lock_elem, void *not_used UNUSED) {
	struct lock *first_lock = list_entry(first_lock_elem, struct lock, lock_list_elem);
	struct lock *second_lock = list_entry(second_lock_elem, struct lock, lock_list_elem);

	struct thread *top_waiter_first = find_most_important_waiter(&first_lock->waiting_threads);
	struct thread *top_waiter_second = find_most_important_waiter(&second_lock->waiting_threads);

	int priority_first = get_dynamic_priority_safely(top_waiter_first);
	int priority_second = get_dynamic_priority_safely(top_waiter_second);

	return priority_first < priority_second;
}

static void
propagate_priority_to_holder(struct thread *lock_holder_thread) {
	if (lock_holder_thread == NULL)
		return;

	int base_priority = lock_holder_thread->priority;
	int best_priority = base_priority;

	struct lock *dominant_lock = find_most_contended_lock(&lock_holder_thread->held_locks);

	if (dominant_lock != NULL) {
		struct thread *top_donor = find_most_important_waiter(&dominant_lock->waiting_threads);

		int donated_priority = get_dynamic_priority_safely(top_donor);

		if (donated_priority > best_priority) {
			best_priority = donated_priority;
		}
	}

	lock_holder_thread->dynamic_priority = best_priority;
	struct lock *upstream_lock = lock_holder_thread->lock_waiting_on;
	if (upstream_lock != NULL && upstream_lock->holder != NULL) {
		propagate_priority_to_holder(upstream_lock->holder);
	}
}



/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire(struct lock *lock) {
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(!lock_held_by_current_thread(lock));

	struct thread *cur = thread_current();
	struct thread *holder = lock->holder;

	if (thread_mlfqs) {
		sema_down(&lock->semaphore);
		lock->holder = cur;
		return;
	}

	if (holder != NULL) {
		list_push_back(&lock->waiting_threads, &cur->waiting_list_elem);

		cur->lock_waiting_on = lock;

		propagate_priority_to_holder(holder);
	}

	sema_down(&lock->semaphore);
	if (holder != NULL) {
		list_remove(&cur->waiting_list_elem);
	}

	cur->lock_waiting_on = NULL;
	list_push_back(&cur->held_locks, &lock->lock_list_elem);
	lock->holder = cur;
}


/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release(struct lock *lock) {
	ASSERT(lock != NULL);
	ASSERT(lock_held_by_current_thread(lock));

	if (thread_mlfqs) {
		lock->holder = NULL;
		sema_up(&lock->semaphore);
		return;
	}

	struct thread *holder = lock->holder;

	list_remove(&lock->lock_list_elem);
	lock->holder = NULL;
	propagate_priority_to_holder(holder);
	sema_up(&lock->semaphore);
}


/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	list_insert_ordered(&cond->waiters, &waiter.elem, condvar_waiter_has_lower_priority, NULL);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

static bool
condvar_waiter_has_lower_priority(const struct list_elem *first_list_elem, const struct list_elem *second_list_elem, void *auxiliary_data UNUSED) {
	struct semaphore_elem *sem_elem_a = list_entry(first_list_elem, struct semaphore_elem, elem);
	struct semaphore_elem *sem_elem_b = list_entry(second_list_elem, struct semaphore_elem, elem);

	struct semaphore *sema_a = &sem_elem_a->semaphore;
	struct semaphore *sema_b = &sem_elem_b->semaphore;

	struct list_elem *top_waiter_elem_a = list_max(&sema_a->waiters, is_higher_priority, NULL);
	struct list_elem *top_waiter_elem_b = list_max(&sema_b->waiters, is_higher_priority, NULL);

	const struct thread *waiter_a = list_entry(top_waiter_elem_a, struct thread, elem);
	const struct thread *waiter_b = list_entry(top_waiter_elem_b, struct thread, elem);

	return waiter_a->dynamic_priority < waiter_b->dynamic_priority;
}




/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal(struct condition *cond, struct lock *lock UNUSED) {
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	if (!list_empty(&cond->waiters)) {
		struct list_elem *target_elem = list_max(&cond->waiters, condvar_waiter_has_lower_priority, NULL);
		list_remove(target_elem);
		struct semaphore_elem *sem_elem = list_entry(target_elem, struct semaphore_elem, elem);
		sema_up(&sem_elem->semaphore);

	}
}



/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
