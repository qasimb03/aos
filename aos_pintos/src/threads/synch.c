#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Forward declarations */
static bool compare_priority (const struct list_elem *a,
                              const struct list_elem *b,
                              void *aux UNUSED);

/* ---------------------------------------------------------------------- */
/* Semaphore                                                              */
/* ---------------------------------------------------------------------- */

/* Initializes semaphore SEMA to VALUE. */
void
sema_init (struct semaphore *sema, unsigned value)
{
  ASSERT (sema != NULL);
  sema->value = value;
  list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore. Wait for value to become > 0, then decrement. */
void
sema_down (struct semaphore *sema)
{
  enum intr_level old_level = intr_disable ();
  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  while (sema->value == 0)
  {
    /* Insert the thread into the semaphore's waiters by priority. */
    list_insert_ordered (&sema->waiters, &thread_current ()->elem,
                         compare_priority, NULL);
    thread_block ();
  }
  sema->value--;
  intr_set_level (old_level);
}

/* Down operation, but don't block if already 0. */
bool
sema_try_down (struct semaphore *sema)
{
  enum intr_level old_level = intr_disable ();
  bool success = false;
  ASSERT (sema != NULL);

  if (sema->value > 0)
  {
    sema->value--;
    success = true;
  }
  intr_set_level (old_level);
  return success;
}

/* Up or "V" operation on a semaphore. Increment value, wake up highest-priority waiter. */
void
sema_up (struct semaphore *sema)
{
  enum intr_level old_level = intr_disable ();
  ASSERT (sema != NULL);

  if (!list_empty (&sema->waiters))
  {
    list_sort (&sema->waiters, compare_priority, NULL);
    struct thread *t = list_entry (list_pop_front (&sema->waiters),
                                   struct thread, elem);
    thread_unblock (t);
  }
  sema->value++;
  intr_set_level (old_level);

  /* If the unblocked thread has higher priority, yield. */
  if (!intr_context ())
  {
    enum intr_level old = intr_disable ();
    if (!list_empty (&ready_list))
    {
      struct thread *highest =
          list_entry (list_front (&ready_list), struct thread, elem);
      if (highest->priority > thread_current ()->priority)
        thread_yield ();
    }
    intr_set_level (old);
  }
}

void
sema_self_test (void)
{
  /* Not needed for Project 1 tests. */
}

/* ---------------------------------------------------------------------- */
/* Lock                                                                   */
/* ---------------------------------------------------------------------- */

/* Initializes LOCK. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);
  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);

  /* We'll also keep a list_elem so a lock can be in a thread's locks_held list. */
  /* (Add if you store it in the lock structure: 
       struct list_elem lock_elem; 
     declared in synch.h or so. 
     But here, let's assume you've done so if needed.)
  */
}

/* Acquire LOCK, sleeping until available if necessary.
   Implement priority donation here. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  struct thread *cur = thread_current ();
  struct thread *holder = lock->holder;

  /* If lock is held and we have higher priority, donate. */
  if (holder != NULL && holder != cur)
  {
    /* Record that current thread is waiting on this lock. */
    cur->waiting_lock = lock;

    /* Donate current priority to the lock holder if needed. */
    donate_priority (cur, holder);

    /* Now proceed to down the semaphore (which may block). */
  }

  sema_down (&lock->semaphore);
  /* We have the lock now. */
  lock->holder = cur;
  cur->waiting_lock = NULL;

  /* The current thread now holds this lock. Add to locks_held. */
  list_push_back (&cur->locks_held, &lock->lock_elem);
}

/* Try to acquire LOCK, return true if successful. */
bool
lock_try_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  bool success = sema_try_down (&lock->semaphore);
  if (success)
  {
    lock->holder = thread_current ();
    list_push_back (&thread_current ()->locks_held, &lock->lock_elem);
  }
  return success;
}

/* Release LOCK, remove donations, etc. */
void
lock_release (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  struct thread *cur = thread_current ();
  lock->holder = NULL;

  /* Remove this lock from cur's list of locks_held. */
  list_remove (&lock->lock_elem);

  /* Now remove any donation that was from threads waiting on this lock. */
  remove_lock_donation (cur, lock);

  /* Recalculate the current thread's priority, which may drop. */
  refresh_priority (cur);

  sema_up (&lock->semaphore);
}

/* Returns true if the current thread holds LOCK. */
bool
lock_held_by_current_thread (const struct lock *lock)
{
  ASSERT (lock != NULL);
  return lock->holder == thread_current ();
}

/* ---------------------------------------------------------------------- */
/* Condition Variables                                                    */
/* ---------------------------------------------------------------------- */

/* One semaphore in a list, for condition variables. */
struct semaphore_elem
{
  struct list_elem elem;
  struct semaphore semaphore;
  int priority; /* The priority of the highest-priority thread waiting here. */
};

/* Initializes condition variable COND. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);
  list_init (&cond->waiters);
}

/* Wait on COND, releasing LOCK. When signaled, reacquire LOCK. */
void
cond_wait (struct condition *cond, struct lock *lock)
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  struct semaphore_elem waiter;
  sema_init (&waiter.semaphore, 0);

  /* Record the current thread's priority in waiter. */
  waiter.priority = thread_current ()->priority;

  /* Insert in sorted order by priority. */
  list_insert_ordered (&cond->waiters, &waiter.elem,
                       cond_sema_priority_compare, NULL);

  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/* Signals one waiting thread to wake up. Must hold LOCK. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters))
  {
    /* Pop the highest-priority waiter. */
    list_sort (&cond->waiters, cond_sema_priority_compare, NULL);
    struct semaphore_elem *sem_elem =
        list_entry (list_pop_front (&cond->waiters),
                    struct semaphore_elem, elem);
    sema_up (&sem_elem->semaphore);
  }
}

/* Wakes up all threads, if any, waiting on COND. */
void
cond_broadcast (struct condition *cond, struct lock *lock)
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}

/* ---------------------------------------------------------------------- */
/*                      Internal Helper Functions                         */
/* ---------------------------------------------------------------------- */

/* Compare threads by priority, descending. */
static bool
compare_priority (const struct list_elem *a,
                  const struct list_elem *b,
                  void *aux UNUSED)
{
  const struct thread *t_a = list_entry (a, struct thread, elem);
  const struct thread *t_b = list_entry (b, struct thread, elem);
  return t_a->priority > t_b->priority;
}

/* Compare two semaphore_elem objects by the priority stored in them. */
bool
cond_sema_priority_compare (const struct list_elem *a,
                            const struct list_elem *b,
                            void *aux UNUSED)
{
  const struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
  const struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);
  return sa->priority > sb->priority;
}