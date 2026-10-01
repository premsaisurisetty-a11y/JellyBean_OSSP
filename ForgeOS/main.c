
#define _GNU_SOURCE
#include <ncurses.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PROCESSES 512
#define NAME_LEN 64

typedef struct {
    pid_t pid;
    char name[NAME_LEN];
    char state[16];
    double cpu;
    long rss_kb;
} ProcessInfo;

typedef struct {
    ProcessInfo items[MAX_PROCESSES];
    int count;
} ProcessTable;

static ProcessTable table;
static sem_t table_sem;
static volatile int running = 1;
static int sort_mode = 0; /* 0 PID, 1 CPU, 2 MEM */
static int selected = 0;
static pid_t demo_pid = -1;
static char status_msg[160] = "Ready. Press H for help.";
static pthread_t monitor_tid;

/* ---------- /proc parsing ---------- */

static int is_number_string(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; ++s) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static int read_rss(pid_t pid, long *rss_kb) {
    char path[128], line[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 7) == 0) {
            long v = 0;
            if (sscanf(line + 7, "%ld", &v) == 1) {
                *rss_kb = v;
                fclose(fp);
                return 0;
            }
        }
    }
    fclose(fp);
    return -1;
}

static int read_stat(pid_t pid, char *name, size_t n, char *state,
                     double *cpu_percent, unsigned long long *total_ticks,
                     unsigned long long *starttime) {
    char path[128], buf[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    char *lp = strchr(buf, '(');
    char *rp = strrchr(buf, ')');
    if (!lp || !rp || rp <= lp) return -1;

    size_t name_len = (size_t)(rp - lp - 1);
    if (name_len >= n) name_len = n - 1;
    memcpy(name, lp + 1, name_len);
    name[name_len] = '\0';

    char rest[4096];
    snprintf(rest, sizeof(rest), "%s", rp + 2); /* skip ") " */
    char *save = NULL;
    char *tok = strtok_r(rest, " ", &save);
    if (!tok) return -1;
    *state = tok[0];

    unsigned long long utime = 0, stime = 0, st = 0;
    /* after state, field 4 is ppid. utime is field 14 => 11 tokens later */
    for (int field = 4; field <= 22; ++field) {
        tok = strtok_r(NULL, " ", &save);
        if (!tok) return -1;
        if (field == 14) utime = strtoull(tok, NULL, 10);
        else if (field == 15) stime = strtoull(tok, NULL, 10);
        else if (field == 22) st = strtoull(tok, NULL, 10);
    }
    *total_ticks = utime + stime;
    *starttime = st;

    /* CPU is filled by monitor thread using previous samples. */
    *cpu_percent = 0.0;
    return 0;
}

static int read_system_cpu_ticks(unsigned long long *ticks) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;
    char line[512];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    unsigned long long user=0,nice=0,sys=0,idle=0,iowait=0,irq=0,softirq=0,steal=0;
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user,&nice,&sys,&idle,&iowait,&irq,&softirq,&steal) < 4) return -1;
    *ticks = user+nice+sys+idle+iowait+irq+softirq+steal;
    return 0;
}

typedef struct {
    pid_t pid;
    unsigned long long proc_ticks;
} PrevCPU;

static PrevCPU prev[MAX_PROCESSES];
static int prev_count = 0;

static double calculate_cpu(pid_t pid, unsigned long long now_ticks,
                            unsigned long long previous_sys,
                            unsigned long long current_sys) {
    for (int i = 0; i < prev_count; ++i) {
        if (prev[i].pid == pid) {
            double result = 0.0;
            if (current_sys > previous_sys && now_ticks >= prev[i].proc_ticks) {
                result = ((double)(now_ticks - prev[i].proc_ticks) /
                          (double)(current_sys - previous_sys)) * 100.0;
            }
            prev[i].proc_ticks = now_ticks;
            return result;
        }
    }
    if (prev_count < MAX_PROCESSES) {
        prev[prev_count].pid = pid;
        prev[prev_count].proc_ticks = now_ticks;
        prev_count++;
    }
    return 0.0;
}

static void sort_table(ProcessInfo *a, int n) {
    for (int i = 1; i < n; ++i) {
        ProcessInfo key = a[i];
        int j = i - 1;
        int move = 0;
        while (j >= 0) {
            if (sort_mode == 1) move = a[j].cpu < key.cpu;
            else if (sort_mode == 2) move = a[j].rss_kb < key.rss_kb;
            else move = a[j].pid > key.pid;
            if (!move) break;
            a[j+1] = a[j];
            --j;
        }
        a[j+1] = key;
    }
}

static int scan_processes(ProcessTable *out) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    out->count = 0;
    struct dirent *ent;
    unsigned long long sys_ticks = 0;
    read_system_cpu_ticks(&sys_ticks);
    static unsigned long long previous_sys_ticks = 0;
    unsigned long long old_sys_ticks = previous_sys_ticks;
    previous_sys_ticks = sys_ticks;

    while ((ent = readdir(dir)) != NULL && out->count < MAX_PROCESSES) {
        if (!is_number_string(ent->d_name)) continue;
        pid_t pid = (pid_t)strtol(ent->d_name, NULL, 10);

        ProcessInfo p;
        memset(&p, 0, sizeof(p));
        p.pid = pid;
        unsigned long long proc_ticks=0, starttime=0;
        char state='?';
        if (read_stat(pid, p.name, sizeof(p.name), &state, &p.cpu,
                      &proc_ticks, &starttime) != 0) continue;
        (void)starttime;
        p.state[0] = '\0';
        switch (state) {
            case 'R': snprintf(p.state,sizeof(p.state),"Running"); break;
            case 'S': snprintf(p.state,sizeof(p.state),"Sleeping"); break;
            case 'D': snprintf(p.state,sizeof(p.state),"Disk Sleep"); break;
            case 'T': snprintf(p.state,sizeof(p.state),"Stopped"); break;
            case 'Z': snprintf(p.state,sizeof(p.state),"Zombie"); break;
            case 'I': snprintf(p.state,sizeof(p.state),"Idle"); break;
            case 'X': snprintf(p.state,sizeof(p.state),"Dead"); break;
            default: snprintf(p.state,sizeof(p.state),"Other"); break;
        }
        read_rss(pid, &p.rss_kb);
        p.cpu = calculate_cpu(pid, proc_ticks, old_sys_ticks, sys_ticks);
        out->items[out->count++] = p;
    }
    closedir(dir);
    sort_table(out->items, out->count);
    return 0;
}

static void *monitor_thread(void *arg) {
    (void)arg;
    while (running) {
        ProcessTable local;
        memset(&local, 0, sizeof(local));
        if (scan_processes(&local) == 0) {
            sem_wait(&table_sem);
            table = local;
            sem_post(&table_sem);
        }
        sleep(1);
    }
    return NULL;
}

/* ---------- process control ---------- */

static const char *signal_name(int sig) {
    switch(sig) {
        case SIGSTOP: return "SIGSTOP";
        case SIGCONT: return "SIGCONT";
        case SIGTERM: return "SIGTERM";
        case SIGKILL: return "SIGKILL";
        default: return "signal";
    }
}

static void send_selected_signal(int sig) {
    ProcessInfo p;
    int ok = 0;
    sem_wait(&table_sem);
    if (selected >= 0 && selected < table.count) {
        p = table.items[selected];
        ok = 1;
    }
    sem_post(&table_sem);
    if (!ok) {
        snprintf(status_msg,sizeof(status_msg),"No process selected.");
        return;
    }
    if (p.pid == getpid()) {
        snprintf(status_msg,sizeof(status_msg),"Safety: ForgeOS will not signal itself.");
        return;
    }
    if (kill(p.pid, sig) == 0)
        snprintf(status_msg,sizeof(status_msg),"%s sent to PID %d.", signal_name(sig), p.pid);
    else
        snprintf(status_msg,sizeof(status_msg),"PID %d: %s", p.pid, strerror(errno));
}

static void launch_demo(void) {
    if (demo_pid > 0) {
        if (kill(demo_pid, 0) == 0) {
            snprintf(status_msg,sizeof(status_msg),"Demo process PID %d already exists.", demo_pid);
            return;
        }
        demo_pid = -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(status_msg,sizeof(status_msg),"fork(): %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        execlp("sleep", "sleep", "300", (char *)NULL);
        _exit(127);
    }
    demo_pid = pid;
    snprintf(status_msg,sizeof(status_msg),"Demo child created with PID %d.", pid);
}

static void reap_demo(void) {
    if (demo_pid > 0) {
        int st;
        pid_t r = waitpid(demo_pid, &st, WNOHANG);
        if (r == demo_pid) {
            snprintf(status_msg,sizeof(status_msg),"Demo PID %d has exited and was reaped.", demo_pid);
            demo_pid = -1;
        }
    }
}

/* ---------- getrusage ---------- */

static void show_usage(WINDOW *w) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return;
    int row = 2;
    mvwprintw(w,row++,2,"ForgeOS Resource Usage (RUSAGE_SELF)");
    mvwprintw(w,row++,2,"User CPU time   : %ld.%06ld s",ru.ru_utime.tv_sec,ru.ru_utime.tv_usec);
    mvwprintw(w,row++,2,"System CPU time : %ld.%06ld s",ru.ru_stime.tv_sec,ru.ru_stime.tv_usec);
    mvwprintw(w,row++,2,"Max RSS         : %ld KB",ru.ru_maxrss);
    mvwprintw(w,row++,2,"Voluntary ctx   : %ld",ru.ru_nvcsw);
    mvwprintw(w,row++,2,"Involuntary ctx : %ld",ru.ru_nivcsw);
    mvwprintw(w,row+1,2,"Press any key to return.");
    wrefresh(w);
    wgetch(w);
}

/* ---------- Memory management: FIFO/LRU ---------- */

static int contains(int *frames, int n, int page) {
    for (int i=0;i<n;i++) if (frames[i]==page) return i;
    return -1;
}

static int fifo_faults(const int *ref, int len, int nf) {
    int *frames = calloc(nf, sizeof(int));
    for(int i=0;i<nf;i++) frames[i]=-1;
    int next=0, faults=0;
    for(int i=0;i<len;i++) {
        if(contains(frames,nf,ref[i])<0) {
            frames[next]=ref[i]; next=(next+1)%nf; faults++;
        }
    }
    free(frames); return faults;
}

static int lru_faults(const int *ref, int len, int nf) {
    int *frames = calloc(nf,sizeof(int));
    int *age = calloc(nf,sizeof(int));
    for(int i=0;i<nf;i++){frames[i]=-1; age[i]=0;}
    int faults=0, tick=0;
    for(int i=0;i<len;i++) {
        tick++;
        int pos=contains(frames,nf,ref[i]);
        if(pos>=0) age[pos]=tick;
        else {
            faults++;
            int victim=0;
            for(int j=0;j<nf;j++) if(frames[j]==-1){victim=j;break;}
            if(frames[victim]!=-1) for(int j=1;j<nf;j++) if(age[j]<age[victim]) victim=j;
            frames[victim]=ref[i]; age[victim]=tick;
        }
    }
    free(frames); free(age); return faults;
}

static void memory_module(void) {
    clear();
    mvprintw(1,2,"FORGEOS - Memory Management Module");
    mvprintw(3,2,"This module demonstrates FIFO and LRU page replacement.");
    mvprintw(5,2,"Reference string example: 7 0 1 2 0 3 0 4");
    mvprintw(6,2,"Frames: 3");
    mvprintw(8,2,"1. Run default FIFO + LRU simulation");
    mvprintw(9,2,"2. Enter your own reference string");
    mvprintw(10,2,"Q. Back");
    refresh();

    int ch=getch();
    if(ch=='q'||ch=='Q') return;

    int ref[64]={7,0,1,2,0,3,0,4};
    int len=8, frames=3;
    if(ch=='2') {
        echo();
        char input[256];
        mvprintw(12,2,"Enter pages separated by spaces (max 20): ");
        getnstr(input,sizeof(input)-1);
        noecho();
        len=0;
        char *save=NULL, *tok=strtok_r(input," ,",&save);
        while(tok && len<64){ref[len++]=atoi(tok); tok=strtok_r(NULL," ,",&save);}
        mvprintw(13,2,"Enter number of frames (2-6): ");
        echo(); char fbuf[16]; getnstr(fbuf,sizeof(fbuf)-1); noecho();
        frames=atoi(fbuf); if(frames<2)frames=2; if(frames>6)frames=6;
    }
    int ff=fifo_faults(ref,len,frames), ll=lru_faults(ref,len,frames);
    mvprintw(15,2,"Results:");
    mvprintw(16,4,"FIFO page faults: %d",ff);
    mvprintw(17,4,"LRU  page faults: %d",ll);
    mvprintw(19,2,"Press any key to return.");
    refresh(); getch();
}

/* ---------- IPC demo: pipe + fork + waitpid ---------- */

static void ipc_demo(void) {
    clear();
    mvprintw(1,2,"FORGEOS - IPC Demonstration");
    mvprintw(3,2,"Demonstrates pipe(), fork(), write(), read(), and waitpid().");
    refresh();

    int p2c[2] = {-1, -1}, c2p[2] = {-1, -1};
    if (pipe(p2c) < 0 || pipe(c2p) < 0) {
        mvprintw(5,2,"pipe(): %s", strerror(errno));
        if (p2c[0] >= 0) close(p2c[0]);
        if (p2c[1] >= 0) close(p2c[1]);
        if (c2p[0] >= 0) close(c2p[0]);
        if (c2p[1] >= 0) close(c2p[1]);
        refresh(); getch(); return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        mvprintw(5,2,"fork(): %s", strerror(errno));
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        refresh(); getch(); return;
    }

    if (pid == 0) {
        close(p2c[1]);
        close(c2p[0]);
        char buf[128] = {0};
        ssize_t n = read(p2c[0], buf, sizeof(buf)-1);
        close(p2c[0]);
        if (n > 0) {
            const char *ack = "Child received the message through pipe.";
            write(c2p[1], ack, strlen(ack)+1);
        }
        close(c2p[1]);
        _exit(0);
    }

    close(p2c[0]);
    close(c2p[1]);
    const char *msg = "Hello from ForgeOS parent through a POSIX pipe!";
    write(p2c[1], msg, strlen(msg)+1);
    close(p2c[1]);

    char ack[128] = {0};
    read(c2p[0], ack, sizeof(ack)-1);
    close(c2p[0]);

    int st = 0;
    waitpid(pid, &st, 0);

    mvprintw(5,2,"Parent PID: %d", getpid());
    mvprintw(6,2,"Child PID : %d", pid);
    mvprintw(8,2,"Parent sent : %s", msg);
    mvprintw(9,2,"Child reply : %s", ack[0] ? ack : "No reply");
    mvprintw(11,2,"waitpid() collected the child successfully.");
    mvprintw(13,2,"Press any key to return.");
    refresh(); getch();
}

/* ---------- UI ---------- */

static void draw_help(void) {
    clear();
    mvprintw(1,2,"FORGEOS - Controls");
    mvprintw(3,2,"Up/Down : select process");
    mvprintw(4,2,"C       : sort by CPU");
    mvprintw(5,2,"M       : sort by memory");
    mvprintw(6,2,"P       : sort by PID");
    mvprintw(7,2,"S       : SIGSTOP");
    mvprintw(8,2,"R       : SIGCONT");
    mvprintw(9,2,"T       : SIGTERM");
    mvprintw(10,2,"K       : SIGKILL");
    mvprintw(11,2,"L       : launch controlled demo child");
    mvprintw(12,2,"V       : memory management (FIFO/LRU)");
    mvprintw(13,2,"I       : IPC pipe demonstration");
    mvprintw(14,2,"U       : ForgeOS getrusage() information");
    mvprintw(15,2,"H       : this help");
    mvprintw(16,2,"Q       : quit");
    mvprintw(18,2,"Press any key to return.");
    refresh(); getch();
}

static void draw_ui(void) {
    erase();
    int rows, cols;
    getmaxyx(stdscr,rows,cols);
    sem_wait(&table_sem);
    ProcessTable local=table;
    sem_post(&table_sem);

    mvprintw(0,2,"FORGEOS - Linux Process Monitoring and Control System");
    mvprintw(1,2,"Processes: %d | CPU Cores: %ld | Sort: %s",
             local.count,sysconf(_SC_NPROCESSORS_ONLN),
             sort_mode==1?"CPU":(sort_mode==2?"MEMORY":"PID"));
    mvprintw(3,2,"PID     NAME                         STATE          CPU %%       MEMORY");
    mvprintw(4,2,"--------------------------------------------------------------------------");

    int visible=rows-9;
    if(visible<1) visible=1;
    if(selected>=local.count) selected=local.count-1;
    if(selected<0) selected=0;

    int start=0;
    if(selected>=visible) start=selected-visible+1;
    for(int i=0;i<visible && start+i<local.count;i++){
        int idx=start+i, y=5+i;
        ProcessInfo *p=&local.items[idx];
        if(idx==selected) attron(A_REVERSE);
        mvprintw(y,2,"%-7d %-28.28s %-14.14s %7.2f%% %10ld KB",
                 p->pid,p->name,p->state,p->cpu,p->rss_kb);
        if(idx==selected) attroff(A_REVERSE);
    }

    int by=rows-4;
    if(by<6) by=6;
    mvprintw(by,2,"Controls: Up/Down  C:CPU  M:Memory  P:PID  S:Stop  R:Resume  T:Terminate  K:Kill");
    mvprintw(by+1,2,"L:Demo  V:Memory  I:IPC  U:Usage  H:Help  Q:Quit");
    mvprintw(by+2,2,"Status: %.100s",status_msg);
    mvprintw(by+3,2,"Monitoring thread active | /proc scan every 1 second | semaphore protected");
    if(cols<90) mvprintw(by+3,2,"Resize terminal for full dashboard.");
    refresh();
}

int main(void) {
    if (sem_init(&table_sem,0,1)!=0) {
        fprintf(stderr,"sem_init failed: %s\n",strerror(errno));
        return 1;
    }
    memset(&table,0,sizeof(table));
    if(pthread_create(&monitor_tid,NULL,monitor_thread,NULL)!=0){
        fprintf(stderr,"pthread_create failed\n");
        sem_destroy(&table_sem);
        return 1;
    }

    initscr();
    cbreak();
    noecho();
    keypad(stdscr,TRUE);
    timeout(200);
    curs_set(0);

    while(running) {
        reap_demo();
        draw_ui();
        int ch=getch();
        switch(ch){
            case KEY_UP: selected--; break;
            case KEY_DOWN: selected++; break;
            case 'c': case 'C': sort_mode=1; break;
            case 'm': case 'M': sort_mode=2; break;
            case 'p': case 'P': sort_mode=0; break;
            case 's': case 'S': send_selected_signal(SIGSTOP); break;
            case 'r': case 'R': send_selected_signal(SIGCONT); break;
            case 't': case 'T': send_selected_signal(SIGTERM); break;
            case 'k': case 'K': send_selected_signal(SIGKILL); break;
            case 'l': case 'L': launch_demo(); break;
            case 'v': case 'V': timeout(-1); memory_module(); timeout(200); break;
            case 'i': case 'I': timeout(-1); ipc_demo(); timeout(200); break;
            case 'u': case 'U': timeout(-1); show_usage(stdscr); timeout(200); break;
            case 'h': case 'H': timeout(-1); draw_help(); timeout(200); break;
            case 'q': case 'Q': running=0; break;
            default: break;
        }
    }

    running=0;
    pthread_join(monitor_tid,NULL);
    endwin();

    if(demo_pid>0){
        if(kill(demo_pid,SIGTERM)==0) {
            waitpid(demo_pid,NULL,0);
        }
    }
    sem_destroy(&table_sem);
    printf("ForgeOS exited cleanly.\n");
    return 0;
}
