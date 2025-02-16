#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* For scheduling. */
#define TIME_SLICE 4
static unsigned thread_ticks;
bool thread_mlfqs;

/* Forward declarations. */
static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static bool is_thread (struct thread *) UNUSED;
static void init_thread (struct thread *, const char *name, int priority);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Comparator used by list_insert_ordered() for threads, comparing priorities. */
static bool compare_priority (const struct list_elem *a,
                              const struct list_elem *b,
                              void *aux UNUSED);

/* ---------------------------------------------------------------------- */
/*    Priority Donation Helper Functions                                  */
/* ---------------------------------------------------------------------- */

/* Recompute a thread's effective priority = max(base_priority, any donations).
   Also propagate donation upward if this thread itself is holding a lock
   for which a higher-priority thread is waiting. */
void refresh_priority (struct thread *t);

/* Donate `donor`'s priority to `t` if higher, then recursively propagate. */
void donate_priority (struct thread *donor, struct thread *t);

/* Remove any donation(s) related to LOCK from thread T and refresh T's priority. */
void remove_lock_donation (struct thread *t, struct lock *lock);

/* ---------------------------------------------------------------------- */

/* Initializes the threading system, called by main() in Pintos init. */
void
thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  /* Initialize the first (initial) thread context. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Creates the idle thread and starts scheduling. */
void
thread_start (void)
{
  struct semaphore idle_started;
  sema_init (&idle_started, 0);

  /* Create idle thread. */
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start interrupts (scheduler). */
  intr_enable ();

  /* Wait for idle thread to get going. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick. */
void
thread_tick (void)
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    ;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    ;
#endif
  else
    ;

  /* Enforce preemption if time slice is over. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints stats for debugging. */
void
thread_print_stats (void)
{
  printf ("Thread: no stats.\n");
}

/* Create a new kernel thread named NAME with the given initial PRIORITY,
   which executes FUNCTION passing AUX. */
tid_t
thread_create (const char *name, int priority, thread_func *function, void *aux)
{
  struct thread *t;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate and clear page for the new thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread fields. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Build a minimal stack frame for kernel_thread(). */
  struct kernel_thread_frame
  {
    void *eip;             /* Return address. */
    thread_func *function; /* Function to call. */
    void *aux;             /* Auxiliary data for function. */
  };
  struct switch_entry_frame
  {
    void (*eip) (void);
  };
  struct switch_threads_frame
  {
    void (*eip) (void);
    uint32_t ebp;
  };

  /* Stack frame for kernel_thread(). */
  struct kernel_thread_frame *kf
      = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  struct switch_entry_frame *ef
      = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  struct switch_threads_frame *sf
      = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Ready to run. */
  thread_unblock (t);

  /* If the newly created thread has higher priority, yield. */
  if (t->priority > thread_current ()->priority)
    thread_yield ();

  return tid;
}

/* Blocks the current thread. */
void
thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions thread T from blocked to ready-to-run. */
void
thread_unblock (struct thread *t)
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered (&ready_list, &t->elem, compare_priority, NULL);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void)
{
  return thread_current ()->name;
}

/* Returns the currently running thread. */
struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);
  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void)
{
  return thread_current ()->tid;
}

/* Destroys the current thread; never returns. */
void
thread_exit (void)
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  intr_disable ();
  list_remove (&thread_current ()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU. The current thread is not put to sleep. */
void
thread_yield (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;

  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread)
    list_insert_ordered (&ready_list, &cur->elem, compare_priority, NULL);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* For each thread, call FUNC(thread, aux). */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
  {
    struct thread *t = list_entry (e, struct thread, allelem);
    func (t, aux);
  }
}

/* ---------------------------------------------------------------------- */
/*                  Priority & Donation Mechanics                         */
/* ---------------------------------------------------------------------- */

/* Sets the current thread's BASE priority to NEW_PRIORITY and
   recalculates its effective priority (possibly yields). */
void
thread_set_priority (int new_priority)
{
  enum intr_level old_level = intr_disable ();
  struct thread *cur = thread_current ();

  cur->base_priority = new_priority;
  refresh_priority (cur);

  /* If we're no longer the highest-priority thread, yield. */
  if (!list_empty (&ready_list))
  {
    struct thread *highest =
        list_entry (list_front (&ready_list), struct thread, elem);
    if (highest->priority > cur->priority)
      thread_yield ();
  }
  intr_set_level (old_level);
}

/* Returns the current thread's effective priority. */
int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

/* Helpers for Donation. -------------------------------------- */

/* Recompute t->priority = max(t->base_priority, any donations from locks).
   If t->waiting_lock != NULL, propagate new priority to the holder. */
void
refresh_priority (struct thread *t)
{
  int old_priority = t->priority;
  /* Start with base priority. */
  t->priority = t->base_priority;

  /* Check all locks that t holds, find highest waiting thread's priority. */
  if (!list_empty (&t->locks_held))
  {
    struct list_elem *e;
    for (e = list_begin (&t->locks_held); e != list_end (&t->locks_held); e = list_next (e))
    {
      struct lock *lock_held = list_entry (e, struct lock, lock_elem);
      if (!list_empty (&lock_held->semaphore.waiters))
      {
        /* Highest-priority waiter on this lock. */
        list_sort (&lock_held->semaphore.waiters, compare_priority, NULL);
        struct thread *top_waiter =
            list_entry (list_front (&lock_held->semaphore.waiters),
                        struct thread, elem);
        if (top_waiter->priority > t->priority)
          t->priority = top_waiter->priority;
      }
    }
  }

  /* If priority changed, and t is waiting on a lock, donate upward. */
  if (t->priority != old_priority && t->waiting_lock != NULL)
  {
    struct thread *holder = t->waiting_lock->holder;
    donate_priority (t, holder);
  }
}

/* Donate donor->priority to t, if it's higher.
   Then, if t is waiting on another lock, propagate further. */
void
donate_priority (struct thread *donor, struct thread *t)
{
  if (donor->priority > t->priority)
  {
    t->priority = donor->priority;
    if (t->waiting_lock != NULL)
      donate_priority (t, t->waiting_lock->holder);
  }
}

/* Removes any donation from T that is linked to LOCK, then refresh T's priority. */
void
remove_lock_donation (struct thread *t, struct lock *lock)
{
  /* The typical approach is to recalc from scratch after removing the lock. */
  refresh_priority (t);
}

/* ---------------------------------------------------------------------- */
/*                     Advanced Scheduler Stubs                           */
/* ---------------------------------------------------------------------- */

void
thread_set_nice (int nice UNUSED)
{
  /* Not implemented. */
}

int
thread_get_nice (void)
{
  return 0;
}

int
thread_get_load_avg (void)
{
  return 0;
}

int
thread_get_recent_cpu (void)
{
  return 0;
}

/* ---------------------------------------------------------------------- */
/*                           Internal Helpers                              */
/* ---------------------------------------------------------------------- */

/* Idle thread.  Executes when no other thread is ready. */
static void
idle (void *idle_started_ UNUSED)
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;)
  {
    intr_disable ();
    thread_block ();
    /* Re-enable interrupts and wait for next. */
    asm volatile ("sti; hlt" : : : "memory");
  }
}

/* A kernel thread running FUNCTION(AUX). */
static void
kernel_thread (thread_func *function, void *aux)
{
  ASSERT (function != NULL);
  intr_enable ();
  function (aux);
  thread_exit ();
}

/* Returns the running thread. */
struct thread *
running_thread (void)
{
  uint32_t *esp;
  /* Copy stack pointer into `esp`, then round down to page boundary. */
  asm ("mov %%esp, %0" : "=g"(esp));
  return pg_round_down (esp);
}

/* Returns true if T is a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Initialize a thread struct T, named NAME, with priority PRIORITY. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  /* We'll treat t->priority as "effective" priority. */
  t->priority = priority;
  /* Also store a base priority. */
  t->base_priority = priority;
  /* Thread magic for debug checks. */
  t->magic = THREAD_MAGIC;

  /* No lock yet. */
  t->waiting_lock = NULL;
  list_init (&t->locks_held);

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocate a SIZE-byte frame at top of thread T's stack. */
void *
alloc_frame (struct thread *t, size_t size)
{
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Choose next thread to run from the ready list, or idle if empty. */
static struct thread *
next_thread_to_run (void)
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
  {
    /* Pop highest-priority thread. */
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
  }
}

/* Complete a thread switch. PREV is the old thread, already switched out. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  ASSERT (intr_get_level () == INTR_OFF);
  cur->status = THREAD_RUNNING;
  thread_ticks = 0;

#ifdef USERPROG
  process_activate ();
#endif

  /* If the old thread is dying, free its page. */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
  {
    ASSERT (prev != cur);
    palloc_free_page (prev);
  }
}

/* Switch to a new process, called with interrupts off. */
static void
schedule (void)
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a new TID for a thread. */
static tid_t
allocate_tid (void)
{
  static tid_t next_tid = 1;
  tid_t tid;
  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);
  return tid;
}

/* Compare priorities between two threads for list ordering (descending). */
static bool
compare_priority (const struct list_elem *a,
                  const struct list_elem *b,
                  void *aux UNUSED)
{
  const struct thread *t_a = list_entry (a, struct thread, elem);
  const struct thread *t_b = list_entry (b, struct thread, elem);
  return t_a->priority > t_b->priority;
}

uint32_t thread_stack_ofs = offsetof (struct thread, stack);