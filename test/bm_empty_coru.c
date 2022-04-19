#include "../minicoru.h"
#include <time.h>
#include <x86intrin.h>
#include <stdio.h>

int dumb() {
  mc_return(0);
}

int main() {
  mc_init();
  unsigned int cpuid;
  unsigned long long t0 = __rdtscp(&cpuid);
  const int ncoru = 1000;
  for (int i = 0; i < ncoru; ++i)
    {
      mc_arrange(mc_async(dumb));
    }
  mc_run();
  unsigned long long t1 = __rdtscp(&cpuid);
  printf("%d: %llu\n", ncoru, t1 - t0);
  return 0;
}
