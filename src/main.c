// File: src/main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define DELIM " \t\r\n\a"

// Function to split input into arguments
void parse_input(char *line, char **args) {
    int i = 0;
    args[i] = strtok(line, DELIM);
    while (args[i] != NULL && i < MAX_ARGS - 1) {
        i++;
        args[i] = strtok(NULL, DELIM);
    }
    args[i] = NULL;
}

// Built-in command: cd
int shell_cd(char **args) {
    if (args[1] == NULL) {
        fprintf(stderr, "shell: expected argument to \"cd\"\n");
    } else {
        if (chdir(args[1]) != 0) {
            perror("shell");
        }
    }
    return 1;
}

// Execute non-built-in command
int shell_execute(char **args) {
    pid_t pid, wpid;
    int status;

    pid = fork();
    if (pid == 0) {
        // Child process
        if (execvp(args[0], args) == -1) {
            perror("shell");
        }
        exit(EXIT_FAILURE);
    } else if (pid < 0) {
        perror("shell");
    } else {
        // Parent process
        do {
            wpid = waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }
    return 1;
}

// Main shell loop
int main() {
    char *line = NULL;
    size_t bufsize = 0;
    char *args[MAX_ARGS];
    int status = 1;

    while (status) {
        printf("shell> ");
        getline(&line, &bufsize, stdin);

        // Remove newline
        line[strcspn(line, "\n")] = 0;

        // Skip empty input
        if (strlen(line) == 0)
            continue;

        // Parse input
        parse_input(line, args);

        // Check for built-in commands
        if (strcmp(args[0], "cd") == 0) {
            status = shell_cd(args);
        } else if (strcmp(args[0], "exit") == 0) {
            status = 0; // Exit shell
        } else {
            status = shell_execute(args);
        }
    }

    free(line);
    return 0;
}
