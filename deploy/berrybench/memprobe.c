/* Cost of memory-management calls on BB10/QNX, and what POSIX_MADV_DONTNEED
 * does to page contents. Touches only its own anonymous memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>

static double now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(void) {
  const size_t kTotal = 64u << 20;
  char* base = mmap(0, kTotal, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (base == MAP_FAILED) { perror("mmap"); return 1; }
  double t = now_us();
  memset(base, 0x5a, kTotal);
  printf("first touch of 64 MB: %.0f us (%.1f us per 4K page)\n", now_us() - t,
         (now_us() - t) / (kTotal / 4096));
  t = now_us();
  memset(base, 0x5b, kTotal);
  printf("second touch of 64 MB: %.0f us\n", now_us() - t);

  size_t sizes[] = {4096, 16384, 65536, 262144, 1048576, 4194304, 16777216};
  size_t off = 0;
  for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    size_t n = sizes[i];
    char* p = base + off;
    off += n;
    t = now_us();
    int rc = posix_madvise(p, n, POSIX_MADV_DONTNEED);
    double adv = now_us() - t;
    size_t zero = 0, same = 0;
    t = now_us();
    for (size_t k = 0; k < n; k += 4096) {
      if (p[k] == 0) zero++; else if (p[k] == 0x5b) same++;
    }
    double rd = now_us() - t;
    t = now_us();
    memset(p, 1, n);
    double wr = now_us() - t;
    printf("madvise(DONTNEED) %8zu bytes: rc=%d %8.0f us | after: %zu pages zero, %zu unchanged | "
           "read back %6.0f us, rewrite %6.0f us\n", n, rc, adv, zero, same, rd, wr);
  }

  /* Repeat a 64K discard on already-discarded memory and on a hot range. */
  char* q = base + (40u << 20);
  for (int r = 0; r < 3; r++) {
    t = now_us();
    posix_madvise(q, 65536, POSIX_MADV_DONTNEED);
    printf("madvise 64K again (round %d): %.0f us\n", r, now_us() - t);
  }

  /* mprotect and mmap/munmap round trips. */
  char* r = base + (48u << 20);
  t = now_us();
  for (int k = 0; k < 200; k++) {
    mprotect(r, 65536, PROT_READ);
    mprotect(r, 65536, PROT_READ | PROT_WRITE);
  }
  printf("mprotect 64K: %.0f us per call\n", (now_us() - t) / 400);
  t = now_us();
  for (int k = 0; k < 200; k++) {
    char* m = mmap(0, 262144, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    munmap(m, 262144);
  }
  printf("mmap+munmap 256K (untouched): %.0f us per pair\n", (now_us() - t) / 200);
  t = now_us();
  for (int k = 0; k < 50; k++) {
    char* m = mmap(0, 262144, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    memset(m, 1, 262144);
    munmap(m, 262144);
  }
  printf("mmap+touch+munmap 256K: %.0f us per round\n", (now_us() - t) / 50);
  /* Decommit the way V8's own platform code does it: remap in place. */
  t = now_us();
  for (int k = 0; k < 50; k++) {
    mmap(r, 262144, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    mprotect(r, 262144, PROT_READ | PROT_WRITE);
    memset(r, 1, 262144);
  }
  printf("remap-in-place+mprotect+touch 256K: %.0f us per round\n", (now_us() - t) / 50);
  /* How much of that is the page faults alone? */
  t = now_us();
  for (int k = 0; k < 50; k++) memset(r, 2, 262144);
  printf("memset 256K (warm): %.0f us per round\n", (now_us() - t) / 50);
  return 0;
}
