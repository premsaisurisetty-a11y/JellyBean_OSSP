
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ncurses.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <stdarg.h>

#define MAX_PROCESSES 8192
#define NAME_LEN 64

typedef struct {
    pid_t pid;
    char name[NAME_LEN];
    char state;
    long rss_kb;
    unsigned long long cpu_ticks;
    double cpu_percent;
} ProcessInfo;

typedef struct {
    pid_t pid;
    unsigned long long ticks;
} CpuSample;

typedef enum {
    SORT_PID,
    SORT_CPU,
    SORT_MEMORY
} SortMode;

typedef struct {
    unsigned long long total_kb;
    unsigned long long available_kb;
    unsigned long long used_kb;
    unsigned long long swap_total_kb;
    unsigned long long swap_free_kb;
} MemoryInfo;

static CpuSample previous_samples[MAX_PROCESSES];
static size_t previous_count = 0;
static unsigned long long previous_total_cpu = 0;

static pid_t selected_pid = -1;
static pid_t demo_child_pid = -1;
static SortMode sort_mode = SORT_PID;
static int scroll_offset = 0;
static char status_message[256] = "Ready";

static int is_numeric(const char *s)
{
    if (s == NULL || *s == '\0')
        return 0;

    while (*s) {
        if (!isdigit((unsigned char)*s))
            return 0;
        s++;
    }

    return 1;
}

static void set_status(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(status_message, sizeof(status_message), format, args);
    va_end(args);
}

static unsigned long long read_total_cpu_ticks(void)
{
    FILE *file = fopen("/proc/stat", "r");

    if (file == NULL)
        return 0;

    char line[512];

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 0;
    }

    fclose(file);

    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;

    sscanf(line,
           "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &user, &nice, &system, &idle,
           &iowait, &irq, &softirq, &steal);

    return user + nice + system + idle +
           iowait + irq + softirq + steal;
}

static int find_previous_cpu(pid_t pid,
                             unsigned long long *ticks)
{
    for (size_t i = 0; i < previous_count; i++) {
        if (previous_samples[i].pid == pid) {
            *ticks = previous_samples[i].ticks;
            return 1;
        }
    }

    return 0;
}

static void store_previous_cpu(pid_t pid,
                               unsigned long long ticks)
{
    for (size_t i = 0; i < previous_count; i++) {
        if (previous_samples[i].pid == pid) {
            previous_samples[i].ticks = ticks;
            return;
        }
    }

    if (previous_count < MAX_PROCESSES) {
        previous_samples[previous_count].pid = pid;
        previous_samples[previous_count].ticks = ticks;
        previous_count++;
    }
}

static int read_process_status(pid_t pid, ProcessInfo *process)
{
    char path[128];
    char line[512];

    snprintf(path, sizeof(path),
             "/proc/%d/status", pid);

    FILE *file = fopen(path, "r");

    if (file == NULL)
        return 0;

    process->name[0] = '\0';
    process->state = '?';
    process->rss_kb = 0;

    while (fgets(line, sizeof(line), file)) {

        if (strncmp(line, "Name:", 5) == 0) {
            char temp[NAME_LEN];

            if (sscanf(line, "Name:\t%63[^\n]", temp) == 1 ||
                sscanf(line, "Name: %63[^\n]", temp) == 1) {

                strncpy(process->name,
                        temp,
                        NAME_LEN - 1);

                process->name[NAME_LEN - 1] = '\0';
            }
        }

        else if (strncmp(line, "State:", 6) == 0) {
            sscanf(line, "State: %c",
                   &process->state);
        }

        else if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line, "VmRSS: %ld",
                   &process->rss_kb);
        }
    }

    fclose(file);

    return process->name[0] != '\0';
}

static int read_process_stat(pid_t pid,
                             unsigned long long *cpu_ticks,
                             char *state)
{
    char path[128];
    char line[4096];

    snprintf(path, sizeof(path),
             "/proc/%d/stat", pid);

    FILE *file = fopen(path, "r");

    if (file == NULL)
        return 0;

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 0;
    }

    fclose(file);

    /*
       The process name is field 2 and is enclosed in parentheses.
       Find the last ')' so names containing ')' are handled safely.
    */
    char *closing_parenthesis = strrchr(line, ')');

    if (closing_parenthesis == NULL)
        return 0;

    char *data = closing_parenthesis + 2;

    /*
       After the process name:
       field 3  = state
       field 14 = utime
       field 15 = stime
    */

    int field = 3;
    unsigned long long utime = 0;
    unsigned long long stime = 0;

    char *save_pointer = NULL;
    char *token = strtok_r(data, " ",
                           &save_pointer);

    while (token != NULL) {

        if (field == 3) {
            *state = token[0];
        }

        else if (field == 14) {
            utime = strtoull(token, NULL, 10);
        }

        else if (field == 15) {
            stime = strtoull(token, NULL, 10);
            break;
        }

        field++;
        token = strtok_r(NULL, " ",
                         &save_pointer);
    }

    *cpu_ticks = utime + stime;

    return field >= 15;
}

static int read_memory_info(MemoryInfo *memory)
{
    FILE *file = fopen("/proc/meminfo", "r");

    if (file == NULL)
        return 0;

    char line[256];

    memset(memory, 0, sizeof(*memory));

    while (fgets(line, sizeof(line), file)) {

        unsigned long long value = 0;

        if (sscanf(line, "MemTotal: %llu", &value) == 1)
            memory->total_kb = value;

        else if (sscanf(line, "MemAvailable: %llu", &value) == 1)
            memory->available_kb = value;

        else if (sscanf(line, "SwapTotal: %llu", &value) == 1)
            memory->swap_total_kb = value;

        else if (sscanf(line, "SwapFree: %llu", &value) == 1)
            memory->swap_free_kb = value;
    }

    fclose(file);

    if (memory->total_kb >= memory->available_kb)
        memory->used_kb =
            memory->total_kb - memory->available_kb;

    return 1;
}

static int read_process(pid_t pid,
                        ProcessInfo *process,
                        unsigned long long total_cpu,
                        long cpu_count)
{
    memset(process, 0, sizeof(*process));

    process->pid = pid;

    if (!read_process_status(pid, process))
        return 0;

    unsigned long long current_ticks = 0;
    char stat_state = '?';

    if (!read_process_stat(pid,
                           &current_ticks,
                           &stat_state))
        return 0;

    process->cpu_ticks = current_ticks;

    if (stat_state != '?')
        process->state = stat_state;

    process->cpu_percent = 0.0;

    unsigned long long old_ticks = 0;

    if (find_previous_cpu(pid, &old_ticks) &&
        total_cpu > previous_total_cpu) {

        unsigned long long process_delta =
            current_ticks - old_ticks;

        unsigned long long total_delta =
            total_cpu - previous_total_cpu;

        /*
           Linux top-style percentage:
           process share of total CPU * number of CPUs.
        */
        process->cpu_percent =
            ((double)process_delta /
             (double)total_delta) *
            100.0 *
            (double)cpu_count;
    }

    store_previous_cpu(pid, current_ticks);

    return 1;
}

static const char *state_text(char state)
{
    switch (state) {
        case 'R': return "Running";
        case 'S': return "Sleeping";
        case 'D': return "Waiting";
        case 'T': return "Stopped";
        case 'Z': return "Zombie";
        case 'I': return "Idle";
        default:  return "Unknown";
    }
}

static int compare_processes(const void *a,
                             const void *b)
{
    const ProcessInfo *p1 =
        (const ProcessInfo *)a;

    const ProcessInfo *p2 =
        (const ProcessInfo *)b;

    if (sort_mode == SORT_CPU) {

        if (p1->cpu_percent < p2->cpu_percent)
            return 1;

        if (p1->cpu_percent > p2->cpu_percent)
            return -1;
    }

    else if (sort_mode == SORT_MEMORY) {

        if (p1->rss_kb < p2->rss_kb)
            return 1;

        if (p1->rss_kb > p2->rss_kb)
            return -1;
    }

    if (p1->pid < p2->pid)
        return -1;

    if (p1->pid > p2->pid)
        return 1;

    return 0;
}

static int scan_processes(ProcessInfo *processes,
                          unsigned long long total_cpu,
                          long cpu_count)
{
    DIR *directory = opendir("/proc");

    if (directory == NULL)
        return 0;

    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(directory)) != NULL) {

        if (!is_numeric(entry->d_name))
            continue;

        if (count >= MAX_PROCESSES)
            break;

        pid_t pid =
            (pid_t)strtol(entry->d_name, NULL, 10);

        if (pid <= 0)
            continue;

        /*
           A process can disappear between reading /proc
           and opening its files. That is a normal race.
        */
        if (read_process(pid,
                         &processes[count],
                         total_cpu,
                         cpu_count)) {

            count++;
        }
    }

    closedir(directory);

    qsort(processes,
          count,
          sizeof(ProcessInfo),
          compare_processes);

    return count;
}

static int selected_index(ProcessInfo *processes,
                          int count)
{
    if (count <= 0)
        return -1;

    for (int i = 0; i < count; i++) {
        if (processes[i].pid == selected_pid)
            return i;
    }

    selected_pid = processes[0].pid;

    return 0;
}

static void send_signal_to_selected(int signal_number,
                                    ProcessInfo *processes,
                                    int count)
{
    int index =
        selected_index(processes, count);

    if (index < 0) {
        set_status("No process is available");
        return;
    }

    pid_t pid = processes[index].pid;

    /*
       Safety protection.
       Do not allow accidental control of PID 1
       or ForgeOS itself.
    */
    if (pid <= 1) {
        set_status("Safety: PID %d is protected", pid);
        return;
    }

    if (pid == getpid()) {
        set_status("Safety: cannot control ForgeOS itself");
        return;
    }

    if (kill(pid, signal_number) == -1) {

        if (errno == EPERM)
            set_status("PID %d: permission denied", pid);

        else if (errno == ESRCH)
            set_status("PID %d: process no longer exists", pid);

        else
            set_status("PID %d: %s",
                       pid,
                       strerror(errno));

        return;
    }

    set_status("Signal %d sent to PID %d (%s)",
               signal_number,
               pid,
               processes[index].name);
}

static void launch_demo_child(void)
{
    if (demo_child_pid > 0 &&
        kill(demo_child_pid, 0) == 0) {

        set_status("Demo child already exists: PID %d",
                   demo_child_pid);
        return;
    }

    pid_t pid = fork();

    if (pid < 0) {
        set_status("fork() failed: %s",
                   strerror(errno));
        return;
    }

    if (pid == 0) {
        execlp("sleep",
               "sleep",
               "300",
               (char *)NULL);

        _exit(127);
    }

    demo_child_pid = pid;

    set_status("Created child PID %d using fork()+exec()",
               pid);
}

static void reap_children(void)
{
    int status;
    pid_t child;

    while ((child =
            waitpid(-1,
                    &status,
                    WNOHANG)) > 0) {

        if (child == demo_child_pid) {

            if (WIFEXITED(status)) {
                set_status(
                    "Child PID %d exited normally, status=%d",
                    child,
                    WEXITSTATUS(status));
            }

            else if (WIFSIGNALED(status)) {
                set_status(
                    "Child PID %d terminated by signal %d",
                    child,
                    WTERMSIG(status));
            }

            demo_child_pid = -1;
        }
    }
}

static void draw_ui(ProcessInfo *processes,
                     int count,
                     MemoryInfo *memory,
                     long cpu_count,
                     struct rusage *usage)
{
    erase();

    attron(A_BOLD);

    mvprintw(
        0,
        2,
        "FORGEOS - Linux Process Monitoring and Control System");

    attroff(A_BOLD);

    const char *sort_name =
        sort_mode == SORT_CPU ? "CPU" :
        sort_mode == SORT_MEMORY ? "Memory" :
        "PID";

    mvprintw(
        1,
        2,
        "Processes: %d | CPU Cores: %ld | Refresh: 1 second",
        count,
        cpu_count);

    mvprintw(
        2,
        2,
        "Sort: %s | Selected PID: %d | Demo Child: %s",
        sort_name,
        selected_pid > 0 ? selected_pid : -1,
        demo_child_pid > 0 ? "ACTIVE" : "none");

    if (memory->total_kb > 0) {

        double used_mb =
            memory->used_kb / 1024.0;

        double total_mb =
            memory->total_kb / 1024.0;

        double swap_used_mb =
            (memory->swap_total_kb -
             memory->swap_free_kb) / 1024.0;

        double swap_total_mb =
            memory->swap_total_kb / 1024.0;

        mvprintw(
            3,
            2,
            "System Memory: %.1f / %.1f MB | Swap: %.1f / %.1f MB",
            used_mb,
            total_mb,
            swap_used_mb,
            swap_total_mb);
    }

    mvprintw(
        5,
        2,
        "%-2s %-7s %-24s %-11s %8s %12s",
        "",
        "PID",
        "NAME",
        "STATE",
        "CPU %",
        "RSS");

    mvhline(6, 2, '-', 78);

    int visible_rows = LINES - 14;

    if (visible_rows < 1)
        visible_rows = 1;

    int index =
        selected_index(processes, count);

    if (index < scroll_offset)
        scroll_offset = index;

    if (index >= scroll_offset + visible_rows)
        scroll_offset =
            index - visible_rows + 1;

    if (scroll_offset < 0)
        scroll_offset = 0;

    for (int row = 0;
         row < visible_rows;
         row++) {

        int i = scroll_offset + row;

        if (i >= count)
            break;

        if (i == index)
            attron(A_REVERSE);

        mvprintw(
            7 + row,
            2,
            "%-2s %-7d %-24.24s %-11.11s %7.2f%% %9ld KB",
            i == index ? ">" : "",
            processes[i].pid,
            processes[i].name,
            state_text(processes[i].state),
            processes[i].cpu_percent,
            processes[i].rss_kb);

        if (i == index)
            attroff(A_REVERSE);
    }

    attron(A_BOLD);
    mvprintw(LINES - 7, 2, "CONTROLS");
    attroff(A_BOLD);

    mvprintw(
        LINES - 6,
        2,
        "UP/DOWN: Select | C: CPU | M: Memory | P: PID");

    mvprintw(
        LINES - 5,
        2,
        "S: STOP | R: RESUME | T: TERM | K: KILL");

    mvprintw(
        LINES - 4,
        2,
        "L: Launch demo child | Q: Quit");

    mvprintw(
        LINES - 3,
        2,
        "Status: %.110s",
        status_message);

    double user_seconds =
        usage->ru_utime.tv_sec +
        usage->ru_utime.tv_usec / 1000000.0;

    double system_seconds =
        usage->ru_stime.tv_sec +
        usage->ru_stime.tv_usec / 1000000.0;

    mvprintw(
        LINES - 2,
        2,
        "ForgeOS usage: user %.2fs | system %.2fs | max RSS %ld KB",
        user_seconds,
        system_seconds,
        usage->ru_maxrss);

    mvprintw(
        LINES - 1,
        2,
        "Linux/WSL processes visible through /proc");

    refresh();
}

int main(void)
{
    ProcessInfo *processes =
        calloc(MAX_PROCESSES,
               sizeof(ProcessInfo));

    if (processes == NULL) {
        perror("calloc");
        return 1;
    }

    long cpu_count =
        sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count < 1)
        cpu_count = 1;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    /*
       200 ms keyboard timeout lets the dashboard
       refresh while still responding to keys.
    */
    timeout(200);

    unsigned long long total_cpu =
        read_total_cpu_ticks();

    previous_total_cpu = total_cpu;

    int running = 1;

    while (running) {

        reap_children();

        total_cpu =
            read_total_cpu_ticks();

        MemoryInfo memory;

        if (!read_memory_info(&memory))
            memset(&memory, 0, sizeof(memory));

        int process_count =
            scan_processes(processes,
                           total_cpu,
                           cpu_count);

        struct rusage usage;

        getrusage(RUSAGE_SELF, &usage);

        draw_ui(processes,
                process_count,
                &memory,
                cpu_count,
                &usage);

        int key = getch();

        if (key != ERR) {

            switch (key) {

                case 'q':
                case 'Q':
                    running = 0;
                    break;

                case KEY_UP: {
                    int index =
                        selected_index(processes,
                                       process_count);

                    if (index > 0)
                        selected_pid =
                            processes[index - 1].pid;

                    break;
                }

                case KEY_DOWN: {
                    int index =
                        selected_index(processes,
                                       process_count);

                    if (index >= 0 &&
                        index < process_count - 1)
                        selected_pid =
                            processes[index + 1].pid;

                    break;
                }

                case KEY_NPAGE:
                    scroll_offset += 10;
                    break;

                case KEY_PPAGE:
                    scroll_offset -= 10;

                    if (scroll_offset < 0)
                        scroll_offset = 0;

                    break;

                case 'c':
                case 'C':
                    sort_mode = SORT_CPU;
                    scroll_offset = 0;
                    break;

                case 'm':
                case 'M':
                    sort_mode = SORT_MEMORY;
                    scroll_offset = 0;
                    break;

                case 'p':
                case 'P':
                    sort_mode = SORT_PID;
                    scroll_offset = 0;
                    break;

                case 's':
                case 'S':
                    send_signal_to_selected(
                        SIGSTOP,
                        processes,
                        process_count);
                    break;

                case 'r':
                case 'R':
                    send_signal_to_selected(
                        SIGCONT,
                        processes,
                        process_count);
                    break;

                case 't':
                case 'T':
                    send_signal_to_selected(
                        SIGTERM,
                        processes,
                        process_count);
                    break;

                case 'k':
                case 'K':
                    send_signal_to_selected(
                        SIGKILL,
                        processes,
                        process_count);
                    break;

                case 'l':
                case 'L':
                    launch_demo_child();
                    break;

                default:
                    break;
            }
        }

        previous_total_cpu = total_cpu;
    }

    endwin();

    struct rusage final_usage;

    getrusage(RUSAGE_SELF,
              &final_usage);

    printf("\nForgeOS stopped.\n");

    printf(
        "User CPU time: %ld.%06ld seconds\n",
        final_usage.ru_utime.tv_sec,
        final_usage.ru_utime.tv_usec);

    printf(
        "System CPU time: %ld.%06ld seconds\n",
        final_usage.ru_stime.tv_sec,
        final_usage.ru_stime.tv_usec);

    printf(
        "Maximum resident memory: %ld KB\n",
        final_usage.ru_maxrss);

    free(processes);

    return 0;
}
