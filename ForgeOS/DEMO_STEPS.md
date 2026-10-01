# ForgeOS Demo Steps

1. `make clean`
2. `make`
3. `./forgeos`
4. Show the live /proc process table.
5. Press `L` to create the controlled `sleep` demo child.
6. Select its PID and press `S` to stop it.
7. Press `R` to resume it.
8. Create another demo with `L`, select it and press `T` for SIGTERM.
9. Create another demo with `L`, select it and press `K` for SIGKILL.
10. Press `V` for FIFO/LRU page replacement.
11. Press `I` for pipe IPC.
12. Press `U` for getrusage().
13. Press C/M/P to demonstrate sorting.
14. Press H for controls.
