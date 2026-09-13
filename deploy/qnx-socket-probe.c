// QNX AF_UNIX SOCK_STREAM probe for the Berry MP Mojo channel bug.
//
// Mojo's ChannelPosix assumes Linux semantics on a socketpair(AF_UNIX,
// SOCK_STREAM, 0):
//   1. recvmsg() stops at an ancillary-data (SCM_RIGHTS) boundary, so the fds
//      it returns belong to the bytes it just returned.
//   2. sendmsg() with SCM_RIGHTS may write fewer bytes than requested, but the
//      fds always go with the first byte.
//   3. MSG_CTRUNC is never set when the control buffer is large enough.
// If QNX breaks any of those, the byte stream desyncs and the receiver reads a
// message header out of the middle of a payload -- which is what we see as
// "Invalid message size" and absurd num_bytes values.
//
// Build: /root/qnx800/bin/arm-blackberry-qnx8eabi-gcc -O2 \
//          -o qnx-socket-probe qnx-socket-probe.c -lsocket -lc
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Matches mojo::kMaxSendmsgHandles.
#define MAX_HANDLES 128
#define CMSG_BUF_SIZE CMSG_SPACE(MAX_HANDLES * sizeof(int))

static int g_fail = 0;

static void ok(const char* what) { printf("  PASS  %s\n", what); }
static void bad(const char* what, const char* detail) {
  printf("  FAIL  %s -- %s\n", what, detail);
  g_fail++;
}

static int make_fd(void) {
  int fd = open("/dev/null", O_RDONLY);
  if (fd < 0) fd = dup(0);
  return fd;
}

// ---------------------------------------------------------------------------
// Faithful copies of the two functions Mojo actually uses.
// ---------------------------------------------------------------------------

static ssize_t probe_sendmsg_with_handles(int sock, struct iovec* iov,
                                          size_t num_iov, const int* fds,
                                          size_t num_fds) {
  char cmsg_buf[CMSG_BUF_SIZE];
  struct msghdr msg;
  struct cmsghdr* cmsg;
  size_t i;

  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = num_iov;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = CMSG_LEN(num_fds * sizeof(int));
  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(num_fds * sizeof(int));
  for (i = 0; i < num_fds; i++)
    ((int*)CMSG_DATA(cmsg))[i] = fds[i];

  return sendmsg(sock, &msg, MSG_NOSIGNAL);
}

// Returns bytes read; *out_nfds gets the fd count, *out_ctrunc gets MSG_CTRUNC.
static ssize_t probe_recvmsg(int sock, void* buf, size_t num_bytes,
                             int* out_fds, size_t* out_nfds, int* out_ctrunc) {
  char cmsg_buf[CMSG_BUF_SIZE];
  struct iovec iov;
  struct msghdr msg;
  struct cmsghdr* cmsg;
  ssize_t result;

  iov.iov_base = buf;
  iov.iov_len = num_bytes;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = sizeof(cmsg_buf);

  *out_nfds = 0;
  *out_ctrunc = 0;
  result = recvmsg(sock, &msg, 0);
  if (result < 0) return result;

  *out_ctrunc = (msg.msg_flags & MSG_CTRUNC) ? 1 : 0;
  if (msg.msg_controllen == 0) return result;

  for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      size_t payload = cmsg->cmsg_len - CMSG_LEN(0);
      size_t n = payload / sizeof(int);
      size_t i;
      for (i = 0; i < n; i++)
        out_fds[(*out_nfds)++] = ((int*)CMSG_DATA(cmsg))[i];
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Test A: environment
// ---------------------------------------------------------------------------

static void test_env(int sock) {
  int sndbuf = 0, rcvbuf = 0;
  socklen_t len = sizeof(int);
  long iov_max_sc = sysconf(_SC_IOV_MAX);

  printf("\n== A: environment ==\n");
  if (getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len) == 0)
    printf("  SO_SNDBUF        = %d\n", sndbuf);
  else
    printf("  SO_SNDBUF        = <getsockopt errno %d>\n", errno);
  len = sizeof(int);
  if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len) == 0)
    printf("  SO_RCVBUF        = %d\n", rcvbuf);
  else
    printf("  SO_RCVBUF        = <getsockopt errno %d>\n", errno);
#ifdef IOV_MAX
  printf("  IOV_MAX (header) = %d\n", (int)IOV_MAX);
#else
  printf("  IOV_MAX (header) = <undefined; channel_posix.cc assumes 1024>\n");
#endif
  printf("  _SC_IOV_MAX      = %ld\n", iov_max_sc);
  printf("  MSG_NOSIGNAL     = %d\n", (int)MSG_NOSIGNAL);
  printf("  CMSG_SPACE(128fd)= %d\n", (int)CMSG_BUF_SIZE);

  // Mojo hardcodes 1024 iovecs for QNX. If the real limit is lower, the
  // writev flush path fails exactly on big multi-message batches.
  if (iov_max_sc > 0 && iov_max_sc < 1024)
    printf("  NOTE: _SC_IOV_MAX (%ld) < the 1024 channel_posix.cc assumes.\n",
           iov_max_sc);
}

// ---------------------------------------------------------------------------
// Test B: does recvmsg stop at the SCM_RIGHTS boundary?
// This is the single most important question. Mojo relies on it.
// ---------------------------------------------------------------------------

static void test_ancillary_boundary(void) {
  int sv[2];
  struct iovec iov;
  char buf[4096];
  int fds[MAX_HANDLES];
  size_t nfds;
  int ctrunc;
  ssize_t n;
  int f1, f2;
  char detail[256];

  printf("\n== B: recvmsg ancillary-data boundary ==\n");
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    bad("socketpair", strerror(errno));
    return;
  }
  f1 = make_fd();
  f2 = make_fd();

  // Three writes: 4B+fd, 4B+fd, 8B plain. Linux gives the reader 4 bytes and
  // 1 fd on the first recvmsg, because it stops at the boundary.
  iov.iov_base = (void*)"AAAA";
  iov.iov_len = 4;
  if (probe_sendmsg_with_handles(sv[0], &iov, 1, &f1, 1) != 4) {
    bad("sendmsg #1", strerror(errno));
    goto done;
  }
  iov.iov_base = (void*)"BBBB";
  iov.iov_len = 4;
  if (probe_sendmsg_with_handles(sv[0], &iov, 1, &f2, 1) != 4) {
    bad("sendmsg #2", strerror(errno));
    goto done;
  }
  if (send(sv[0], "CCCCCCCC", 8, MSG_NOSIGNAL) != 8) {
    bad("send #3", strerror(errno));
    goto done;
  }

  n = probe_recvmsg(sv[1], buf, sizeof(buf), fds, &nfds, &ctrunc);
  if (n < 0) {
    bad("recvmsg", strerror(errno));
    goto done;
  }
  printf("  first recvmsg: bytes=%ld fds=%u ctrunc=%d data=%.*s\n", (long)n,
         (unsigned)nfds, ctrunc, (int)(n > 16 ? 16 : n), buf);

  if (ctrunc)
    bad("MSG_CTRUNC set",
        "fds were silently dropped; Mojo only DCHECKs this, so release "
        "builds lose handles without any error");

  if (n == 4 && nfds == 1) {
    ok("recvmsg stops at the SCM_RIGHTS boundary (Linux semantics)");
  } else if (n > 4) {
    snprintf(detail, sizeof(detail),
             "coalesced %ld bytes across the boundary and returned %u fds "
             "(expected 4 bytes / 1 fd). Mojo's handle-to-message mapping "
             "cannot survive this.",
             (long)n, (unsigned)nfds);
    bad("recvmsg boundary preservation", detail);
  } else {
    snprintf(detail, sizeof(detail), "bytes=%ld fds=%u", (long)n,
             (unsigned)nfds);
    bad("recvmsg boundary preservation", detail);
  }

  while (nfds > 0) close(fds[--nfds]);
done:
  close(f1);
  close(f2);
  close(sv[0]);
  close(sv[1]);
}

// ---------------------------------------------------------------------------
// Test C/D: large-message integrity, then small messages behind it.
// Mimics ChannelPosix::WriteNoLock (fd on the first sendmsg, then plain
// send() for the remainder, handling partial writes) against a reader that
// mimics OnFileCanReadWithoutBlocking (4096-byte recvmsg chunks).
// ---------------------------------------------------------------------------

#define BIG_SIZE (512 * 1024)
#define NUM_SMALL 24
#define SMALL_SIZE 64
#define TOTAL_SIZE (BIG_SIZE + NUM_SMALL * SMALL_SIZE)

static unsigned char pattern_byte(size_t i) {
  return (unsigned char)((i * 31u + 7u) & 0xff);
}

struct writer_args {
  int sock;
  ssize_t first_send_result;
  size_t first_send_requested;
  int short_write_seen;
  int error;
};

static void* writer_thread(void* p) {
  struct writer_args* a = (struct writer_args*)p;
  unsigned char* payload = (unsigned char*)malloc(TOTAL_SIZE);
  size_t sent = 0;
  size_t i;
  int fd = make_fd();
  struct iovec iov;
  ssize_t n;

  for (i = 0; i < TOTAL_SIZE; i++) payload[i] = pattern_byte(i);

  // Big message: one sendmsg carrying the fd, then send() the remainder.
  iov.iov_base = payload;
  iov.iov_len = BIG_SIZE;
  a->first_send_requested = BIG_SIZE;
  n = probe_sendmsg_with_handles(a->sock, &iov, 1, &fd, 1);
  a->first_send_result = n;
  if (n < 0) {
    a->error = errno;
    goto out;
  }
  sent = (size_t)n;
  if (sent < BIG_SIZE) a->short_write_seen = 1;
  while (sent < BIG_SIZE) {
    n = send(a->sock, payload + sent, BIG_SIZE - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      a->error = errno;
      goto out;
    }
    sent += (size_t)n;
  }

  // Then a burst of small fd-bearing messages -- the "Invalid message size: 0"
  // scenario, where the stream is already broken behind the big write.
  for (i = 0; i < NUM_SMALL; i++) {
    int sfd = make_fd();
    size_t off = BIG_SIZE + i * SMALL_SIZE;
    size_t left = SMALL_SIZE;
    iov.iov_base = payload + off;
    iov.iov_len = SMALL_SIZE;
    n = probe_sendmsg_with_handles(a->sock, &iov, 1, &sfd, 1);
    if (n < 0) {
      a->error = errno;
      close(sfd);
      goto out;
    }
    left -= (size_t)n;
    while (left > 0) {
      n = send(a->sock, payload + off + (SMALL_SIZE - left), left,
               MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR) continue;
        a->error = errno;
        close(sfd);
        goto out;
      }
      left -= (size_t)n;
    }
    close(sfd);
  }

out:
  close(fd);
  free(payload);
  shutdown(a->sock, SHUT_WR);
  return NULL;
}

static void test_large_message(void) {
  int sv[2];
  pthread_t tid;
  struct writer_args wa;
  unsigned char* got = (unsigned char*)malloc(TOTAL_SIZE + 8192);
  size_t total = 0;
  size_t total_fds = 0;
  size_t reads = 0;
  size_t ctrunc_count = 0;
  size_t first_bad = (size_t)-1;
  size_t i;
  char detail[256];

  printf("\n== C: %d-byte message + %d small fd messages ==\n", BIG_SIZE,
         NUM_SMALL);
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    bad("socketpair", strerror(errno));
    free(got);
    return;
  }

  memset(&wa, 0, sizeof(wa));
  wa.sock = sv[0];
  if (pthread_create(&tid, NULL, writer_thread, &wa) != 0) {
    bad("pthread_create", strerror(errno));
    free(got);
    close(sv[0]);
    close(sv[1]);
    return;
  }

  // Reader mimics Mojo: fixed 4096-byte recvmsg chunks (Channel::
  // kReadBufferSize), fds accumulated in arrival order.
  for (;;) {
    int fds[MAX_HANDLES];
    size_t nfds;
    int ct;
    ssize_t n = probe_recvmsg(sv[1], got + total, 4096, fds, &nfds, &ct);
    if (n < 0) {
      if (errno == EINTR) continue;
      snprintf(detail, sizeof(detail), "recvmsg after %lu bytes: %s",
               (unsigned long)total, strerror(errno));
      bad("read loop", detail);
      break;
    }
    if (n == 0) break;
    reads++;
    total += (size_t)n;
    total_fds += nfds;
    if (ct) ctrunc_count++;
    while (nfds > 0) close(fds[--nfds]);
    if (total >= TOTAL_SIZE) break;
  }
  pthread_join(tid, NULL);

  printf("  writer: first sendmsg requested=%lu returned=%ld%s errno=%d\n",
         (unsigned long)wa.first_send_requested, (long)wa.first_send_result,
         wa.short_write_seen ? " (SHORT -- partial write path exercised)" : "",
         wa.error);
  printf("  reader: %lu recvmsg calls, %lu bytes, %lu fds, %lu MSG_CTRUNC\n",
         (unsigned long)reads, (unsigned long)total, (unsigned long)total_fds,
         (unsigned long)ctrunc_count);

  for (i = 0; i < total && i < TOTAL_SIZE; i++) {
    if (got[i] != pattern_byte(i)) {
      first_bad = i;
      break;
    }
  }

  if (total != TOTAL_SIZE) {
    snprintf(detail, sizeof(detail), "expected %d bytes, got %lu (delta %ld)",
             TOTAL_SIZE, (unsigned long)total, (long)total - (long)TOTAL_SIZE);
    bad("byte count", detail);
  } else {
    ok("byte count matches");
  }

  if (first_bad != (size_t)-1) {
    snprintf(detail, sizeof(detail),
             "first corrupt byte at offset %lu (expected 0x%02x got 0x%02x) "
             "-- this is the stream desync",
             (unsigned long)first_bad, pattern_byte(first_bad),
             got[first_bad]);
    bad("payload integrity", detail);
  } else {
    ok("payload integrity");
  }

  if (total_fds != 1 + NUM_SMALL) {
    snprintf(detail, sizeof(detail), "expected %d fds, received %lu",
             1 + NUM_SMALL, (unsigned long)total_fds);
    bad("fd count", detail);
  } else {
    ok("all fds received");
  }
  if (ctrunc_count)
    bad("MSG_CTRUNC during bulk transfer",
        "Mojo compiles this check out in release builds");

  free(got);
  close(sv[0]);
  close(sv[1]);
}

// ---------------------------------------------------------------------------
// Test E: how large a single sendmsg can QNX absorb, and does it ever report
// a short write with SCM_RIGHTS attached?
// ---------------------------------------------------------------------------

static void test_send_thresholds(void) {
  size_t sizes[] = {4096, 16384, 65536, 131072, 262144, 524288};
  size_t n_sizes = sizeof(sizes) / sizeof(sizes[0]);
  size_t s;

  printf("\n== E: single sendmsg capacity (nonblocking, fd attached) ==\n");
  for (s = 0; s < n_sizes; s++) {
    int sv[2];
    unsigned char* buf;
    struct iovec iov;
    int fd;
    ssize_t n;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) continue;
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    buf = (unsigned char*)calloc(1, sizes[s]);
    fd = make_fd();
    iov.iov_base = buf;
    iov.iov_len = sizes[s];
    n = probe_sendmsg_with_handles(sv[0], &iov, 1, &fd, 1);
    printf("  request %7lu -> %8ld%s\n", (unsigned long)sizes[s], (long)n,
           n < 0 ? (errno == EAGAIN ? " (EAGAIN)" : " (error)")
                 : ((size_t)n < sizes[s] ? "  SHORT" : ""));
    if (n < 0) printf("                   errno=%d %s\n", errno,
                      strerror(errno));
    close(fd);
    free(buf);
    close(sv[0]);
    close(sv[1]);
  }
}

// ---------------------------------------------------------------------------
// BB10 refuses to exec unsigned binaries from a shell, so the only way to run
// this on-device is to be launched by the Navigator as the app itself. When a
// content_shell.real.exe sits next to us we act as a pass-through wrapper:
// write the report to shared/misc, then exec the real binary so the browser
// still comes up normally.
// ---------------------------------------------------------------------------

#define REPORT_PATH "/accounts/1000/shared/misc/berry-sockprobe.txt"

static void resolve_dir(char* dir, size_t n, const char* argv0) {
  int fd;
  ssize_t r;
  char* slash;

  dir[0] = '\0';
  fd = open("/proc/self/exefile", O_RDONLY);
  if (fd >= 0) {
    r = read(fd, dir, n - 1);
    close(fd);
    if (r > 0) {
      dir[r] = '\0';
      while (r > 0 && (dir[r - 1] == '\n' || dir[r - 1] == '\r' ||
                       dir[r - 1] == ' ' || dir[r - 1] == '\0'))
        dir[--r] = '\0';
    }
  }
  if (!dir[0] && argv0 && strchr(argv0, '/')) {
    strncpy(dir, argv0, n - 1);
    dir[n - 1] = '\0';
  }
  if (dir[0]) {
    slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
  }
  if (!dir[0]) strncpy(dir, ".", n);
}

static void test_chunk_sweep(void);
static void test_sockbuf_tuning(void);

static void run_all_tests(void) {
  int sv[2];

  printf("=== QNX AF_UNIX SOCK_STREAM probe (Berry MP Mojo channel) ===\n");
  printf("pid=%d uid=%d\n", (int)getpid(), (int)getuid());
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    printf("socketpair failed: %s\n", strerror(errno));
    return;
  }
  test_env(sv[0]);
  close(sv[0]);
  close(sv[1]);

  test_ancillary_boundary();
  test_large_message();
  test_send_thresholds();
  test_chunk_sweep();
  test_sockbuf_tuning();

  printf("\n=== %d failure(s) ===\n", g_fail);
}

// ---------------------------------------------------------------------------
// Test F: find the largest chunk size that transfers losslessly on a BLOCKING
// socket, and measure how badly send() over-reports. This decides the fix.
// ---------------------------------------------------------------------------

#define SWEEP_TOTAL (256 * 1024)

struct sweep_args {
  int sock;
  size_t chunk;
  int attach_fd;
  size_t claimed;  // sum of send() return values
  int error;
};

static void* sweep_writer(void* p) {
  struct sweep_args* a = (struct sweep_args*)p;
  unsigned char* payload = (unsigned char*)malloc(SWEEP_TOTAL);
  size_t off = 0;
  size_t i;
  int first = 1;

  for (i = 0; i < SWEEP_TOTAL; i++) payload[i] = pattern_byte(i);

  while (off < SWEEP_TOTAL) {
    size_t want = SWEEP_TOTAL - off;
    ssize_t n;
    if (want > a->chunk) want = a->chunk;

    if (first && a->attach_fd) {
      int fd = make_fd();
      struct iovec iov;
      iov.iov_base = payload + off;
      iov.iov_len = want;
      n = probe_sendmsg_with_handles(a->sock, &iov, 1, &fd, 1);
      close(fd);
      first = 0;
    } else {
      n = send(a->sock, payload + off, want, MSG_NOSIGNAL);
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      a->error = errno;
      break;
    }
    a->claimed += (size_t)n;
    off += (size_t)n;
  }

  free(payload);
  shutdown(a->sock, SHUT_WR);
  return NULL;
}

static void sweep_one(size_t chunk, int attach_fd) {
  int sv[2];
  pthread_t tid;
  struct sweep_args wa;
  unsigned char* got;
  size_t total = 0;
  size_t first_bad = (size_t)-1;
  size_t i;

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
  got = (unsigned char*)malloc(SWEEP_TOTAL + 65536);
  memset(&wa, 0, sizeof(wa));
  wa.sock = sv[0];
  wa.chunk = chunk;
  wa.attach_fd = attach_fd;

  if (pthread_create(&tid, NULL, sweep_writer, &wa) != 0) {
    free(got);
    close(sv[0]);
    close(sv[1]);
    return;
  }

  for (;;) {
    ssize_t n = recv(sv[1], got + total, 4096, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    total += (size_t)n;
    if (total >= SWEEP_TOTAL + 32768) break;
  }
  pthread_join(tid, NULL);

  for (i = 0; i < total && i < SWEEP_TOTAL; i++) {
    if (got[i] != pattern_byte(i)) {
      first_bad = i;
      break;
    }
  }

  printf("  chunk %6lu %s: send() claimed %7lu, arrived %7lu, lost %6ld, %s\n",
         (unsigned long)chunk, attach_fd ? "fd " : "   ",
         (unsigned long)wa.claimed, (unsigned long)total,
         (long)wa.claimed - (long)total,
         (total == SWEEP_TOTAL && first_bad == (size_t)-1) ? "INTACT"
                                                           : "CORRUPT");
  if (first_bad != (size_t)-1)
    printf("                first corrupt byte at offset %lu\n",
           (unsigned long)first_bad);

  free(got);
  close(sv[0]);
  close(sv[1]);
}

static void test_chunk_sweep(void) {
  size_t chunks[] = {2048, 4096, 8192, 16332, 16384, 32768, 65536, 262144};
  size_t n = sizeof(chunks) / sizeof(chunks[0]);
  size_t i;

  printf("\n== F: blocking chunked write integrity (%d KB total) ==\n",
         SWEEP_TOTAL / 1024);
  printf("  If send() claims more than arrives, the return value cannot be\n"
         "  trusted -- which is exactly what Mojo's write loop relies on.\n");
  for (i = 0; i < n; i++) sweep_one(chunks[i], 0);
  printf("  -- same sweep with an fd on the first write --\n");
  for (i = 0; i < n; i++) sweep_one(chunks[i], 1);
}

// ---------------------------------------------------------------------------
// Test G: can we raise the socket buffers, and does that raise the ceiling?
// ---------------------------------------------------------------------------

static void test_sockbuf_tuning(void) {
  int targets[] = {32768, 131072, 262144};
  size_t n = sizeof(targets) / sizeof(targets[0]);
  size_t i;

  printf("\n== G: SO_SNDBUF / SO_RCVBUF tuning ==\n");
  for (i = 0; i < n; i++) {
    int sv[2];
    int want = targets[i];
    int got_snd = 0, got_rcv = 0;
    int rc_snd, rc_rcv;
    socklen_t len;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) continue;
    rc_snd = setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &want, sizeof(want));
    rc_rcv = setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
    len = sizeof(got_snd);
    getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &got_snd, &len);
    len = sizeof(got_rcv);
    getsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &got_rcv, &len);
    printf("  request %6d -> SO_SNDBUF=%6d (rc=%d) SO_RCVBUF=%6d (rc=%d)\n",
           want, got_snd, rc_snd, got_rcv, rc_rcv);

    if (rc_snd == 0 && got_snd >= want) {
      // Does a bigger buffer lift the single-send ceiling?
      unsigned char* buf = (unsigned char*)calloc(1, 262144);
      struct iovec iov;
      int fd = make_fd();
      ssize_t sent;
      fcntl(sv[0], F_SETFL, O_NONBLOCK);
      iov.iov_base = buf;
      iov.iov_len = 262144;
      sent = probe_sendmsg_with_handles(sv[0], &iov, 1, &fd, 1);
      printf("           single 262144-byte sendmsg now returns %ld\n",
             (long)sent);
      close(fd);
      free(buf);
    }
    close(sv[0]);
    close(sv[1]);
  }
}

int main(int argc, char** argv) {
  char dir[2048];
  char real[2100];
  int is_child = 0;
  int i;

  resolve_dir(dir, sizeof(dir), argv[0]);
  snprintf(real, sizeof(real), "%s/content_shell.real.exe", dir);

  if (access(real, F_OK) != 0) {
    // Standalone run (Linux baseline, or a device that lets us exec).
    run_all_tests();
    return g_fail ? 1 : 0;
  }

  // Wrapper mode. Child processes pass straight through -- we only want one
  // report per launch, from the browser process.
  for (i = 1; i < argc; i++) {
    if (argv[i] && strncmp(argv[i], "--type=", 7) == 0) is_child = 1;
  }

  if (!is_child) {
    fprintf(stderr, "BerryShell: running socket probe -> %s\n", REPORT_PATH);
    fflush(stderr);
    if (freopen(REPORT_PATH, "w", stdout) != NULL) {
      run_all_tests();
      fflush(stdout);
    }
    fprintf(stderr, "BerryShell: socket probe done, %d failure(s)\n", g_fail);
    fflush(stderr);
  }

  execv(real, argv);
  perror("BerryShell: socket probe wrapper execv failed");
  return 1;
}
