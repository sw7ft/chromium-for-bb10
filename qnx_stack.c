/* QNX /proc thread stack walker. Usage: qnx_stack <pid> <tid>
 * Dumps the thread's registers (esp. LR) and scans its stack for code-like
 * return addresses, to reconstruct a backtrace (symbolize offline). */
#define _DEBUG_TARGET_ARM 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/procfs.h>
#include <sys/dcmd_proc.h>

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s pid tid\n", argv[0]); return 1; }
  int tid = atoi(argv[2]);
  char path[64];
  snprintf(path, sizeof(path), "/proc/%s/as", argv[1]);
  int fd = open(path, O_RDONLY);
  if (fd < 0) { perror("open"); return 1; }

  if (devctl(fd, DCMD_PROC_CURTHREAD, &tid, sizeof(tid), 0) != EOK) {
    perror("curthread"); return 1;
  }
  procfs_greg greg;
  memset(&greg, 0, sizeof(greg));
  int nbytes = 0;
  if (devctl(fd, DCMD_PROC_GETGREG, &greg, sizeof(greg), &nbytes) != EOK) {
    perror("getgreg"); return 1;
  }
  unsigned sp = greg.arm.gpr[13];
  unsigned lr = greg.arm.gpr[14];
  unsigned pc = greg.arm.gpr[15];
  printf("tid=%d pc=0x%x lr=0x%x sp=0x%x\n", tid, pc, lr, sp);
  for (int r = 0; r < 16; r++) printf("r%d=0x%x\n", r, greg.arm.gpr[r]);

  /* Scan up to 4KB of stack for code-like addresses (main .text range). */
  printf("STACK SCAN:\n");
  for (unsigned off = 0; off < 4096; off += 4) {
    unsigned val = 0;
    if (pread(fd, &val, 4, (off_t)(sp + off)) != 4) break;
    /* content_shell .text roughly 0x008xxxxx .. 0x06xxxxxx */
    if (val > 0x00800000u && val < 0x06000000u) {
      printf("  +0x%x: 0x%x\n", off, val & ~1u);
    }
  }
  close(fd);
  return 0;
}
