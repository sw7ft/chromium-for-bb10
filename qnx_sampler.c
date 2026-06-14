/* QNX /proc thread sampler: reads each thread's instruction pointer and CPU
 * time directly from procfs, no signals needed (works on threads that block
 * signals). Usage: qnx_sampler <pid> <iterations> <usec_between>
 * Prints "tid state ip sutime_ns" for RUNNING/READY threads each iteration. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/procfs.h>
#include <sys/dcmd_proc.h>

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s pid [iters] [usec]\n", argv[0]); return 1; }
  int iters = (argc > 2) ? atoi(argv[2]) : 300;
  int usec  = (argc > 3) ? atoi(argv[3]) : 10000;
  char path[64];
  snprintf(path, sizeof(path), "/proc/%s/as", argv[1]);
  int fd = open(path, O_RDONLY);
  if (fd < 0) { perror("open"); return 1; }

  for (int it = 0; it < iters; it++) {
    for (int tid = 1; tid < 80; tid++) {
      procfs_status st;
      memset(&st, 0, sizeof(st));
      st.tid = tid;
      if (devctl(fd, DCMD_PROC_TIDSTATUS, &st, sizeof(st), 0) != EOK) continue;
      if (st.tid != (pthread_t)tid) continue;
      /* Only report on-CPU or runnable threads (state 1=RUNNING, 2=READY). */
      if (st.state == 1 || st.state == 2) {
        printf("S tid=%d state=%d ip=0x%llx sutime=%llu\n",
               tid, st.state, (unsigned long long)st.ip,
               (unsigned long long)st.sutime);
      }
    }
    usleep(usec);
  }
  close(fd);
  return 0;
}
