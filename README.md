# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor.

---

## 1. Team Information

| Name                      | SRN |
|------                     |-----|
| Shweta Mailaragouda Patil | PES2UG25CS825 |
| Soubhagya                 | PES2UG25CS827 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot OFF. WSL will not work.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build

```bash
cd boilerplate
make
```

### Prepare Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

### Launch Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96
```

### CLI Commands

```bash
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
```

### Teardown

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
# Ctrl+C to stop supervisor
ps aux | grep defunct
sudo rmmod monitor
lsmod | grep monitor
```

---

## 3. Demo with Screenshots

### Screenshot 1 — Multi-Container Supervision

![Screenshot 1]<img width="866" height="360" alt="image" src="https://github.com/user-attachments/assets/3cce4ed6-b0b4-4157-89d0-dbb6c8d2e66d" />


*Two containers launched under one supervisor: alpha (PID 6414) and beta (PID 6426) both running concurrently.*

---

### Screenshot 2 — Metadata Tracking

![Screenshot 2]<img width="866" height="670" alt="image" src="https://github.com/user-attachments/assets/68738739-42d5-4020-8f2f-4aca74a70e5d" />


*`engine ps` shows container IDs, host PIDs, states (running), and hard memory limits for both containers.*

---

### Screenshot 3 — Bounded-Buffer Logging

![Screenshot 3]<img width="866" height="670" alt="image" src="https://github.com/user-attachments/assets/1966bb29-2283-47f2-a177-3d7c17d15248" />


*`engine logs alpha` reads from `logs/alpha.log`, populated by the supervisor's bounded-buffer producer/consumer pipeline capturing container stdout.*

---

### Screenshot 4 — CLI and IPC

![Screenshot 4]<img width="866" height="670" alt="image" src="https://github.com/user-attachments/assets/51b6eec9-1832-43ec-a04c-c11ce9ee56c9" />


*`engine stop alpha` sends a command over the UNIX domain socket. The supervisor responds with "Sent SIGTERM to container 'alpha'", confirming the IPC channel works.*

---

### Screenshot 5 — Soft-Limit Warning

![Screenshot 5](screenshot/screenshot%205%206.png)<img width="866" height="670" alt="image" src="https://github.com/user-attachments/assets/75952235-6773-4f42-b035-a954e9385477" />


*dmesg shows `[container_monitor] SOFT LIMIT container=memtest pid=6757 rss=59318272 limit=52428800` — the kernel module detected RSS exceeding the soft limit and logged a warning.*

---

### Screenshot 6 — Hard-Limit Enforcement

![Screenshot 6]<img width="866" height="670" alt="image" src="https://github.com/user-attachments/assets/3bb0abf6-27d0-4c96-b912-92d5ab79cbd6" />


*dmesg shows `[container_monitor] HARD LIMIT container=memtest pid=6757 rss=210444288 limit=209715200` — the kernel module sent SIGKILL and the container was terminated.*

---

### Screenshot 7 — Scheduling Experiment

![Screenshot 7]<img width="1214" height="768" alt="image" src="https://github.com/user-attachments/assets/71404168-c967-4325-b309-349a73895e8d" />


*cpuA (nice=0) and cpuB (nice=15) ran the same cpu_hog workload simultaneously. Both completed 10 seconds but cpuA received more CPU time from the CFS scheduler due to its higher priority weight.*

---

### Screenshot 8 — Clean Teardown

![Screenshot 8]<img width="866" height="670" alt="image" src="https://github.com/user-attachments/assets/867f5cca-cdac-4d99-861f-09bd141bf817" />

![Screenshot 9]<img width="866" height="670" alt="image" src="https://github.com/user-attachments/assets/e9856449-22ca-4e0a-ae5b-2c6be56769ed" />


*`ps aux | grep defunct` shows no zombie processes. `sudo rmmod monitor` succeeds. `lsmod | grep monitor` returns nothing — clean teardown confirmed.*

---

## 4. Engineering Analysis

### 1. Isolation Mechanisms

The runtime uses Linux namespaces to isolate each container. `clone()` is called with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`, creating isolated PID, UTS, and mount namespaces. The child then calls `chroot()` to restrict its filesystem view to its assigned `rootfs-*` directory, and mounts `/proc` so tools like `ps` work inside the container. The host kernel still shares physical memory, CPU scheduling, network stack, and devices with all containers — namespaces provide an isolated view, not a hardware partition.

### 2. Supervisor and Process Lifecycle

A long-running supervisor is necessary because the kernel requires a parent process to reap children via `waitpid()`. Without it, exited containers become zombies. The supervisor maintains a linked list of `container_record_t` structs protected by a mutex. On `SIGCHLD`, it calls `waitpid()` in a non-blocking loop, updates container state to `CONTAINER_EXITED` or `CONTAINER_KILLED`, and records the exit code. On `SIGINT`/`SIGTERM`, it sets a shutdown flag, joins the logger thread, and kills any remaining containers before exiting.

### 3. IPC, Threads, and Synchronization

Two IPC mechanisms are used. **Path A (logging):** Container stdout/stderr flows through `pipe()` into a `bounded_buffer_t` shared between producer threads (one per container) and a single consumer logger thread. The buffer uses a `pthread_mutex_t` for mutual exclusion and two `pthread_cond_t` variables (`not_empty`, `not_full`) for producer/consumer coordination. **Path B (control):** The CLI connects to the supervisor over a UNIX domain socket (`AF_UNIX, SOCK_STREAM`), sends a `control_request_t`, and receives a `control_response_t`. The metadata list is protected by a separate mutex since the accept loop and SIGCHLD handler can access it concurrently. In the kernel module, `spin_lock_bh` is used because the timer callback runs in softirq context where sleeping locks are forbidden.

### 4. Memory Management and Enforcement

RSS (Resident Set Size) measures physical memory pages currently in RAM for a process. It does not count swapped-out pages, unfaulted mapped files, or shared library pages. Soft and hard limits serve different policies: the soft limit warns without killing (allowing graceful response), while the hard limit enforces a strict ceiling with SIGKILL. Enforcement belongs in kernel space because user-space monitors can be descheduled or delayed. The kernel timer fires reliably every tick, and `get_mm_rss()` and `send_sig()` are available directly without syscall overhead.

### 5. Scheduling Behavior

Linux CFS assigns CPU time proportionally by weight derived from `nice` values. Nice=0 has weight 1024; nice=15 has weight ~88. With cpuA (nice=0) and cpuB (nice=15) running together, CFS allocates ~92% CPU to cpuA and ~8% to cpuB. Both containers completed the 10-second duration, but cpuA performed more iterations. This demonstrates CFS proportional-share scheduling: higher nice values reduce CPU share without starvation, making them suitable for background/batch work that should not impact foreground processes.

---

## 5. Design Decisions and Tradeoffs

**Namespace Isolation:** Used `chroot` instead of `pivot_root`. Simpler to implement but escapable by a privileged process. Sufficient for demonstrating isolation concepts in this project.

**Supervisor Architecture:** Single blocking accept loop handles CLI requests sequentially. One request at a time avoids complex concurrent locking. Acceptable because CLI commands complete in microseconds.

**IPC and Logging:** UNIX socket for control (bidirectional, message-oriented); pipes + bounded buffer for logging (natural for child stdout capture). The bounded buffer can drop data if the consumer falls behind, but a capacity of 16 chunks is sufficient for normal workloads.

**Kernel Monitor:** 1-second timer interval balances enforcement latency against kernel overhead. Spinlock required (not mutex) due to softirq context. A container could allocate significant memory within one tick window, but this is acceptable for this project's scale.

**Scheduling Experiments:** Used `nice` values to demonstrate CFS priority differences. Does not provide hard CPU guarantees like `SCHED_FIFO`, but works without elevated privileges and clearly shows proportional allocation behavior.

---

## 6. Scheduler Experiment Results

| Container | Nice Value | Duration | Completed |
|-----------|-----------|----------|-----------|
| cpuA | 0 | 10s | Yes |
| cpuB | 15 | 10s | Yes |

Both containers ran the same `cpu_hog` for 10 seconds simultaneously. cpuA received approximately 92% of available CPU time vs cpuB's 8%, matching the theoretical CFS weight ratio for nice=0 vs nice=15. Both completed within the time window because `cpu_hog` is time-bounded. The difference in accumulator values between the two logs confirms cpuA executed significantly more loop iterations per second. This shows CFS provides fairness proportional to weight — lower-priority processes are not starved but receive less CPU share when competing with higher-priority work.
