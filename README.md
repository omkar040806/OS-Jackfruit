# 🐳 Mini Container Runtime with Kernel Memory Monitor

## 1. Team Information

| Name | SRN |
|------|-----|
| Omkar | PES1UG24CS310 |
| Prahas Bodanapati | PES1UG24CS327 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Prepare Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

### Build

```bash
cd ~/OS-Jackfruit/boilerplate
make clean
make
```

### Load Kernel Module

```bash
sudo insmod monitor.ko
lsmod | grep monitor
ls -l /dev/container_monitor
```

### Create Per-Container Writable Root Filesystems

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

To run workload binaries inside a container, copy them into the container's rootfs before launch:

```bash
cp memory_hog ./rootfs-alpha/
cp cpu_hog ./rootfs-alpha/
cp io_pulse ./rootfs-beta/
```

### Start Supervisor

```bash
# Run in a dedicated terminal — stays alive as a daemon
sudo ./engine supervisor ./rootfs-base
```

### Start Containers (in another terminal)

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96
```

### Run a Container (foreground, blocks until exit)

```bash
sudo ./engine run memtest ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 30
```

### List Running Containers

```bash
sudo ./engine ps
```

### View Container Logs

```bash
sudo ./engine logs alpha
sudo ./engine logs beta
```

### Stop a Container

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Inspect Kernel Logs

```bash
dmesg | tail -20
```

### Unload Kernel Module and Clean Up

```bash
sudo rmmod monitor
rm -f /tmp/mini_runtime.sock
```

---

## 3. Demo with Screenshots

### 1. Multi-Container Supervision

![Multi-container](screenshots/Multi-container%20Supervision_1.png)

*Two containers (`alpha` and `beta`) launched under a single supervisor process. The supervisor stays alive managing both.*

![Multi-container](screenshots/Multi-container%20Supervision_2.png)

*Both containers executing concurrently with isolated PID and UTS namespaces visible from the host.*

---

### 2. Metadata Tracking (`ps` output)

![Metadata](screenshots/Metadata%20Tracking%20(ps%20output).png)

*Output of `engine ps` showing each container's ID, host PID, state, soft/hard memory limits, exit code, and log file path.*

---

### 3. Bounded-Buffer Logging

![Logging](screenshots/Bounded-buffer%20Logging.png)

*Log file contents written through the producer-consumer pipeline. Producer threads read from container pipes; the consumer thread flushes to per-container `.log` files in the `logs/` directory.*

---

### 4. CLI and IPC

![CLI IPC](screenshots/CLI%20and%20IPC.png)

*A CLI command (`engine start`) sent to the running supervisor over the UNIX domain socket at `/tmp/mini_runtime.sock`. The supervisor receives the request, starts the container, and sends a response back.*

---

### 5. Soft-Limit Warning

![Soft Limit](screenshots/Soft-limit%20Warning.png)

*`dmesg` output showing a soft-limit warning emitted by the kernel module when a container's RSS crossed the configured soft threshold. The container continues running.*

---

### 6. Hard-Limit Enforcement

![Hard Limit](screenshots/Hard-limit%20Enforcement.png)

*`dmesg` output showing the kernel module sending `SIGKILL` after RSS exceeded the hard limit. The supervisor metadata reflects state `killed` and exit code `137`.*

---

### 7. Scheduling Experiment

![Scheduling](screenshots/Scheduling%20Experiment.png)

*Two CPU-bound containers running `cpu_hog` with different `nice` values. The lower-nice container completes measurably faster, confirming the CFS scheduler's priority weighting.*

---

### 8. Clean Teardown (No Zombies)

![Teardown](screenshots/Clean%20Teardown%20(No%20Zombies).png)

*`ps aux | grep defunct` returns nothing after supervisor shutdown. All container children were reaped via `waitpid`, producer threads exited, and the logger thread was joined cleanly.*

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

The runtime achieves process and filesystem isolation through Linux namespaces and `chroot`. Each container is created with `clone()` using the flags `CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS`. This gives every container its own hostname (`sethostname` inside `child_fn`), its own PID numbering starting from 1, and its own mount namespace so filesystem changes are not visible to the host or sibling containers.

Filesystem isolation is achieved using `chroot()` into the container's dedicated rootfs directory (e.g., `rootfs-alpha/`), followed by `chdir("/")` to prevent directory traversal escapes. `mount("proc", "/proc", "proc", 0, NULL)` is called inside the container so that tools like `ps` see only the container's own process tree.

The host kernel is still shared by all containers — the same scheduler, the same physical memory, the same network stack (we do not use `CLONE_NEWNET`), and the same kernel module. Namespaces virtualise the *view* of those resources, not the resources themselves.

### 4.2 Supervisor and Process Lifecycle

A long-running parent supervisor is necessary because container processes are children of the process that called `clone()`. If that process exits, the children become orphans adopted by PID 1 and their exit status is never collected, causing zombies or lost metadata.

The supervisor stays alive in a `select()` loop, accepting CLI connections and handling `SIGCHLD`. When a container exits, the kernel delivers `SIGCHLD` to the supervisor. A signal handler sets `g_sigchld_seen`, and the main loop calls `reap_children()`, which calls `waitpid(-1, &status, WNOHANG)` in a loop to collect all ready children without blocking. For each reaped PID, the matching `container_record_t` is updated with the exit code, terminating signal, and final state (`stopped`, `exited`, or `killed`). Threads waiting on `run` are unblocked via `pthread_cond_broadcast` on the container's `exit_cv`. The `stop_requested` flag distinguishes a graceful supervisor-initiated stop from an unexpected kill.

If a container ignores SIGTERM for more than 2 seconds, the supervisor escalates to SIGKILL via escalate_stubborn_containers(), preventing indefinite hangs during shutdown

### 4.3 IPC, Threads, and Synchronization

The project uses two distinct IPC mechanisms:

**Path A — Logging (pipes):** Each container's `stdout` and `stderr` are connected to the write end of a `pipe()`. A dedicated producer thread per container reads from the read end and pushes `log_item_t` structs into the shared `bounded_buffer_t`. A single consumer (logger) thread pops items and appends them to per-container log files. Without synchronization, concurrent pushes and pops on the circular buffer would cause torn reads/writes and lost data.

The bounded buffer uses a `pthread_mutex` to serialize all access, plus two `pthread_cond_t` variables: `not_full` (producers wait here when the buffer is at capacity) and `not_empty` (the consumer waits here when the buffer is empty). This classic monitor pattern prevents busy-waiting, avoids missed wakeups, and guarantees that no log line is dropped as long as the buffer is not permanently full. On shutdown, `bounded_buffer_begin_shutdown()` sets a flag and broadcasts on both condition variables so blocked threads exit immediately. The consumer then drains all remaining items before returning.

**Path B — Control (UNIX domain socket):** The CLI process connects to `/tmp/mini_runtime.sock`, writes a fixed-size `control_request_t` struct, reads a fixed-size `control_response_t` struct, then reads any trailing text and exits. The supervisor accepts connections in its main loop with `accept()`. Because each CLI command is handled synchronously before the next `accept()`, no additional locking is needed for the socket itself. Container metadata is protected by `ctx.metadata_lock` (a `pthread_mutex`) because both the main thread and the SIGCHLD reap path access it.

A `mutex` was chosen over a `spinlock` for both locks because the critical sections involve memory allocation, file I/O path lookups, and condition-variable waits — operations that can sleep. Spinlocks must never be held across a sleep or a kernel preemption point, making them inappropriate here. In the kernel module, `mutex_lock` is similarly used in the timer callback and `ioctl` handler, both of which run in sleepable contexts.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical memory pages currently mapped into a process's address space and present in RAM. It does not count pages that have been swapped out, memory-mapped files that are not yet faulted in, or shared library pages counted once per library regardless of how many processes share them. This makes RSS a conservative, real-time indicator of actual physical memory pressure caused by a specific process.

Soft and hard limits represent different enforcement policies. A soft limit is a warning threshold — the container is allowed to continue operating, but the event is logged so operators can investigate. A hard limit is a termination threshold — once crossed, the container is killed with `SIGKILL` because it is consuming resources beyond what was provisioned.

Enforcement belongs in kernel space rather than user space because user-space polling is inherently racy: a process can exhaust memory between two polling intervals, or a misbehaving container can ignore or block signals sent by a user-space monitor. The kernel module's timer fires once per second inside the kernel, calls `get_mm_rss()` directly on the `task_struct`, and sends `SIGKILL` atomically via `send_sig()` with no window for the process to escape. The kernel also has privileged access to the process's `mm_struct` without needing `/proc` parsing.

### 4.5 Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS), which assigns CPU time proportional to each task's weight. Weight is derived from the `nice` value: lower nice means higher weight and a larger share of CPU time. Our experiments ran two CPU-bound `cpu_hog` containers simultaneously — one at `nice 0` and one at `nice 10`. The `nice 10` container received approximately half the CPU share of the `nice 0` container, consistent with the CFS weight table where `nice 0` has weight 1024 and `nice 10` has weight 110. Completion time for an equal workload was roughly 9× longer for the deprioritized container on a single-core VM, matching the weight ratio. An I/O-bound `io_pulse` container interleaved with a CPU-bound container showed that the I/O container frequently yielded the CPU voluntarily during `read`/`write` waits, allowing the CPU-bound container to run unimpeded, which is expected behavior because CFS rewards sleeping tasks with accumulated virtual runtime debt.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation — `chroot` over `pivot_root`

**Choice:** We used `chroot()` for filesystem isolation rather than `pivot_root`.

**Tradeoff:** `chroot` does not fully prevent a privileged process from escaping via `chroot("../../..")` after a second `chroot` call. `pivot_root` makes the old root inaccessible entirely, which is the approach used by production runtimes like runc.

**Justification:** For a university project where containers run trusted workloads, `chroot` is sufficient and significantly simpler to implement. Adding `MS_REC | MS_PRIVATE` remounting before `chroot` prevents mount propagation back to the host, which covers the main practical concern.

### Supervisor Architecture — Single-threaded event loop with per-container producer threads

**Choice:** The supervisor main loop is single-threaded (using `select()`), while each container gets its own producer thread for log reading.

**Tradeoff:** Handling one CLI client at a time means a slow `logs` response can delay the next command. A multi-threaded accept loop would remove this bottleneck but would require more careful locking of the metadata list.

**Justification:** Container management commands are infrequent and fast. The serialised model eliminates entire classes of race conditions on the supervisor's accept socket while keeping the code reviewable.

### IPC/Logging — UNIX domain socket + bounded circular buffer

**Choice:** UNIX domain socket for control (Path B), pipes plus a 16-slot circular buffer for logging (Path A).

**Tradeoff:** A fixed-capacity buffer of 16 slots can back-pressure producers if the consumer is slow (e.g., disk I/O stall). Dropping logs was explicitly ruled out by the spec, so the producer blocks on `not_full` rather than discarding.

**Justification:** Separating the two IPC paths keeps concerns clean: the control channel needs request-response semantics that sockets handle naturally, while logging needs one-way streaming that pipes handle efficiently. The bounded buffer decouples bursty container output from disk write latency.

### Kernel Monitor — `mutex` over `spinlock`

**Choice:** `DEFINE_MUTEX` protects the monitored list in both the timer callback and the `ioctl` handler.

**Tradeoff:** A mutex has higher acquisition cost than a spinlock on an uncontended fast path. A spinlock would be slightly faster if the critical section were always short and non-sleeping.

**Justification:** The timer callback calls `get_rss_bytes()` which takes `rcu_read_lock` and `get_task_mm` — both can schedule. Holding a spinlock across a potential schedule point is illegal in Linux and will cause a kernel BUG. A mutex is the correct primitive here.

### Scheduling Experiments — `nice` values over CPU affinity

**Choice:** We used `nice()` inside `child_fn` to adjust scheduler priority rather than `sched_setaffinity` to pin containers to cores.

**Tradeoff:** `nice` affects relative CPU share but does not provide hard isolation. Two CPU-bound containers at the same nice value share the CPU equally, which can make latency-sensitive workloads unpredictable. CPU affinity would give deterministic isolation but removes the scheduler's ability to balance load.

**Justification:** `nice` directly exercises CFS weight-based scheduling, which is the core scheduling mechanism the project asks us to explore. Pinning to cores would bypass the scheduler entirely and produce less interesting results for the analysis.

---

## 6. Scheduler Experiment Results

### Experiment 1: Two CPU-Bound Containers at Different Nice Values

Both containers ran an identical CPU-bound loop (counting to 10 billion). Container `hi` ran at `nice 0`, container `lo` ran at `nice 10`.

| Container | Nice | Wall-clock time (s) | CPU share observed |
|-----------|------|--------------------|--------------------|
| hi        | 0    | ~18                | ~90%               |
| lo        | 10   | ~162               | ~10%               |

The CFS weight for `nice 0` is 1024 and for `nice 10` is 110. The theoretical CPU ratio is 1024 / (1024 + 110) ≈ 90%, which matches the observed result closely.

### Experiment 2: CPU-Bound vs I/O-Bound Container

Container `cpu` ran `cpu_hog` at `nice 0`. Container `io` ran `io_pulse` (alternating 1 MB writes and reads with `fsync`) at `nice 0`.

| Container | Type    | CPU utilisation | Completion time |
|-----------|---------|-----------------|-----------------|
| cpu       | CPU     | ~98%            | ~20 s           |
| io        | I/O     | ~4%             | ~22 s           |

The I/O container voluntarily yielded the CPU during every `fsync` call, accumulating CFS virtual runtime debt. When it woke from I/O, CFS temporarily prioritised it to drain that debt, giving it short bursts of 100% CPU. The CPU-bound container was largely unaffected because the I/O container's active periods were brief. This demonstrates CFS's responsiveness goal: sleeping tasks are rewarded with priority boosts when they wake, preventing starvation of interactive or I/O-driven workloads.

**Conclusion:** Linux CFS distributes CPU proportionally to task weight (controlled via `nice`) and rewards sleeping tasks with catchup time. These behaviors align with the scheduler's dual goals of fairness (equal nice → equal share) and responsiveness (I/O tasks get priority on wakeup).

---

## 🧠 Key Observations

- Containers exceeding the **soft limit** generate `dmesg` warnings but continue execution.
- Containers exceeding the **hard limit** are terminated with `SIGKILL`; exit code is **137**.
- CPU scheduling share changes measurably with different **nice values**, consistent with CFS weight tables.
- The supervisor correctly tracks the full container lifecycle and distinguishes `stopped` (operator-requested) from `killed` (hard-limit enforcement).

---

## 🧹 Clean Teardown Verification

```bash
ps aux | grep defunct
```

✔ No zombie processes found. All children reaped via `waitpid`, all producer threads exited when pipe read ends closed, and the logger thread was joined after `bounded_buffer_begin_shutdown`.

---

## 👨‍💻 Authors

Omkar (SRN: PES1UG24CS310)
Prahas Bodanapati (SRN: PES1UG24CS327)
