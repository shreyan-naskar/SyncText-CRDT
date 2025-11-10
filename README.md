# SyncText-CRDT
A collaborative text editor that synchronizes document changes across multiple processes using **POSIX message queues**, **shared memory**, and a **Last-Writer-Wins CRDT** merge strategy. Each user edits a **local copy** of the document, and the system ensures that all replicas eventually converge to the same state, without requiring locks or central coordination.

---

## Features

| Feature | Description |
|--------|-------------|
| Local editing | Users freely edit their document using any editor (vim, nano, gedit, etc.). |
| Real-time change detection | File system timestamps are monitored to detect and classify changes. |
| Peer discovery | Shared memory registry tracks up to 5 active users and their message queues. |
| Message-based broadcast | Changes are accumulated and broadcast using POSIX message queues. |
| Lock-free concurrency | A single-producer/single-consumer ring buffer transfers incoming updates. |
| CRDT merging | Conflicting edits are resolved deterministically using Last-Writer-Wins. |
| UI terminal display | Current document view, recent edits, and merge notifications are displayed live. |

---

## System Requirements
- Linux environment with:
  - POSIX message queues enabled (`/dev/mqueue` mounted)
  - Shared memory support (`/dev/shm`)
- C++17 compatible compiler (e.g., `g++`)
- Multi-terminal access (one terminal per user/editor instance)

---

## Makefile and execution

Copy and paste this in bash terminal

```bash
# execute the Makefile
make

# execute the .exe file 
# for example: ./control u1
./control <user_id>
