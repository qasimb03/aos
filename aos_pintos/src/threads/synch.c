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

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void sema_init (struct semaphore *sema, unsigned value)
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA"s value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void sema_down (struct semaphore *sema)
{
  ASSERT (sema != NULL);
  ASSERT (!intr_context ()); 
  enum intr_level old_level = intr_disable(); /* Disable interrupts */
  while (sema->value == 0) /* Waiting on resource */
    {
      list_insert_ordered (&sema->waiters, &thread_current()->elem, compareThreadPriority, NULL); /* Insert thread into waiting list of semaphore in priority order */
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool sema_try_down (struct semaphore *sema)
{
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

/* Up or "V" operation on a semaphore.  Increments SEMA"s value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void sema_up (struct semaphore *sema)
{
  
  ASSERT (sema != NULL);
  
  enum intr_level old_level = intr_disable ();
  if (!list_empty (&sema->waiters)) {
    list_sort(&sema->waiters, compareThreadPriority, NULL); /* Resorts list by priority as priority donation could have messed with order */
    thread_unblock (list_entry(list_pop_front (&sema->waiters), struct thread, elem));
  }
  sema->value++;

  /* If not called within an interrupt handler, yield to the CPU.
   This would, if newly unblocked thread is higher priority, to
   be run as scheduler would select it from ready_list. CPU might
   select same thread to run if no other higher priority processes. */
  if (!intr_context()) {
    thread_yield();  
  } 
  
  intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what"s going on. */
void sema_self_test (void)
{
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
static void sema_test_helper (void *sema_)
{
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
   onerous, it"s a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  enum intr_level oldInterruptLevel = intr_disable(); /* Disable interrupts to ensure atomic list operations */
  struct thread *current_thread = thread_current(); /* Current thread is the thread that needs/waiting on lock */

  /* Some other process already holds the lock that this thread needs */
  if (lock->holder) 
  {
    struct thread *thread_holding_lock = lock->holder;
    current_thread->lock_waiting_for = lock; /* Stores pointer what lock the current thread is waiting for */

    /* Donate priority to thread that holds lock if its priority lower than thread waiting on the lock */
     if (thread_holding_lock->priority < current_thread->priority) {
       list_insert_ordered (&lock->holder->threads_waiting_on_this_thread, &current_thread->threads_waiting_elem, compareWaitingThreadsPriority, NULL); /* Add donor thread to list of donors for thread that holds the lock */

       int i = 0;

       /* Handles nested donation */
       while (i < 8)
       {
        if (!current_thread->lock_waiting_for) /* Current thread is not waiting on a lock, break out of loop */
        {
          break;
        }

        /* Otherwise, propagate donations throughout nest of lock donations  */
        thread_holding_lock = current_thread->lock_waiting_for->holder;
        thread_holding_lock->priority = current_thread->priority; /* Set priority of thread that has lock to higher priority (of thread waiting on lock)*/
        current_thread = thread_holding_lock; /* Move forward down the chain of locks */
        i++;
       }
    }
  }

  intr_set_level(oldInterruptLevel); /* Set interrupts back to old value */
  sema_down (&lock->semaphore); /* If another thread holds this lock, this thread will get added to semaphore's waiter list within the sema_down function */
  
  current_thread->lock_waiting_for = NULL; /* Thread is not waiting for a lock, it can acquire it because not held by anyone else */
  lock->holder = thread_current ();
}

/* Tries to acquires LOCK and returns true if successful or false
on failure.  The lock must not already be held by the current
thread.

This function will not sleep, so it may be called within an
interrupt handler. */
bool lock_try_acquire (struct lock *lock)
{
  bool success;
  
  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));
  
  success = sema_try_down (&lock->semaphore);
  if (success)
  lock->holder = thread_current ();
  return success;
}

/* Releases LOCK, which must be owned by the current thread.

An interrupt handler cannot acquire a lock, so it does not
make sense to try to release a lock within an interrupt
handler. */
void lock_release (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));
  struct thread *current_thread = thread_current();
  
  /* After releasing a lock, removes thread that was waiting on that lock from list of thread's donors. */
  for (struct list_elem *e = list_begin (&current_thread->threads_waiting_on_this_thread); e != list_end (&current_thread->threads_waiting_on_this_thread); e = list_next (e)){
    struct thread *t = list_entry (e, struct thread, threads_waiting_elem);
    if (t->lock_waiting_for == lock){
      list_remove (&t->threads_waiting_elem);
    }
  }
  
  /* Reset priority to original priority of thread */
  //thread_set_priority(current_thread->actual_priority);
  
  current_thread->priority = current_thread->actual_priority;
  /* If there are threads still waiting on this thread to release a lock, set priority to priority of highest thread that is waiting on thread to release lock */
  if (!list_empty(&current_thread->threads_waiting_on_this_thread)) 
  {
    list_sort(&current_thread->threads_waiting_on_this_thread, compareWaitingThreadsPriority, NULL);
    if (list_entry(list_front(&current_thread->threads_waiting_on_this_thread), struct thread, threads_waiting_elem)->priority > current_thread->priority)
    {
      current_thread->priority = list_entry(list_front(&current_thread->threads_waiting_on_this_thread), struct thread, threads_waiting_elem)->priority;
    }
    lock->holder = NULL;
  }
  
  lock->holder = NULL;
  sema_up (&lock->semaphore); /* Unblock thread waiting on lock through lock's semaphore */
}

/* Returns true if the current thread holds LOCK, false
otherwise.  (Note that testing whether some other thread holds
a lock would be racy.) */
bool lock_held_by_current_thread (const struct lock *lock)
{
  ASSERT (lock != NULL);
  
  return lock->holder == thread_current ();
}

/* One semaphore in a list. 

Each thread that calls cond_wait creates a semaphore_elem. This semaphore element 
contains a semaphore. This semaphore_elem"s semaphore"s waiter list contains a list of 
blocked threads waiting on this semaphore, which is JUST the thread that created the 
semaphore_elem (and semaphore). */
struct semaphore_elem
{
  struct list_elem elem;      /* List element. */
  struct semaphore semaphore; /* This semaphore. */
  int priority;
};

/* Initializes condition variable COND.  A condition variable
allows one piece of code to signal a condition and cooperating
code to receive the signal and act upon it. */
void cond_init (struct condition *cond)
{
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
void cond_wait (struct condition *cond, struct lock *lock)
{
  /* Creates a semaphore_elem waiter which 
  is used to block thread that calls it. The 
  semaphore_elem"s semaphore is decremented to 0,
  which blocks thread that calls it, and this thread 
  is added to the semaphore"s waiter list (list of threads 
  waiting on this semaphore -- just contains 1 thread [thread that called it]
  ).*/
  struct semaphore_elem waiter;
  
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0); /* Initialize semaphore_elem to 0 (blocks itself until signal) */
  waiter.priority = thread_get_priority();
  list_insert_ordered (&cond->waiters, &waiter.elem, compareSemaphorePriority, NULL); /* Insert thread into waiting list in priority order */
  lock_release (lock); /* Release lock to allow other threads to access it */
  sema_down (&waiter.semaphore); /* Blocks the thread that is waiting on the condition variable until it is signalled to be unblocked */
  lock_acquire (lock); /* Acquire lock */
}

/* If any threads are waiting on COND (protected by LOCK), then
this function signals one of them to wake up from its wait.
LOCK must be held before calling this function.

An interrupt handler cannot acquire a lock, so it does not
make sense to try to signal a condition variable within an
interrupt handler. */
void cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  
  /* Increments the semaphore associated with the semaphore_elem 
  in the list of "threads" waiting on this condition variable. 
  This unblocks the thread that is waiting for this condition variable. */
  if (!list_empty (&cond->waiters)) {
    list_sort(&cond->waiters, compareSemaphorePriority, NULL);
    sema_up(&list_entry (list_pop_front (&cond->waiters), struct semaphore_elem, elem)->semaphore);
  }
}

/* Wakes up all threads, if any, waiting on COND (protected by
LOCK).  LOCK must be held before calling this function.

An interrupt handler cannot acquire a lock, so it does not
make sense to try to signal a condition variable within an
interrupt handler. */
void cond_broadcast (struct condition *cond, struct lock *lock)
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  
  while (!list_empty (&cond->waiters))
  cond_signal (cond, lock);
}


/* Each thread that calls condition variable creates a semaphore_elem. This semaphore element 
contains a semaphore. This semaphore_elem's semaphore"s waiter list contains a list of 
blocked threads waiting on this semaphore, which is JUST the thread that created the 
semaphore_elem (and semaphore). 

This function is used to get the highest priority thread that is waiting on a condition variable, by
seeing which thread has the highest priority. In achieves this by going through the list of semaphore_elem
that are waiting on a condition variable (&cond->waiters), going to that semaphore_elem's semaphore, grabbing the thread
associated with that semaphore, and checking its priority. */
bool compareSemaphorePriority (const struct list_elem *list_item_a, const struct list_elem *list_item_b) {

  struct semaphore_elem *semaphore_elem_a = list_entry(list_item_a, struct semaphore_elem, elem);
  struct semaphore_elem *semaphore_elem_b = list_entry(list_item_b, struct semaphore_elem, elem);
  /* Update priority in semaphore_elem as priority of thread in semaphore_elem.waiters list (thread that created semaphore_elem) might have changed */
  semaphore_elem_a->priority = list_entry(list_begin(&semaphore_elem_a->semaphore.waiters), struct thread, elem)->priority;
  semaphore_elem_b->priority = list_entry(list_begin(&semaphore_elem_b->semaphore.waiters), struct thread, elem)->priority;
  return semaphore_elem_a->priority > semaphore_elem_b->priority;
}



