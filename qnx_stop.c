/* QNX: STOP the process so thread registers are current, then dump the real
 * PC/LR/SP and a stack scan for the given tid. Usage: qnx_stop <pid> <tid> */
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
  int fd = open(path, O_RDWR);
  if (fd < 0) { perror("open"); return 1; }

  procfs_status pstop;
  memset(&pstop, 0, sizeof(pstop));
  if (devctl(fd, DCMD_PROC_STOP, &pstop, sizeof(pstop), 0) != EOK)
    perror("stop(continuing anyway)");

  if (devctl(fd, DCMD_PROC_CURTHREAD, &tid, sizeof(tid), 0) != EOK) {
    perror("curthread"); return 1;
  }
  procfs_greg greg;
  memset(&greg, 0, sizeof(greg));
  int nb = 0;
  if (devctl(fd, DCMD_PROC_GETGREG, &greg, sizeof(greg), &nb) != EOK) {
    perror("getgreg"); return 1;
  }
  unsigned sp = greg.arm.gpr[13];
  unsigned lr = greg.arm.gpr[14];
  unsigned pc = greg.arm.gpr[15];
  printf("REAL tid=%d pc=0x%x lr=0x%x sp=0x%x fp=0x%x\n",
         tid, pc, lr, sp, greg.arm.gpr[11]);

  /* Stack scan for code-like return addresses (now accurate, thread stopped) */
  printf("STACK:\n");
  for (unsigned off = 0; off < 2048; off += 4) {
    unsigned val = 0;
    if (pread(fd, &val, 4, (off_t)(sp + off)) != 4) break;
    if (val > 0x00800000u && val < 0x07000000u) {
      printf("  +0x%x: 0x%x\n", off, val & ~1u);
    }
  }
  close(fd);
  return 0;
}
