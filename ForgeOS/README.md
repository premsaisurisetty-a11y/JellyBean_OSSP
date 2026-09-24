# ForgeOS
Linux Process Monitoring and Control System

## Build

```bash
make
```

or:

```bash
gcc -Wall -Wextra -O2 main.c -o forgeos -lncurses
```

## Run

```bash
./forgeos
```

## Controls

- Up / Down: select a process
- C: sort by CPU
- M: sort by memory
- P: sort by PID
- S: SIGSTOP
- R: SIGCONT
- T: SIGTERM
- K: SIGKILL
- L: launch a demo child process (`sleep 300`)
- Q: quit

## Project concepts

- `/proc` scanning with `opendir()` / `readdir()`
- `/proc/[pid]/status`
- `/proc/[pid]/stat`
- `/proc/meminfo`
- ncurses live terminal UI
- CPU and memory sorting
- POSIX signals through `kill()`
- `getrusage()` resource accounting
- `fork()`, `exec()`, and `waitpid()` for the controlled demo child
- permission and process-exit race handling
