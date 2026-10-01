# ForgeOS Final Project

Linux Process Monitoring and Control System.

## Requirements
Ubuntu/WSL with GCC, Make and ncurses development files:
```bash
sudo apt update
sudo apt install build-essential libncurses-dev
```

## Build and run
```bash
make clean
make
./forgeos
```

## Controls
Up/Down select | C CPU sort | M memory sort | P PID sort
S SIGSTOP | R SIGCONT | T SIGTERM | K SIGKILL
L controlled demo child | V FIFO/LRU memory module | I pipe IPC demo
U getrusage | H help | Q quit

## CO mapping
CO1: Linux C, /proc and system interfaces
CO2: fork/exec/waitpid and process control
CO3: signals and pipe IPC
CO4: FIFO and LRU page replacement
CO5: /proc directory/file handling
CO6: pthread monitor + semaphore-protected shared process table
