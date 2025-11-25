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
#include <sys/stat.h>
#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_HISTORY 1000
#define HISTORY_FILE "/home/okasha/myshell_history"

// Terminal settings
struct termios orig_termios;

// History storage
char *history[MAX_HISTORY];
int history_count = 0;

int is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;  // path does not exist
    return S_ISDIR(st.st_mode);
}

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
    printf("\033[0;32m]\n└─");
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
void parse_input(char *input, char **args) {
    int i = 0;
    int pos = 0;
    int len = strlen(input);

    while (pos < len) {
        while (pos < len && (input[pos] == ' ' || input[pos] == '\t'))
            pos++;

        if (pos >= len) break;

        // Quoted argument
        if (input[pos] == '"' || input[pos] == '\'') {
            char quote = input[pos++];
            int start = pos;

            while (pos < len && input[pos] != quote)
                pos++;

            int size = pos - start;
            args[i] = malloc(size + 1);
            strncpy(args[i], input + start, size);
            args[i][size] = '\0';
            i++;

            if (pos < len) pos++;  // skip closing quote
        } 
        
        else {
            int start = pos;
            while (pos < len && input[pos] != ' ' && input[pos] != '\t')
                pos++;

            int size = pos - start;
            args[i] = malloc(size + 1);
            strncpy(args[i], input + start, size);
            args[i][size] = '\0';
            i++;
        }
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
// Execute external commands with I/O redirection (combined)
int shell_execute(char **args) {
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        int i = 0;
        while (args[i] != NULL) {
            if (strcmp(args[i], ">") == 0) {
                args[i] = NULL;
                FILE *f = fopen(args[i + 1], "w");
                if (!f) { perror("shell"); exit(EXIT_FAILURE); }
                dup2(fileno(f), STDOUT_FILENO);
                fclose(f);
            } else if (strcmp(args[i], ">>") == 0) {
                args[i] = NULL;
                FILE *f = fopen(args[i + 1], "a");
                if (!f) { perror("shell"); exit(EXIT_FAILURE); }
                dup2(fileno(f), STDOUT_FILENO);
                fclose(f);
            } else if (strcmp(args[i], "<") == 0) {
                args[i] = NULL;
                FILE *f = fopen(args[i + 1], "r");
                if (!f) { perror("shell"); exit(EXIT_FAILURE); }
                dup2(fileno(f), STDIN_FILENO);
                fclose(f);
            }
            i++;
        }

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
    int pos = 0;               // current cursor position
    int len = 0;               // current line length
    int c;
    int history_index = history_count;

    buffer[0] = '\0';

    while (1) {
        c = getchar();

        if (c == '\n') {
            buffer[len] = '\0';
            printf("\n");
            break;
        } 
        else if (c == 127) { // backspace
            if (pos > 0) {
                for (int i = pos - 1; i < len - 1; i++) buffer[i] = buffer[i+1];
                pos--;
                len--;
                buffer[len] = '\0';
                printf("\b"); // move left
                printf("%s ", buffer + pos); // overwrite rest of line
                for (int i = 0; i <= len - pos; i++) printf("\b"); // move cursor back
            }
        } 
        else if (c == 27) { // escape sequence
            if ((c = getchar()) == 91) { // CSI
                c = getchar();
                if (c == 'A') { // up arrow
                    if (history_index > 0) {
                        for (int i = 0; i < len; i++) printf("\b \b");
                        history_index--;
                        strcpy(buffer, history[history_index]);
                        len = strlen(buffer);
                        pos = len;
                        printf("%s", buffer);
                    }
                } 
                else if (c == 'B') { // down arrow
                    for (int i = 0; i < len; i++) printf("\b \b");
                    if (history_index < history_count - 1) {
                        history_index++;
                        strcpy(buffer, history[history_index]);
                        len = strlen(buffer);
                        pos = len;
                        printf("%s", buffer);
                    } else {
                        buffer[0] = '\0';
                        len = 0;
                        pos = 0;
                    }
                } 
                else if (c == 'C') { // right arrow
                    if (pos < len) {
                        printf("\033[C");
                        pos++;
                    }
                } 
                else if (c == 'D') { // left arrow
                    if (pos > 0) {
                        printf("\033[D");
                        pos--;
                    }
                }
            }
        } 
        else { // normal character
            for (int i = len; i > pos; i--) buffer[i] = buffer[i-1]; // shift right
            buffer[pos] = c;
            printf("%c", c);
            pos++;
            len++;
            buffer[len] = '\0';

            // redraw rest of line if not at end
            if (pos < len) {
                printf("%s", buffer + pos);
                for (int i = 0; i < len - pos; i++) printf("\b");
            }
        }
    }
}


// Main shell loop
int main() {
    char line[MAX_LINE];
    char raw_line[MAX_LINE];
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
        strcpy(raw_line, line);
        parse_input(line, args);

        if (strcmp(args[0], "cd") == 0) {
            status = shell_cd(args);
        } else if (strcmp(args[0], "exit") == 0) {
            status = 0;
        } else if (strcmp(args[0], "history") == 0) {
            for (int i = 0; i < history_count; i++)
                printf("%d %s\n", i + 1, history[i]);
        } else if (strcmp(args[0], "echo") == 0) {
            // Print everything that comes AFTER "echo" in original input
            char *p = strstr(raw_line, "echo");
            if (p) {
                p += 4; // skip the word "echo"
                while (*p == ' ') p++; // skip one space
                printf("%s\n", p);
            } else {
                printf("\n");
            }
        }else {
            if (is_directory(args[0])) {
                // Treat it as cd
                char *cd_args[2];
                cd_args[0] = "cd";
                cd_args[1] = args[0];
                cd_args[2] = NULL;
                status = shell_cd(cd_args);
            } else {
                // Execute as normal command
                status = shell_execute(args);
            }
        }

    }

    disable_raw_mode();
    return 0;
}
