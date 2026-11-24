#!/usr/bin/env python3
"""Where one thread's time went, from a QNX kernel trace.

usage: kevsum.py <traceprinter-output.txt> <pid> [tid]

Decode the .kev first:  traceprinter -f results/kev-X.kev > /tmp/kev-X.txt
Without a tid, the thread of <pid> that ran the most is used (the page's main
thread during a load). Reports time per thread state and, for time spent
blocked on another process, which process and which request.
"""
import collections
import re
import sys

path, PID = sys.argv[1], sys.argv[2]
TID = sys.argv[3] if len(sys.argv) > 3 else None

IO = ["CONNECT(open)", "READ", "WRITE", "CLOSE_OCB", "STAT", "NOTIFY", "DEVCTL", "UNBLOCK",
      "PATHCONF", "LSEEK", "CHMOD", "CHOWN", "UTIME", "OPENFD", "FDINFO", "LOCK", "SPACE",
      "SHUTDOWN", "MMAP", "MSG", "RSVD", "DUP", "CLOSE", "LOCK_OCB", "UNLOCK_OCB", "SYNC", "POWER"]
MEM = ["MAP(mmap)", "CTRL", "INFO", "OFFSET", "DEBUG_INFO", "SWAP", "PMEM_ADD", "PEER"]
MEMCTRL = ["munmap", "mprotect", "msync", "mlockall", "munlockall", "mlock", "munlock", "madvise"]


def msg_name(w0):
    typ, sub = w0 & 0xFFFF, w0 >> 16
    if 0x100 <= typ < 0x100 + len(IO):
        return "io " + IO[typ - 0x100]
    if 0x40 <= typ < 0x40 + len(MEM):
        if typ == 0x41:
            return "mem " + (MEMCTRL[sub] if sub < len(MEMCTRL) else "ctrl%d" % sub)
        return "mem " + MEM[typ - 0x40]
    return "type 0x%03x/%d" % (typ, sub)


ev = re.compile(r"^t:0x([0-9a-f]+) CPU:(\d+) (\w+)\s*:\s*(\w+)(.*)$")
thr = re.compile(r"pid:(\d+) tid:(\d+)")
words = re.compile(r"\((0x[0-9a-f]+) (0x[0-9a-f]+) (0x[0-9a-f]+)\)")

names = {}
last_call = {}   # cpu -> first word of the last MSG_SEND* message
last_target = {}  # cpu -> pid the last message was sent to
pending_cpu = None
naming = None    # reading the pid/name lines of a PROCCREATE_NAME record
on_cpu = {}      # cpu -> ((pid, tid), since)
ran = collections.Counter()
state_n = collections.defaultdict(lambda: collections.Counter())
cur = {}         # tid -> (state, detail, since)
tot = collections.defaultdict(lambda: collections.Counter())    # tid -> state -> ticks
detail = collections.defaultdict(lambda: collections.Counter())  # tid -> (state, detail) -> ticks
count = collections.defaultdict(lambda: collections.Counter())
base, prev_t, t = 0, 0, 0
first_t = last_t = None
hz = 19200000

for line in open(path, errors="replace"):
    if line.startswith("  TRACE_CYCLES_PER_SEC::"):
        hz = int(line.split("::")[1])
        continue
    m = ev.match(line)
    if not m:
        if naming is not None:
            f = line.strip().split(":", 1)
            if f[0] == "pid":
                naming = f[1]
            elif f[0] == "name" and naming:
                names[naming] = f[1].split("/")[-1]
                naming = None
            continue
        if pending_cpu is not None and "msg:" in line:
            w = words.search(line)
            if w:
                last_call[pending_cpu] = int(w.group(1), 16)
            pending_cpu = None
        continue
    raw = int(m.group(1), 16)
    if raw + (1 << 31) < prev_t:
        base += 1 << 32
    prev_t = raw
    t = base + raw
    if first_t is None:
        first_t = t
    last_t = t
    cpu, cls, name, rest = m.group(2), m.group(3), m.group(4), m.group(5)
    pending_cpu = None
    naming = "" if (cls == "PROCESS" and name == "PROCCREATE_NAME") else None
    if cls == "THREAD" and name == "THRUNNING":
        p = thr.search(rest)
        if p:
            if cpu in on_cpu:
                ran[on_cpu[cpu][0]] += t - on_cpu[cpu][1]
            on_cpu[cpu] = ((p.group(1), p.group(2)), t)
    if cls == "KER_CALL" and name.startswith("MSG_SEND"):
        pending_cpu = cpu
        last_target.pop(cpu, None)
    elif cls == "COMM" and name == "SND_MESSAGE":
        p = re.search(r"pid:(\d+)", rest)
        if p:
            last_target[cpu] = p.group(1)
    elif cls == "THREAD" and name.startswith("TH"):
        p = thr.search(rest)
        if not p or p.group(1) != PID:
            continue
        tid = p.group(2)
        state = name[2:]
        if tid in cur:
            s0, d0, since = cur[tid]
            tot[tid][s0] += t - since
            detail[tid][(s0, d0)] += t - since
            state_n[tid][s0] += 1
            count[tid][(s0, d0)] += 1
        d = ""
        if state in ("REPLY", "SEND"):
            if tid in cur and cur[tid][0] == "SEND" and state == "REPLY":
                d = cur[tid][1]  # same request, now being served
            else:
                d = "%s: %s" % (last_target.get(cpu, "?"), msg_name(last_call.get(cpu, 0xFFFF)))
        cur[tid] = (state, d, t)

if TID is None:
    TID = max(tot, key=lambda k: tot[k]["RUNNING"])
ms = lambda ticks: 1000.0 * ticks / hz
span = sum(tot[TID].values())
print("trace %.2fs; pid %s thread %s accounted %.2fs" % (ms(last_t - first_t) / 1000, PID, TID, ms(span) / 1000))
print("\n-- time by thread state --")
for s, v in tot[TID].most_common():
    print("%8.0f ms %5.1f%%  %7d x  %s" % (ms(v), 100.0 * v / span, state_n[TID][s], s))

print("\n-- who was on the 4 CPUs (share of one CPU) --")
whole = last_t - first_t
idle = sum(v for (p, tid), v in ran.items() if p == "1" and int(tid) <= 4)
print("%8.0f ms %5.1f%%  idle (all CPUs together: %.0f%% of capacity idle)" % (
    ms(idle), 100.0 * idle / whole, 100.0 * idle / (4 * whole)))
for (p, tid), v in [kv for kv in ran.most_common(16) if not (kv[0][0] == "1" and int(kv[0][1]) <= 4)][:11]:
    print("%8.0f ms %5.1f%%  %s tid %s" % (ms(v), 100.0 * v / whole, names.get(p, "pid " + p), tid))

by_proc = collections.Counter()
for (s, d), v in detail[TID].items():
    if s in ("REPLY", "SEND") and d:
        by_proc[d.split(":")[0]] += v
print("\n-- blocked on another process, by process --")
for p, v in by_proc.most_common(8):
    print("%8.0f ms %5.1f%%  %s" % (ms(v), 100.0 * v / span, names.get(p, "pid " + p)))

print("\n-- blocked on another process, by request --")
rows = sorted(((v, k) for k, v in detail[TID].items() if k[0] in ("REPLY", "SEND")), reverse=True)
for v, (s, d) in rows[:22]:
    p, _, what = d.partition(": ")
    n = count[TID][(s, d)]
    print("%8.0f ms %5.1f%%  %6d x  %5.0f us each  %-5s %-16s %s" % (
        ms(v), 100.0 * v / span, n, 1000 * ms(v) / n, s, names.get(p, "pid " + p), what))
