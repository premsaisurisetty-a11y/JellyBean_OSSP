#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>

#define INITIAL_BUFFER_SIZE 64

/* ---------------- HISTORY NODE ---------------- */

struct HistoryNode
{
    char *command;
    struct HistoryNode *next;
};

/* ---------------- GLOBAL HISTORY ---------------- */

struct HistoryNode *head = NULL;
struct HistoryNode *tail = NULL;

int history_count = 0;

/* ---------------- TERMINAL FUNCTIONS ---------------- */

void enableRawMode(struct termios *original)
{
    struct termios raw;

    tcgetattr(STDIN_FILENO, original);

    raw = *original;

    raw.c_lflag &= ~(ICANON | ECHO);

    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void disableRawMode(struct termios *original)
{
    tcsetattr(STDIN_FILENO, TCSANOW, original);
}

/* ---------------- HISTORY FUNCTIONS ---------------- */

void addHistory(const char *command)
{
    struct HistoryNode *newNode;

    newNode = malloc(sizeof(struct HistoryNode));

    if (newNode == NULL)
    {
        perror("malloc");
        return;
    }

    newNode->command = malloc(strlen(command) + 1);

    if (newNode->command == NULL)
    {
        perror("malloc");
        free(newNode);
        return;
    }

    strcpy(newNode->command, command);

    newNode->next = NULL;

    if (head == NULL)
    {
        head = newNode;
        tail = newNode;
    }
    else
    {
        tail->next = newNode;
        tail = newNode;
    }

    history_count++;
}

/* Get history command by index */
char *getHistory(int index)
{
    struct HistoryNode *current = head;
    int i = 0;

    while (current != NULL)
    {
        if (i == index)
            return current->command;

        current = current->next;
        i++;
    }

    return NULL;
}

/* Free complete history */
void freeHistory()
{
    struct HistoryNode *current = head;
    struct HistoryNode *next;

    while (current != NULL)
    {
        next = current->next;

        free(current->command);
        free(current);

        current = next;
    }

    head = NULL;
    tail = NULL;
    history_count = 0;
}

/* ---------------- INPUT BUFFER ---------------- */

char *readInput()
{
    size_t capacity = INITIAL_BUFFER_SIZE;
    size_t length = 0;

    char *buffer = malloc(capacity);

    if (buffer == NULL)
    {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    while (1)
    {
        char ch;

        if (read(STDIN_FILENO, &ch, 1) <= 0)
        {
            break;
        }

        /* Enter key */
        if (ch == '\n' || ch == '\r')
        {
            break;
        }

        /* Backspace */
        if (ch == 127 || ch == 8)
        {
            if (length > 0)
            {
                length--;

                printf("\b \b");
                fflush(stdout);
            }

            continue;
        }

        /* Escape sequence */
        if (ch == 27)
        {
            char seq1;
            char seq2;

            if (read(STDIN_FILENO, &seq1, 1) <= 0)
                continue;

            if (read(STDIN_FILENO, &seq2, 1) <= 0)
                continue;

            /* Up Arrow */
            if (seq1 == '[' && seq2 == 'A')
            {
                continue;
            }

            /* Down Arrow */
            if (seq1 == '[' && seq2 == 'B')
            {
                continue;
            }

            continue;
        }

        /* Normal character */
        if (ch >= 32 && ch <= 126)
        {
            /* Resize buffer if required */
            if (length + 1 >= capacity)
            {
                capacity *= 2;

                char *temp = realloc(buffer, capacity);

                if (temp == NULL)
                {
                    perror("realloc");
                    free(buffer);
                    exit(EXIT_FAILURE);
                }

                buffer = temp;
            }

            buffer[length++] = ch;

            putchar(ch);
            fflush(stdout);
        }
    }

    buffer[length] = '\0';

    return buffer;
}

/* ---------------- DISPLAY HISTORY ---------------- */

void showHistory()
{
    struct HistoryNode *current = head;
    int number = 1;

    printf("\n\nCommand History:\n");

    while (current != NULL)
    {
        printf("%d  %s\n", number, current->command);

        current = current->next;
        number++;
    }
}

/* ---------------- EXECUTE COMMAND ---------------- */

void executeCommand(char *input)
{
    char *args[64];

    int i = 0;

    args[i] = strtok(input, " ");

    while (args[i] != NULL && i < 63)
    {
        i++;
        args[i] = strtok(NULL, " ");
    }

    args[i] = NULL;

    if (args[0] == NULL)
        return;

    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork");
        return;
    }

    if (pid == 0)
    {
        execvp(args[0], args);

        perror("execvp");
        exit(EXIT_FAILURE);
    }

    else
    {
        waitpid(pid, NULL, 0);
    }
}

/* ---------------- MAIN FUNCTION ---------------- */

int main()
{
    struct termios original;

    printf("============================================\n");
    printf("          SKILL 3 - SIMPLE SHELL\n");
    printf("============================================\n");

    printf("Commands supported:\n");
    printf("  ↑  Previous command\n");
    printf("  ↓  Next command\n");
    printf("  history  Show command history\n");
    printf("  exit     Exit shell\n");

    while (1)
    {
        printf("\nMyShell> ");
        fflush(stdout);

        enableRawMode(&original);

        char *input = readInput();

        disableRawMode(&original);

        printf("\n");

        /* Empty input */
        if (strlen(input) == 0)
        {
            free(input);
            continue;
        }

        /* Exit */
        if (strcmp(input, "exit") == 0)
        {
            free(input);
            break;
        }

        /* History command */
        if (strcmp(input, "history") == 0)
        {
            showHistory();

            free(input);
            continue;
        }

        /* Store command in linked-list history */
        addHistory(input);

        /* Execute command */
        executeCommand(input);

        /* Release input buffer */
        free(input);
    }

    /* Release all history memory */
    freeHistory();

    printf("Shell terminated successfully.\n");

    return 0;
}
