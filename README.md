# 🐳 Mini Container Runtime with Kernel Memory Monitor

## 📌 Project Overview

This project implements a **mini container runtime** in user space along with a **kernel module for memory monitoring and enforcement**.

* The **user-space engine** acts as a supervisor to manage multiple containers.
* The **kernel module** tracks memory usage and enforces **soft and hard limits**.
* Containers run inside an isolated **root filesystem (chroot-based environment)**.
* Logging is handled using a **bounded-buffer producer-consumer pipeline**.

---

## ⚙️ Features

* Multi-container supervision
* Metadata tracking (PID, state, limits, exit codes)
* Bounded-buffer logging system
* CLI + IPC via UNIX domain sockets
* Soft memory limit warnings
* Hard memory limit enforcement (SIGKILL)
* Scheduling control using `nice` values
* Clean teardown (no zombie processes)

---

## 🏗️ Architecture

* **engine.c** → User-space container runtime (supervisor + CLI)
* **monitor.c** → Kernel module for memory monitoring
* **Workloads**:

  * `memory_hog` → consumes memory
  * `cpu_hog` → CPU-intensive task
  * `io_pulse` → I/O simulation

---

## 🚀 How to Run

### 1. Build

```bash
cd ~/OS-Jackfruit/boilerplate
make clean
make
```

### 2. Load Kernel Module

```bash
sudo insmod monitor.ko
lsmod | grep monitor
```

### 3. Start Supervisor

```bash
sudo ./engine supervisor ../rootfs-base
```

### 4. Start Containers

```bash
sudo ./engine start cont1 ../rootfs-alpha /bin/sh
sudo ./engine start cont2 ../rootfs-alpha /bin/sh
```

### 5. Check Running Containers

```bash
sudo ./engine ps
```

### 6. View Logs

```bash
sudo ./engine logs <container_id>
```

### 7. Stop Containers

```bash
sudo ./engine stop <container_id>
```

---

## 📸 Screenshots

### 1. Multi-container Supervision

![Multi-container](PASTE_LINK_HERE)

---

### 2. Metadata Tracking (`ps` output)

![Metadata](PASTE_LINK_HERE)

---

### 3. Bounded-buffer Logging

![Logging](PASTE_LINK_HERE)

---

### 4. CLI and IPC

![CLI IPC](PASTE_LINK_HERE)

---

### 5. Soft-limit Warning

![Soft Limit](PASTE_LINK_HERE)

---

### 6. Hard-limit Enforcement

![Hard Limit](PASTE_LINK_HERE)

---

### 7. Scheduling Experiment

![Scheduling](PASTE_LINK_HERE)

---

### 8. Clean Teardown (No Zombies)

![Teardown](PASTE_LINK_HERE)

---

## 🧠 Key Observations

* Containers exceeding **soft limit** generate warnings but continue execution.
* Containers exceeding **hard limit** are terminated with exit code **137 (SIGKILL)**.
* CPU scheduling behavior changes with different **nice values**.
* Supervisor correctly tracks lifecycle and cleans up processes.

---

## 🧪 Example Outputs

### Soft Limit

```
SOFT LIMIT container=memA pid=27182 rss=25821184 limit=20971520
```

### Hard Limit

```
HARD LIMIT container=memA pid=27182 rss=34209792 limit=31457280
```

### Container Killed

```
memA ... killed ... 137
```

---

## 🧹 Clean Teardown Verification

```bash
ps aux | grep defunct
```

✔ No zombie processes found

---

## 📚 Conclusion

This project demonstrates:

* Integration of **user-space and kernel-space components**
* Resource isolation and monitoring
* Process lifecycle management
* Inter-process communication using sockets

---

## 👨‍💻 Authors

Omkar (SRN: PES1UG24CS310)
Prahas Bodanapati (SRN: PES1UG24CS327)
---
