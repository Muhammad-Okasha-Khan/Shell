// File: src/main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_HISTORY 1000
#define HISTORY_FILE "/home/okasha/myshell_history"

// Terminal settings
struct termios orig_termios;

// History storage
char *history[MAX_HISTORY];
int history_count = 0;

// Save original terminal and enable raw mode
void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON); // turn off echo and canonical mode
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Restore terminal
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void print_prompt() {
    char cwd[PATH_MAX];
    char hostname[HOST_NAME_MAX];
    struct passwd *pw = getpwuid(getuid());

    getcwd(cwd, sizeof(cwd));
    gethostname(hostname, sizeof(hostname));
    printf("\033[0;32m┌──(");               // new line with $
    printf("\033[1;34m%s㉿%s",pw->pw_name,hostname);
    printf("\033[0;32m)-[");
    printf("\033[1;37m%s\033[0m",cwd);
    printf("\033[0;32m)]\n└─");
    printf("\033[1;34m$ ");
    printf("\033[0m");

    //printf("\033[1;32mThis text is bold green\n");               // new line with $
}

// Load history from file
void load_history() {
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (history_count < MAX_HISTORY)
            history[history_count++] = strdup(line);
    }
    fclose(f);
}

// Save a line to history file
void save_history(const char *line) {
    FILE *f = fopen(HISTORY_FILE, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
    if (history_count < MAX_HISTORY)
        history[history_count++] = strdup(line);
}

// Parse input into args
void parse_input(char *line, char **args) {
    int i = 0;
    args[i] = strtok(line, " \t\r\n\a");
    while (args[i] != NULL && i < MAX_ARGS - 1) {
        i++;
        args[i] = strtok(NULL, " \t\r\n\a");
    }
    args[i] = NULL;
}

// Built-in commands
int shell_cd(char **args) {
    if (!args[1]) {
        fprintf(stderr, "shell: expected argument to \"cd\"\n");
    } else {
        if (chdir(args[1]) != 0)
            perror("shell");
    }
    return 1;
}

// Execute external commands
int shell_execute(char **args) {
    pid_t pid = fork();
    int status;
    if (pid == 0) {
        if (execvp(args[0], args) == -1)
            perror("shell");
        exit(EXIT_FAILURE);
    } else if (pid < 0) {
        perror("shell");
    } else {
        do {
            waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }
    return 1;
}

// Read line with arrow keys history
void read_line(char *buffer) {
    int pos = 0;
    int c;
    int history_index = history_count;
    char temp[MAX_LINE];

    while (1) {
        c = getchar();

        if (c == '\n') {
            buffer[pos] = '\0';
            printf("\n");
            break;
        } else if (c == 127) { // backspace
            if (pos > 0) {
                pos--;
                printf("\b \b");
            }
        } else if (c == 27) { // arrow keys
            if ((c = getchar()) == 91) {
                c = getchar();
                if (c == 'A') { // up arrow
                    if (history_index > 0) {
                        // erase current line
                        for (int i = 0; i < pos; i++) printf("\b \b");
                        history_index--;
                        strcpy(buffer, history[history_index]);
                        pos = strlen(buffer);
                        printf("%s", buffer);
                    }
                } else if (c == 'B') { // down arrow
                    if (history_index < history_count - 1) {
                        for (int i = 0; i < pos; i++) printf("\b \b");
                        history_index++;
                        strcpy(buffer, history[history_index]);
                        pos = strlen(buffer);
                        printf("%s", buffer);
                    } else {
                        for (int i = 0; i < pos; i++) printf("\b \b");
                        pos = 0;
                        buffer[0] = '\0';
                    }
                }
            }
        } else {
            buffer[pos++] = c;
            putchar(c);
        }
    }
}

// Main shell loop
int main() {
    char line[MAX_LINE];
    char *args[MAX_ARGS];
    int status = 1;

    enable_raw_mode();
    load_history();

    while (status) {
        print_prompt();
        fflush(stdout);

        read_line(line);
        if (strlen(line) == 0) continue;

        save_history(line);
        parse_input(line, args);

        if (strcmp(args[0], "cd") == 0) {
            status = shell_cd(args);
        } else if (strcmp(args[0], "exit") == 0) {
            status = 0;
        } else if (strcmp(args[0], "history") == 0) {
            for (int i = 0; i < history_count; i++)
                printf("%d %s\n", i + 1, history[i]);
        } else {
            status = shell_execute(args);
        }
    }

    disable_raw_mode();
    return 0;
}
