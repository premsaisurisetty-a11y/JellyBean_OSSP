#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_INPUT 1024
#define MAX_ARGS 64

int main()
{
    char input[MAX_INPUT];
    char *args[MAX_ARGS];

    printf("=====================================\n");
    printf("      Simple Linux Shell\n");
    printf(" Type 'exit' to quit the shell\n");
    printf("=====================================\n");

    while (1)
    {
        // Display shell prompt
        printf("\nMyShell> ");
        fflush(stdout);

        // Read user input
        if (fgets(input, sizeof(input), stdin) == NULL)
        {
            printf("\n");
            break;
        }

        // Remove newline character
        input[strcspn(input, "\n")] = '\0';

        // Ignore empty input
        if (strlen(input) == 0)
            continue;

        // Exit condition
        if (strcmp(input, "exit") == 0)
        {
            printf("Exiting MyShell...\n");
            break;
        }

        // Split input into arguments
        int i = 0;
        args[i] = strtok(input, " ");

        while (args[i] != NULL && i < MAX_ARGS - 1)
        {
            i++;
            args[i] = strtok(NULL, " ");
        }

        args[i] = NULL;

        // Create child process
        pid_t pid = fork();

        if (pid < 0)
        {
            perror("Fork Failed");
            continue;
        }

        // Child Process
        if (pid == 0)
        {
            printf("\n----- Child Process -----\n");
            printf("Child PID  : %d\n", getpid());
            printf("Parent PID : %d\n\n", getppid());

            execvp(args[0], args);

            perror("Command Execution Failed");
            exit(EXIT_FAILURE);
        }

        // Parent Process
        else
        {
            printf("\n----- Parent Process -----\n");
            printf("Parent PID : %d\n", getpid());
            printf("Waiting for Child Process...\n");

            wait(NULL);

            printf("Child Process Completed.\n");
        }
    }

    return 0;
}
