#include <signal.h>
#include <stdint.h>
#include <sys/ucontext.h>
#include <time.h>
#include <ucontext.h>
#ifdef DEBUG
#include <stdio.h>
#endif

#include "minicoru.h"

#define MC_MAX_N_MC 1 << 11
#define MC_TIMER_CLOCK_ID CLOCK_REALTIME
#define MC_TIMER_SIG SIGRTMIN

#define likely(x) __builtin_expect ((x), 1)
#define unlikely(x) __builtin_expect ((x), 0)

static uint32_t mc_global_id_counter = 0;
static int32_t mc_n_suspending = 0;

#ifdef DEBUG
static uint32_t mc_stat_n_coru_created = 0;
static uint32_t mc_stat_n_coru_freed = 0;
#endif

enum mc_core_state MC_STATE = MC_UNINT;

typedef struct
{
  size_t capacity;
  size_t size;
  void **head, **tail;
  void *array[];
} queue_t;

minicoru_t *mc_current_mc;
queue_t *mc_waiters;
minicoru_t *mc_scheduler;

queue_t *
q_new (const int maxsize)
{
  queue_t *q = malloc (sizeof (queue_t) + maxsize * sizeof (void *));

  if (unlikely (!q))
    return NULL;
  q->capacity = maxsize;
  q->head = q->tail = q->array;
  q->size = 0;
  return q;
}

int
q_is_full (queue_t const *q)
{
  return q->size >= q->capacity;
}

int
q_is_empty (queue_t const *q)
{
  return q->size <= 0;
}

#ifdef DEBUG
#if MC_STKSIZ < (16 << 10)
#warning potentially risk of SEGV, due to the size of stack `MC_STKSIZ` is too small
#endif
#define BLACK "\033[0;30m"
#define RED "\033[0;31m"
#define GREEN "\033[0;32m"
#define YELLOW "\033[0;33m"
#define BLUE "\033[0;34m"
#define PURPLE "\033[0;35m"
#define CYAN "\033[0;36m"
#define WHITE "\033[0;37m"
#define RESET "\033[0m"

#define LOG_DEBUG(fmt, ...)                                                   \
  fprintf (stderr, GREEN __FILE__ ":%10s:L%4d " RESET fmt,                    \
           __PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__)
// void q_print(queue_t *q);

#else

#define LOG_DEBUG(...) ;

#endif

static inline __attribute__ ((always_inline)) void
assert (int cond, const char *msg)
{
  if (unlikely (!cond))
    {
#ifdef DEBUG
      puts (msg);
#endif
      abort ();
    }
}

void
q_push_unsafe (queue_t *q, void *elem)
{
  *(q->tail) = elem;
  void **new_tail = q->tail + 1;
  q->tail = (new_tail - q->array) > q->capacity ? new_tail - q->capacity
                                                : new_tail;
  ++q->size;
}

void *
q_pop_unsafe (queue_t *q)
{
  void *result = *(q->head);
  void **new_head = q->head + 1;
  q->head = (new_head - q->array) > q->capacity ? new_head - q->capacity
                                                : new_head;
  --q->size;
  return result;
}

int
q_push (queue_t *q, void *elem)
{
#ifdef DEBUG
// q_print(q);
#endif
  if (q_is_full (q))
    return -1;
  q_push_unsafe (q, elem);
  return 0;
}

void *
q_pop (queue_t *q)
{
  // q_print(q);
  if (q_is_empty (q))
    return NULL;
  return q_pop_unsafe (q);
}

void mc_arrange (minicoru_t *t);

#ifdef DEBUG
// void q_print(queue_t *q) {
//   char buf[1 << 9];
//   sprintf(buf, "WAITING_Q [SIZE %zu]:", q->size);
//   int offset = q->head - q->array;
//   sprintf(buf, "offset %d:", offset);
//   for (int i = 0; i < q->size; ++i)
//     sprintf(buf, " %d", ((minicoru_t *)(q->array[(offset + i) %
//     q->capacity]))->id);
//   LOG_DEBUG("%s\n", buf);
// }
#endif

void
mc_wait (minicoru_t *mc)
{
  if (unlikely (MC_STATE != MC_RUNNING || !mc))
    {
      abort ();
    }
  if (mc->state == MC_STATE_RUNNING)
    {
      q_push (mc_waiters, mc);
      mc->state = MC_STATE_WAITING;
      return;
    }
  LOG_DEBUG ("#%u in illegal state %d called wait\n", mc->id, mc->state);
}

void
mc_notify (minicoru_t *mc)
{
  assert (mc == mc_current_mc, "Illegal state: notify from non-current mc");
  if (unlikely (!mc->notify.coru))
    {
      LOG_DEBUG ("[Notify] #%d failed.\n", mc_current_mc->id);
      abort ();
    }
  LOG_DEBUG ("[Notify] %d\n", mc_current_mc->notify.coru->id);
  mc_arrange (mc->notify.coru);
}

void
mc_fin (minicoru_t *mc, void *ret)
{
  mc->result = ret;
  mc->state = MC_STATE_FIN;
  if (mc->notify.coru)
    mc_notify (mc);
}

static inline __attribute__ ((always_inline)) void
mc_suspend (minicoru_t *t)
{
  t->state = MC_STATE_SUSPENDING;
  ++mc_n_suspending;
}

minicoru_t *
mc_new ()
{
  minicoru_t *t = malloc (sizeof (minicoru_t));
#ifdef DEBUG
  ++mc_stat_n_coru_created;
#endif
  if (unlikely (!t))
    abort ();
  t->id = ++mc_global_id_counter;
  if (unlikely (getcontext (&t->ctx)))
    {
#ifdef DEBUG
      perror ("failed to get context\n");
#endif
      abort ();
    }
  t->type = MC_TYPE_WORKER;
  t->ctx.uc_stack.ss_sp = t->stack;
  t->ctx.uc_stack.ss_size = MC_STKSIZ;
  t->ctx.uc_link = NULL;
  t->timer = 0;
  mc_suspend (t);
  return t;
}

minicoru_t *
mc_new_user_mc ()
{
  if (unlikely (MC_STATE == MC_UNINT || !mc_scheduler))
    abort ();
  minicoru_t *t = mc_new ();
  t->ctx.uc_link = &mc_scheduler->ctx;
  return t;
}

void
mc_resume (minicoru_t *next)
{
  mc_current_mc = next;
  next->state = MC_STATE_RUNNING;
  LOG_DEBUG ("resuming #%u\n", mc_current_mc->id);
  setcontext (&next->ctx);
  abort (); // cannot reach here
}

int mc_uring_cq_read ();

void
scheduler ()
{
  mc_scheduler->state = MC_STATE_RUNNING;
  if (likely (mc_current_mc && mc_current_mc->state == MC_STATE_FIN))
    {
#ifdef DEBUG
      ++mc_stat_n_coru_freed;
      LOG_DEBUG (PURPLE "#%d fin: - %u/%u freed/created %d suspending\n" RESET,
                 mc_current_mc->id, mc_stat_n_coru_freed,
                 mc_stat_n_coru_created, mc_n_suspending);
#endif
      if (mc_current_mc->notify.coru)
        mc_arrange (mc_current_mc->notify.coru);
      if (mc_current_mc->notify.with_msg)
        *mc_current_mc->notify.with_msg = mc_current_mc->result;
      if (unlikely (mc_current_mc->type == MC_TYPE_TIMER))
        timer_delete (mc_current_mc->timer); // no need to check return value
      free (mc_current_mc);
      mc_current_mc = NULL;
    }

  if (likely (mc_n_suspending > 0))
    {
    resched:
      mc_uring_cq_read ();
    }

  minicoru_t *next = q_pop (mc_waiters);

  assert (!next || next != mc_scheduler, "schudler is scheduled");
  assert (!next || next->state == MC_STATE_WAITING, "Illegal mc state");

  if (likely (!next && mc_n_suspending > 0))
    {
#ifdef DEBUG
      // LOG_DEBUG("[scheduler] spin\n");
#endif
      goto resched;
    }

  if (unlikely (!next && mc_n_suspending == 0))
    {
      mc_scheduler->state = MC_STATE_FIN;
#ifdef DEBUG
      LOG_DEBUG ("exiting\n");
      ++mc_stat_n_coru_freed;
      LOG_DEBUG (PURPLE "scheduler fin: - %u/%u freed/created\n" RESET,
                 mc_stat_n_coru_freed, mc_stat_n_coru_created);
#endif
      free (mc_scheduler);
      return;
    }

  mc_scheduler->state = MC_STATE_WAITING;
  mc_resume (next);
  // UNREACHABLE
  abort ();
}

void
mc_arrange (minicoru_t *t)
{
  if (t->state == MC_STATE_SUSPENDING)
    {
      --mc_n_suspending;
      t->state = MC_STATE_WAITING;
      q_push (mc_waiters, t);
      LOG_DEBUG ("arraged #%u\n", t->id);
      return;
    }
  LOG_DEBUG ("#%u has already been waitting\n", t->id);
}

inline __attribute__ ((always_inline)) void
mc_schedule ()
{
  swapcontext (&mc_current_mc->ctx, &mc_scheduler->ctx);
}

void *
mc_await (minicoru_t *t)
{
#ifdef DEBUG
  LOG_DEBUG ("#%u awaiting #%u\n", mc_current_mc->id, t->id);
#endif
  void *slot = NULL;
  t->notify.coru = mc_current_mc;
  t->notify.with_msg = &slot;
  if (likely (t->type != MC_TYPE_TIMER))
    mc_arrange (t);
  mc_suspend (mc_current_mc);
  mc_schedule ();
  return slot;
}

#include <linux/io_uring.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

int ring_fd;
unsigned *sring_tail, *sring_mask, *sring_array, *cring_head, *cring_tail,
    *cring_mask;
struct io_uring_sqe *sqes;
struct io_uring_cqe *cqes;

int
io_uring_setup (unsigned entries, struct io_uring_params *p)
{
  return (int)syscall (__NR_io_uring_setup, entries, p);
}

int
io_uring_enter (int ring_fd, unsigned int to_submit, unsigned int min_complete,
                unsigned int flags)
{
  return (int)syscall (__NR_io_uring_enter, ring_fd, to_submit, min_complete,
                       flags, NULL, 0);
}

/* Macros for barriers needed by io_uring */
#define io_uring_smp_store_release(p, v)                                      \
  atomic_store_explicit ((_Atomic typeof (*(p)) *)(p), (v),                   \
                         memory_order_release)
#define io_uring_smp_load_acquire(p)                                          \
  atomic_load_explicit ((_Atomic typeof (*(p)) *)(p), memory_order_acquire)

void
mc_setup_uring ()
{
  struct io_uring_params p;
  void *sq, *cq;
  memset (&p, 0, sizeof (p));
  ring_fd = io_uring_setup (MC_IOURING_QUEUE_DEPTH, &p);
  if (unlikely (ring_fd < 0))
    abort ();
  int sring_sz = p.sq_off.array + p.sq_entries * sizeof (unsigned);
  int cring_sz = p.cq_off.cqes + p.cq_entries * sizeof (struct io_uring_cqe);
  if (p.features & IORING_FEAT_SINGLE_MMAP)
    {
      if (cring_sz > sring_sz)
        sring_sz = cring_sz;
      cring_sz = sring_sz;
    }
  sq = mmap (0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
             ring_fd, IORING_OFF_SQ_RING);
  if (unlikely (sq == MAP_FAILED))
    abort ();
  if (p.features & IORING_FEAT_SINGLE_MMAP)
    {
      cq = sq;
    }
  else
    {
      /* Map in the completion queue ring buffer in older kernels separately */
      cq = mmap (0, cring_sz, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_CQ_RING);
      if (unlikely (cq == MAP_FAILED))
        abort ();
    }

  sring_tail = sq + p.sq_off.tail;
  sring_mask = sq + p.sq_off.ring_mask;
  sring_array = sq + p.sq_off.array;

  sqes = mmap (0, p.sq_entries * sizeof (struct io_uring_sqe),
               PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd,
               IORING_OFF_SQES);
  if (unlikely (sqes == MAP_FAILED))
    abort ();

  /* Save useful fields for later easy reference */
  cring_head = cq + p.cq_off.head;
  cring_tail = cq + p.cq_off.tail;
  cring_mask = cq + p.cq_off.ring_mask;
  cqes = cq + p.cq_off.cqes;
  return;
}

int
mc_uring_cq_read ()
{
  struct io_uring_cqe *cqe;
  unsigned head; //, reaped = 0;

  /* Read barrier */
  head = io_uring_smp_load_acquire (cring_head);
  /*
   * Remember, this is a ring buffer. If head == tail, it means that the
   * buffer is empty.
   * */
  if (head == *cring_tail)
    return -1;

  /* Get the entry */
  cqe = &cqes[head & (*cring_mask)];
  minicoru_t *mc = (minicoru_t *)cqe->user_data;
  if (cqe->res < 0)
    LOG_DEBUG (BLUE "Uring Error: %s\n" RESET, strerror (abs (cqe->res)));

  assert (mc && mc->state == MC_STATE_SUSPENDING,
          "Illegal state: coru must be suspending");
  mc->result = (void *)(long)cqe->res;
  mc_arrange (mc);
  head++;

  /* Write barrier so that update to the head are made visible */
  io_uring_smp_store_release (cring_head, head);

  return 0;
}

struct io_uring_sqe *
mc_uring_make_sqe_slot ()
{
  unsigned index, tail;
  tail = *sring_tail;
  index = tail & *sring_mask;
  sring_array[index] = index;
  tail++;
  return &sqes[index];
}

#define mc_uring_submit_new_sqe(block)                                        \
  do                                                                          \
    {                                                                         \
      unsigned index, tail;                                                   \
      tail = *sring_tail;                                                     \
      index = tail & *sring_mask;                                             \
      sring_array[index] = index;                                             \
      tail++;                                                                 \
      struct io_uring_sqe *sqe = &sqes[index];                                \
      sqe->user_data = (uint64_t)mc_current_mc;                               \
                                                                              \
      (block);                                                                \
                                                                              \
      io_uring_smp_store_release (sring_tail, tail);                          \
      if (unlikely (io_uring_enter (ring_fd, 1, 0, 0) < 0))                   \
        {                                                                     \
          LOG_DEBUG (RED "failed to submit sqe\n" RESET);                     \
          mc_return (-1);                                                     \
        }                                                                     \
      mc_suspend (mc_current_mc);                                             \
      mc_schedule ();                                                         \
      mc_current_mc->state = MC_STATE_FIN;                                    \
      mc_notify (mc_current_mc);                                              \
    }                                                                         \
  while (0)

int
mc_write (int fd, void const *buf, size_t len)
{
  mc_uring_submit_new_sqe (({
    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = (size_t)buf;
    sqe->len = len;
    sqe->off = 0;
  }));
  return 0;
}

int
mc_read (int fd, void const *buf, size_t len)
{
  mc_uring_submit_new_sqe (({
    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = (size_t)buf;
    sqe->len = len;
    sqe->off = 0;
  }));
  return 0;
}

void
mc_timed_wait ()
{
  if (unlikely (mc_current_mc->notify.coru == NULL))
    {
      // this should not happen
      LOG_DEBUG ("timed wait mc is early scheduled");
      abort ();
    }
  mc_fin (mc_current_mc, 0);
}
minicoru_t *
mc_async_timed_wait (struct timespec oneshot)
{
  minicoru_t *coru = mc_new_user_mc ();
  makecontext (&coru->ctx, mc_timed_wait, 0);
  coru->type = MC_TYPE_TIMER;

  struct sigevent sev;
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = MC_TIMER_SIG;
  sev.sigev_value.sival_ptr = coru;
  if (unlikely (timer_create (MC_TIMER_CLOCK_ID, &sev, &coru->timer) == -1))
    {
      LOG_DEBUG ("failed to create timer");
      return NULL;
    }
  struct itimerspec its;
  its.it_value = oneshot;
  its.it_interval = ((struct timespec){ 0 });

  sigset_t sig_mask;
  sigemptyset (&sig_mask);
  sigaddset (&sig_mask, MC_TIMER_SIG);
  if (unlikely (sigprocmask (SIG_SETMASK, &sig_mask, NULL) == -1))
    {
      LOG_DEBUG ("failed to block signals");
      timer_delete (coru->timer);
      free (coru);
      return NULL;
    }

  if ((timer_settime (coru->timer, 0, &its, NULL)) == -1)
    {
      LOG_DEBUG ("failed to arm timer");
      timer_delete (coru->timer);
      free (coru);
      return NULL;
    }

  if (unlikely (sigprocmask (SIG_UNBLOCK, &sig_mask, NULL) == -1))
    {
      LOG_DEBUG ("failed to unblock signals");
      timer_delete (coru->timer);
      free (coru);
      return NULL;
    }
  return coru;
}

void
mc_timer_signal_handler (int sig, siginfo_t *si, void *uc)
{
  sigset_t sig_mask;
  sigemptyset (&sig_mask);
  sigaddset (&sig_mask, MC_TIMER_SIG);
  if (unlikely (sigprocmask (SIG_SETMASK, &sig_mask, NULL) == -1))
    {
      LOG_DEBUG ("failed to block signals");
      abort ();
    }

  minicoru_t *coru = si->si_value.sival_ptr;
  LOG_DEBUG ("timer signal arrived for #%u\n", coru->id);
  mc_arrange (coru);

  if (unlikely (sigprocmask (SIG_UNBLOCK, &sig_mask, NULL) == -1))
    {
      LOG_DEBUG ("failed to unblock signals");
      abort ();
    }
}

void
mc_setup_timer_signal_handler ()
{
  struct sigaction sa;
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = mc_timer_signal_handler;
  sigemptyset (&sa.sa_mask);

  if (sigaction (MC_TIMER_SIG, &sa, NULL) == -1)
    {
      LOG_DEBUG ("failed to set signal handler");
      abort ();
    }
}

void
mc_init ()
{
  mc_waiters = q_new (MC_MAX_N_MC);

  mc_scheduler = mc_new ();
  mc_scheduler->type = MC_TYPE_MANAGER;

  mc_scheduler->state = MC_STATE_WAITING;
  --mc_n_suspending; // we don't schedule schedular, so it is exempted

  mc_setup_uring ();
  mc_setup_timer_signal_handler ();
  MC_STATE = MC_READY;
}

void
mc_run ()
{
  if (MC_STATE != MC_READY)
    abort ();

  ucontext_t orig;
  if (getcontext (&orig) < 0)
    {
#ifdef DEBUG
      perror ("getcontext");
#endif
      abort ();
    }
  mc_scheduler->ctx.uc_link = &orig;
  makecontext (&mc_scheduler->ctx, (void (*) (void))scheduler, 0);
  MC_STATE = MC_RUNNING;
  swapcontext (&orig, &mc_scheduler->ctx);
#ifdef DEBUG
  if (mc_stat_n_coru_created != mc_stat_n_coru_freed)
    {
      LOG_DEBUG (RED "POSSIBLE LEAK DETECTED: created %d but freed %d\n" RESET,
                 mc_stat_n_coru_created, mc_stat_n_coru_freed);
    }
#endif
}
