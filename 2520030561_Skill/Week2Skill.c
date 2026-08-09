#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_INPUT 1024
#define MAX_ARGS 64

/* Enable normal terminal mode */
void enableRawMode(struct termios *old)
{
    struct termios new;

    tcgetattr(STDIN_FILENO, old);
    new = *old;

    new.c_lflag &= ~(ICANON | ECHO);

    tcsetattr(STDIN_FILENO, TCSANOW, &new);
}

/* Restore normal terminal mode */
void disableRawMode(struct termios *old)
{
    tcsetattr(STDIN_FILENO, TCSANOW, old);
}

/* Read keyboard input character by character */
void readInput(char *input)
{
    struct termios old;
    char ch;
    int index = 0;

    enableRawMode(&old);

    while (index < MAX_INPUT - 1)
    {
        read(STDIN_FILENO, &ch, 1);

        /* Enter key */
        if (ch == '\n' || ch == '\r')
        {
            break;
        }

        /* Backspace key */
        if (ch == 127 || ch == 8)
        {
            if (index > 0)
            {
                index--;

                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        /* Normal character */
        if (ch >= 32 && ch <= 126)
        {
            input[index++] = ch;
            printf("%c", ch);
            fflush(stdout);
        }
    }

    input[index] = '\0';

    disableRawMode(&old);

    printf("\n");
}

int main()
{
    char input[MAX_INPUT];
    char *args[MAX_ARGS];

    printf("========================================\n");
    printf("        SIMPLE LINUX SHELL\n");
    printf("========================================\n");
    printf("Type 'exit' to quit.\n");

    while (1)
    {
        /* Display prompt */
        printf("\nMyShell> ");
        fflush(stdout);

        /* Read keyboard input */
        readInput(input);

        /* Handle empty input */
        if (strlen(input) == 0)
        {
            continue;
        }

        /* Handle exit command */
        if (strcmp(input, "exit") == 0)
        {
            printf("Exiting MyShell...\n");
            break;
        }

        /* Split input into arguments */
        int i = 0;

        args[i] = strtok(input, " ");

        while (args[i] != NULL && i < MAX_ARGS - 1)
        {
            i++;
            args[i] = strtok(NULL, " ");
        }

        args[i] = NULL;

        /* Create child process */
        pid_t pid = fork();

        if (pid < 0)
        {
            perror("fork");
            continue;
        }

        /* Child process */
        if (pid == 0)
        {
            execvp(args[0], args);

            /* Runs only if execvp fails */
            perror("execvp");
            exit(EXIT_FAILURE);
        }

        /* Parent process */
        else
        {
            wait(NULL);
        }
    }

    return 0;
}
