#include <stdio.h>
#include "../minicoru.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int get_remote_message(int i) {
  puts("waiting...");

  mc_yield();

  printf("get message %d\n", i);

  mc_return(i);
}

char buf[1 << 9];
int int_scanner(char * const content, int len) {
  long ret = (long)mc_await(mc_async(mc_read, STDIN_FILENO, content, len));

  if (ret < 0) {
    printf("Error: %s\n", strerror((-ret)));
    mc_return(-1);
  }

  mc_return(atoi(buf));
}

int remote_add() {
  printf("remote_add start\n");
  int a = (int)(long)mc_await(mc_async(get_remote_message, 7));
  int b = (int)(long)mc_await(mc_async(int_scanner, buf, sizeof(buf)));
  printf("answer is %d\n", a + b);
  mc_return(0);
}

int printer(int counter, char const *const content) {
  int len = strlen(content);
  for (int i = 1; i <= counter; ++i) {
    long ret;
    ret = (long)mc_await(mc_async(mc_write, STDOUT_FILENO, content, len));
    if (ret < 0) printf("Error: %s\n", strerror((-ret)));
  }
  mc_return(0);
}

int main() {
  mc_init();
  mc_arrange(mc_async(remote_add));
  mc_arrange(mc_async(printer, 9, "Belle Miss Qiao\n"));
  mc_run();
  return 0;
}
