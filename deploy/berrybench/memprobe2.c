/* Does POSIX_MADV_DONTNEED give memory back on BB10? Holds 256 MB, pauses
 * so free memory can be read from outside, discards, pauses again. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
int main(void) {
  const size_t n = 256u << 20;
  char* p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return 1;
  printf("mapped\n"); fflush(stdout); sleep(2);
  memset(p, 7, n);
  printf("touched\n"); fflush(stdout); sleep(2);
  posix_madvise(p, n, POSIX_MADV_DONTNEED);
  printf("discarded\n"); fflush(stdout); sleep(2);
  mmap(p, n, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
  printf("remapped\n"); fflush(stdout); sleep(2);
  size_t nz = 0;
  for (size_t k = 0; k < n; k += 4096) nz += p[k] != 0;
  printf("after remap: %zu non-zero pages\n", nz); fflush(stdout); sleep(2);
  return 0;
}
