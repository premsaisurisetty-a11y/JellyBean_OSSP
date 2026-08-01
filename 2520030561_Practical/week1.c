#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main() {
    char command[100];
    pid_t pid;

    // 1. Accept a Linux command as input (e.g., ls, date, pwd)
    printf("Enter a basic Linux command: ");
    scanf("%99s", command);

    // 2. Create a child process using fork()
    pid = fork();

    if (pid < 0) {
        // Fork failed
        perror("Fork failed");
        exit(1);
    } else if (pid == 0) {
        // --- CHILD PROCESS ---
        
        // 5. Display the Process ID (PID) of the child process
        printf("[Child Process] My PID is: %d\n", getpid());
        printf("[Child Process] Executing the command...\n\n");

        // 3. Execute the command in the child process using execlp()
        // execlp automatically searches the system's PATH for the command
        execlp(command, command, (char *)NULL);

        // If execlp succeeds, this next line is NEVER reached.
        // If it fails (e.g., you type a command that doesn't exist), it prints an error.
        perror("Execution failed");
        exit(1);
    } else {
        // --- PARENT PROCESS ---
        
        // 5. Display the Process ID (PID) of the parent process
        printf("[Parent Process] My PID is: %d\n", getpid());
        printf("[Parent Process] Waiting for the child to finish...\n");

        // 4. Allow the parent process to wait for the child using wait()
        wait(NULL);
        
        printf("\n[Parent Process] Child process has completed. Exiting.\n");
    }

    return 0;
}
