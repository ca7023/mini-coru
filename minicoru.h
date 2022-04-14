#include <stdlib.h>
#include <ucontext.h>
#define mc_yield()                                                             \
  do {                                                                         \
    mc_wait(mc_current_mc);                                                    \
    swapcontext(&mc_current_mc->ctx, &mc_scheduler->ctx);                      \
  } while (0)

#define mc_return(v)                            \
  do {                                          \
    mc_fin(mc_current_mc, (void *)(long) v);    \
    return v;                                   \
  } while (0)

#define mc_async(func, argv...)                                                \
  ((minicoru_t *)({                                                            \
    minicoru_t *c = mc_new_user_mc();                                          \
    makecontext(&c->ctx, (void (*)(void))func, VA_NARGS(argv), ##argv);        \
    c;                                                                         \
  }))

#define VA_NARGS_IMPL(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, N, ...) N
#define VA_NARGS(...)                                                          \
  VA_NARGS_IMPL(_, ##__VA_ARGS__, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#define MC_STKSIZ 16 << 10
#define MC_IOURING_QUEUE_DEPTH 32

typedef struct minicoru_t {
  enum { MC_TYPE_MANAGER, MC_TYPE_WORKER } type;
  unsigned int id;
  ucontext_t ctx;
  char stack[MC_STKSIZ];
  void (*fn)(void);
  enum {
    MC_STATE_WAITING,
    MC_STATE_RUNNING,
    MC_STATE_SUSPENDING,
    MC_STATE_FIN
  } state;
  struct {
    struct minicoru_t *coru; // to who
    void **with_msg;         // with what
  } notify;
  void *result;
} minicoru_t;

enum mc_core_state { MC_UNINT, MC_READY, MC_RUNNING };
extern enum mc_core_state MC_STATE;
extern minicoru_t *mc_current_mc;
extern minicoru_t *mc_scheduler;

void mc_wait(minicoru_t *mc);
void mc_fin(minicoru_t *mc, void *ret);

void mc_arrange(minicoru_t *t);
void *mc_await(minicoru_t *t);

minicoru_t *mc_new_user_mc();

void mc_init();
void mc_run();


int mc_read(int fd, void const *buf, size_t len);
int mc_write(int fd, void const *buf, size_t len);
