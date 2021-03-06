#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "devices/timer.h"
#include "threads/fixed-point.h"
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

/* List of processes in THREAD_BLOCKED state that are SLEEPING.
   This list should always be sorted by ascending wake up time. */
static struct list blocked_sleeping_list;

/* Array of queues used for MLFQ scheduling. The queue at each index i is used
   to store threads with priority i. */
#define MLFQ_QUEUE_SIZE (PRI_MAX - PRI_MIN + 1)
struct mlfq {
  int size;
  struct list queues[MLFQ_QUEUE_SIZE];
};
static struct mlfq thread_mlfq;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Estimates the number of threads ready to run over the past minute. */
static fixed_point_t load_avg;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use priority scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&blocked_sleeping_list);
  for (int i = PRI_MIN; i <= PRI_MAX; i++) {
    list_init(&thread_mlfq.queues[i]);
  }
  thread_mlfq.size = 0;

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->recent_cpu = fix_int(0);
  initial_thread->nice = 0;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

static void try_wake_up_sleeping_threads(void) {
	int64_t ticks = timer_ticks();
	while (true) {
	  if (list_empty(&blocked_sleeping_list)) {
	    return;
	  }
    struct list_elem *it = list_front(&blocked_sleeping_list);
    struct thread *t = list_entry(it, struct thread, elem);
    if (t->blocked.sleeping_wakeup_time > ticks) {
      // blocked_sleeping_list sorted by ascending wakeup time. Therefore if
      // the first wake up is in the future, then all of them are.
      return;
    }
    // Must pop from blocked sleeping list before adding thread to the ready list.
    // Otherwise thread.elem will be in 2 lists simultaneously, which isn't allowed.
    list_pop_front(&blocked_sleeping_list);
    thread_unblock(t);
  }
}

static int mlfq_get_priority(struct thread* t) {
  int unbound_priority = PRI_MAX - fix_round(fix_div(t->recent_cpu, fix_int(4))) - (t->nice * 2);
  if (unbound_priority > PRI_MAX) {
    return PRI_MAX;
  }
  if (unbound_priority < 0) {
    return 0;
  }
  return unbound_priority;
}

static void mlfq_update() {
  for (int i = PRI_MIN; i <= PRI_MAX; i++) {
    struct list_elem* elem = list_begin(&thread_mlfq.queues[i]);

    while (elem != list_end(&thread_mlfq.queues[i])) {
      struct thread* t = list_entry(elem, struct thread, mlfq_elem);
      int new_priority = mlfq_get_priority(t);
      struct list_elem* next_elem = list_next(elem);
      if (new_priority != i) {
        list_remove(elem);
        list_push_back(&thread_mlfq.queues[new_priority], elem);
      }
      elem = next_elem;
    }
  }
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (t != idle_thread) {
    t->recent_cpu = fix_increment(t->recent_cpu);
  }

  if (timer_ticks() % TIMER_FREQ == 0 && thread_mlfqs) {
    // Update the load avg.
    int threads_ready_to_run = thread_mlfq.size;
    if (t != idle_thread) {
      // Current thread is also ready to run.
      threads_ready_to_run++;
    }
    load_avg = fix_add(
                 fix_mul(fix_frac(59, 60), load_avg),
                 fix_mul(fix_frac(1, 60), fix_int(threads_ready_to_run)));

    // Once per second, update the recent_cpu for all threads.
    for (struct list_elem* e = list_begin (&all_list); e != list_end (&all_list); e = list_next(e)) {
      struct thread* thread_it = list_entry (e, struct thread, allelem);
      if (thread_it == idle_thread) {
        continue;
      }
      fixed_point_t recent_cpu_scale =
        fix_div(
          fix_mul(fix_int(2), load_avg),
          fix_add(
              fix_mul(fix_int(2), load_avg),
              fix_int(1)));
      thread_it->recent_cpu = fix_add(fix_mul(recent_cpu_scale, thread_it->recent_cpu), fix_int(thread_it->nice));
    }

    mlfq_update();
  }

  try_wake_up_sleeping_threads();

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   Priority is ignored if MLFQ scheduling is enabled. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  // Inherit recent_cpu and nice values from parent. */
  t->recent_cpu = thread_current()->recent_cpu;
  t->nice = thread_current()->nice;
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to ready queue. */
  thread_unblock (t);

  if (priority > thread_current()->priority && !thread_mlfqs) {
    thread_yield();
  }

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

void
thread_set_block_reason_waiting_on_lock (struct lock* lock) {
  struct thread_blocked blocked_reason = {
      WAITING_ON_LOCK, .lock = lock
  };
  thread_current()->blocked = blocked_reason;
}

static bool sleeping_thread_less_func (const struct list_elem *a,
                                       const struct list_elem *b,
                                       void *aux UNUSED) {
  struct thread *thread_a = list_entry(a, struct thread, elem);
  struct thread *thread_b = list_entry(b, struct thread, elem);
  return thread_a->blocked.sleeping_wakeup_time < thread_b->blocked.sleeping_wakeup_time;
}

void
thread_sleep_until (int64_t ticks) {
  struct thread *cur = thread_current ();
  enum intr_level old_level;

  ASSERT (!intr_context ());

  old_level = intr_disable ();
  cur->status = THREAD_BLOCKED;
  cur->blocked.reason = SLEEPING;
  cur->blocked.sleeping_wakeup_time = ticks;
  if (cur != idle_thread) {
    list_insert_ordered(&blocked_sleeping_list, &cur->elem, sleeping_thread_less_func, NULL);
  }

  schedule ();
  intr_set_level (old_level);
}

// Threads with higher priority come before threads with lower priority.
static bool priority_thread_less_func(const struct list_elem *a,
                                      const struct list_elem *b,
                                      void *aux __attribute__ ((unused))) {
  struct thread *thread_a = list_entry(a, struct thread, elem);
  struct thread *thread_b = list_entry(b, struct thread, elem);
  return thread_get_effective_priority(thread_a) > thread_get_effective_priority(thread_b);
}

// The MLFQ data structure is not thread-safe, so this function must be called
// with interrupts disabled.
static void mlfq_add_thread(struct thread* t) {
  thread_mlfq.size++;
  int priority = mlfq_get_priority(t);
  list_push_back(&thread_mlfq.queues[priority], &t->mlfq_elem);
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  if (thread_mlfqs) {
    mlfq_add_thread(t);
  } else {
    list_insert_ordered(&ready_list, &t->elem, priority_thread_less_func, NULL);
  }
  t->status = THREAD_READY;
  t->blocked.reason = UNKNOWN;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
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

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) {
    if (thread_mlfqs) {
      mlfq_add_thread(cur);
    } else {
      list_insert_ordered(&ready_list, &cur->elem, priority_thread_less_func, NULL);
    }
  }
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  thread_current ()->priority = new_priority;
  // Re-run priority scheduling.
  thread_yield();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_get_effective_priority(thread_current());
}

static void thread_receive_donated_priority(struct thread* t, int donated_priority) {
  if (donated_priority > t->donated_priority) {
    t->donated_priority = donated_priority;
    if (t->status == THREAD_READY) {
      list_remove(&t->elem);
      list_insert_ordered(&ready_list, &t->elem, priority_thread_less_func, NULL);
    }
  }
}

/* Set the priority of the receiver thread. If the receiver thread is blocked
 * on waiting for a lock, then the lock owner will also have it's priority set.
 * This priority-setting continues recursively until a thread that isn't
 * waiting on a lock is found.
 *
 * Must be called with interrupts disabled. Expectation is that a scheduling decision
 * will be run shortly after this executes. */
void
thread_donate_priority(struct thread* receiver, int priority) {
  // The donated priority should be the greatest of all waiting threads.
  thread_receive_donated_priority(receiver, priority);
  struct thread* cur_thread = receiver;
  while (cur_thread->status == THREAD_BLOCKED && cur_thread->blocked.reason == WAITING_ON_LOCK) {
    cur_thread = cur_thread->blocked.lock->holder;
    thread_receive_donated_priority(cur_thread, priority);
  }
}

int thread_get_effective_priority(struct thread* t) {
  return (t->donated_priority > t->priority) ? t->donated_priority : t->priority;
}

/* Calculate the effective priority of a thread. Taking donated priorities into account. Should be called with interrupts disabled. */
int thread_calculate_donated_priority(struct thread* t) {
  // Take maximum priority of threads waiting on owned locks.
  int max_priority = 0;
  for (struct list_elem* elem = list_begin(&t->owned_locks) ; elem != list_end(&t->owned_locks); elem = list_next(elem)) {
    struct lock* lock = list_entry (elem, struct lock, elem);
    struct list* waiting_thread_list = &lock->semaphore.waiters;
    for (struct list_elem* thread_elem = list_begin(waiting_thread_list) ; thread_elem != list_end(waiting_thread_list); thread_elem = list_next(thread_elem)) {
      struct thread* waiting_thread = list_entry (thread_elem, struct thread, elem);
      int waiter_effective_priority = thread_get_effective_priority(waiting_thread);
      if (waiter_effective_priority > max_priority) {
        max_priority = waiter_effective_priority;
      }
    }
  }
  return max_priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  // Ensure that -20 <= nice <= 20.
  if (nice > 20) {
    nice = 20;
  } else if (nice < -20) {
    nice = -20;
  }
  thread_current()->nice = nice;
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return fix_round(fix_mul(load_avg, fix_int(100)));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return fix_round(fix_mul(thread_current()->recent_cpu, fix_int(100)));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the current thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  t->blocked.reason = UNKNOWN;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->donated_priority = 0;
  t->magic = THREAD_MAGIC;
  list_init(&t->owned_locks);

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (thread_mlfqs) {
    if (thread_mlfq.size == 0)
      return idle_thread;
    for (int i = PRI_MAX; i >= PRI_MIN; i--) {
      if (!list_empty(&thread_mlfq.queues[i])) {
        thread_mlfq.size--;
        return list_entry(list_pop_front(&thread_mlfq.queues[i]), struct thread, mlfq_elem);
      }
    }
  } else {
    if (list_empty (&ready_list))
        return idle_thread;
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
  }
  NOT_REACHED();
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
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

/* Returns a tid to use for a new thread. */
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

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
