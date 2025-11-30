// File: src/main.c
#define _POSIX_C_SOURCE 200809L
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
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <stdbool.h>

#define MAX_LINE 1024
#define MAX_ARGS 128
#define MAX_HISTORY 1000
#define HISTORY_FILE "/home/okasha/myshell_history"
#define MAX_JOBS 128
#define JOB_CMDLEN 512

// Terminal settings
struct termios orig_termios;

// History storage
char *history[MAX_HISTORY];
int history_count = 0;

// Job states
typedef enum { JOB_RUNNING=0, JOB_STOPPED=1, JOB_DONE=2 } job_state_t;
typedef struct {
    int id;
    pid_t pgid;
    job_state_t state;
    char cmd[JOB_CMDLEN];
} job_t;

job_t jobs[MAX_JOBS];
int job_count = 0;
int next_job_id = 1;

// Shell pgid and foreground pgid
pid_t shell_pgid;
volatile pid_t fg_pgid = 0;

// -------------------- Utility: jobs --------------------
void job_add(pid_t pgid, job_state_t state, const char *cmd) {
    if (job_count >= MAX_JOBS) return;
    jobs[job_count].id = next_job_id++;
    jobs[job_count].pgid = pgid;
    jobs[job_count].state = state;
    strncpy(jobs[job_count].cmd, cmd ? cmd : "", JOB_CMDLEN - 1);
    jobs[job_count].cmd[JOB_CMDLEN - 1] = '\0';
    job_count++;
}

int job_find_index_by_pgid(pid_t pgid) {
    for (int i = 0; i < job_count; ++i)
        if (jobs[i].pgid == pgid) return i;
    return -1;
}

int job_find_index_by_id(int id) {
    for (int i = 0; i < job_count; ++i)
        if (jobs[i].id == id) return i;
    return -1;
}

void job_remove_index(int idx) {
    if (idx < 0 || idx >= job_count) return;
    for (int i = idx; i + 1 < job_count; ++i) jobs[i] = jobs[i+1];
    job_count--;
}

void job_mark_state(pid_t pgid, job_state_t state) {
    int i = job_find_index_by_pgid(pgid);
    if (i >= 0) jobs[i].state = state;
}

void print_jobs() {
    for (int i = 0; i < job_count; ++i) {
        const char *st = jobs[i].state == JOB_RUNNING ? "Running" :
                         jobs[i].state == JOB_STOPPED ? "Stopped" : "Done";
        printf("[%d] %s\t%s\n", jobs[i].id, st, jobs[i].cmd);
    }
}

// -------------------- History --------------------
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

void save_history(const char *line) {
    FILE *f = fopen(HISTORY_FILE, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
    if (history_count < MAX_HISTORY)
        history[history_count++] = strdup(line);
}

// -------------------- Terminal / signals --------------------
void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

// We'll reap and update job states in SIGCHLD
void sigchld_handler(int signo) {
    int saved_errno = errno;
    pid_t pid;
    int status;
    // Use waitpid in loop to catch all children state changes
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        pid_t pgid = getpgid(pid);
        if (pgid <= 0) continue;
        int idx = job_find_index_by_pgid(pgid);
        if (WIFSTOPPED(status)) {
            if (idx >= 0) jobs[idx].state = JOB_STOPPED;
            if (fg_pgid == pgid) fg_pgid = 0;
        } else if (WIFCONTINUED(status)) {
            if (idx >= 0) jobs[idx].state = JOB_RUNNING;
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (idx >= 0) jobs[idx].state = JOB_DONE;
            if (fg_pgid == pgid) fg_pgid = 0;
        }
    }
    errno = saved_errno;
}

void install_signal_handlers_for_shell(void) {
    // Ignore interactive signals in the shell — children will get default
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

// -------------------- Line reader (raw mode, simple history) --------------------
void read_line(char *buffer) {
    int pos = 0, len = 0;
    int c;
    int history_index = history_count;
    buffer[0] = '\0';

    while (1) {
        c = getchar();
        if (c == EOF) { buffer[0] = '\0'; return; }
        if (c == '\n') {
            buffer[len] = '\0';
            printf("\n");
            break;
        } else if (c == 127 || c == 8) { // backspace
            if (pos > 0) {
                for (int i = pos - 1; i < len - 1; i++) buffer[i] = buffer[i + 1];
                pos--; len--;
                buffer[len] = '\0';
                printf("\b");
                printf("%s ", buffer + pos);
                for (int i = 0; i <= len - pos; i++) printf("\b");
            }
        } else if (c == 27) { // escape
            if ((c = getchar()) == 91) {
                c = getchar();
                if (c == 'A') { // up
                    if (history_index > 0) {
                        for (int i = 0; i < len; i++) printf("\b \b");
                        history_index--;
                        strcpy(buffer, history[history_index]);
                        len = strlen(buffer);
                        pos = len;
                        printf("%s", buffer);
                    }
                } else if (c == 'B') { // down
                    for (int i = 0; i < len; i++) printf("\b \b");
                    if (history_index < history_count - 1) {
                        history_index++;
                        strcpy(buffer, history[history_index]);
                        len = strlen(buffer);
                        pos = len;
                        printf("%s", buffer);
                    } else {
                        buffer[0] = '\0'; len = 0; pos = 0;
                    }
                } else if (c == 'C') { if (pos < len) { printf("\033[C"); pos++; } }
                else if (c == 'D') { if (pos > 0) { printf("\033[D"); pos--; } }
                else if (c == '3') { if (getchar() == 126) { // delete
                    if (pos < len) {
                        for (int i = pos; i < len - 1; i++) buffer[i] = buffer[i+1];
                        len--;
                        buffer[len] = '\0';
                        printf("%s ", buffer + pos);
                        for (int i = 0; i <= len - pos; i++) printf("\b");
                    }
                } }
            }
        } else {
            for (int i = len; i > pos; i--) buffer[i] = buffer[i-1];
            buffer[pos] = c;
            printf("%c", c);
            pos++; len++;
            buffer[len] = '\0';
            if (pos < len) {
                printf("%s", buffer + pos);
                for (int i = 0; i < len - pos; i++) printf("\b");
            }
        }
    }
}

// -------------------- Parsing --------------------
typedef struct {
    char *argv[MAX_ARGS];
    char *infile;
    char *outfile;
    int append;
} cmd_t;

void cmd_init(cmd_t *c) {
    for (int i = 0; i < MAX_ARGS; ++i) c->argv[i] = NULL;
    c->infile = NULL; c->outfile = NULL; c->append = 0;
}

static char *strdup_range(const char *s, int start, int len) {
    char *r = malloc(len + 1);
    memcpy(r, s + start, len);
    r[len] = '\0';
    return r;
}

void parse_segment(const char *seg, cmd_t *out) {
    cmd_init(out);
    int len = strlen(seg);
    int pos = 0;
    int argc = 0;
    while (pos < len) {
        while (pos < len && isspace((unsigned char)seg[pos])) pos++;
        if (pos >= len) break;
        if (seg[pos] == '"' || seg[pos] == '\'') {
            char q = seg[pos++];
            int start = pos;
            while (pos < len && seg[pos] != q) pos++;
            int sz = pos - start;
            if (pos < len && seg[pos] == q) pos++;
            out->argv[argc++] = strdup_range(seg, start, sz);
        } else if (seg[pos] == '>') {
            if (pos+1 < len && seg[pos+1] == '>') { out->append = 1; pos += 2; }
            else { out->append = 0; pos++; }
            while (pos < len && isspace((unsigned char)seg[pos])) pos++;
            if (pos < len && (seg[pos] == '"' || seg[pos] == '\'')) {
                char q = seg[pos++]; int start = pos;
                while (pos < len && seg[pos] != q) pos++;
                int sz = pos - start;
                if (pos < len && seg[pos] == q) pos++;
                out->outfile = strdup_range(seg, start, sz);
            } else {
                int start = pos;
                while (pos < len && !isspace((unsigned char)seg[pos])) pos++;
                int sz = pos - start;
                out->outfile = strdup_range(seg, start, sz);
            }
        } else if (seg[pos] == '<') {
            pos++;
            while (pos < len && isspace((unsigned char)seg[pos])) pos++;
            if (pos < len && (seg[pos] == '"' || seg[pos] == '\'')) {
                char q = seg[pos++]; int start = pos;
                while (pos < len && seg[pos] != q) pos++;
                int sz = pos - start;
                if (pos < len && seg[pos] == q) pos++;
                out->infile = strdup_range(seg, start, sz);
            } else {
                int start = pos;
                while (pos < len && !isspace((unsigned char)seg[pos])) pos++;
                int sz = pos - start;
                out->infile = strdup_range(seg, start, sz);
            }
        } else {
            int start = pos;
            while (pos < len && !isspace((unsigned char)seg[pos])) pos++;
            int sz = pos - start;
            out->argv[argc++] = strdup_range(seg, start, sz);
        }
        if (argc >= (MAX_ARGS-1)) break;
    }
    out->argv[argc] = NULL;
}

void cmd_free(cmd_t *c) {
    for (int i = 0; i < MAX_ARGS && c->argv[i]; ++i) free(c->argv[i]);
    if (c->infile) free(c->infile);
    if (c->outfile) free(c->outfile);
}

char *trim_copy(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    int end = strlen(s) - 1;
    while (end >= 0 && isspace((unsigned char)s[end])) end--;
    int len = (end >= 0) ? (end + 1) : 0;
    char *r = malloc(len + 1);
    memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

// -------------------- Builtins --------------------
int is_builtin(const char *cmd) {
    if (!cmd) return 0;
    return (strcmp(cmd, "cd") == 0 ||
            strcmp(cmd, "exit") == 0 ||
            strcmp(cmd, "history") == 0 ||
            strcmp(cmd, "jobs") == 0 ||
            strcmp(cmd, "fg") == 0 ||
            strcmp(cmd, "bg") == 0 ||
            strcmp(cmd, "echo") == 0);
}

int run_builtin(cmd_t *c, const char *rawcmd) {
    if (c->argv[0] == NULL) return 1;
    if (strcmp(c->argv[0], "cd") == 0) {
        if (!c->argv[1]) {
            fprintf(stderr, "shell: expected argument to \"cd\"\n");
        } else {
            if (chdir(c->argv[1]) != 0) perror("cd");
        }
        return 1;
    } else if (strcmp(c->argv[0], "exit") == 0) {
        exit(0);
    } else if (strcmp(c->argv[0], "history") == 0) {
        for (int i = 0; i < history_count; ++i)
            printf("%d %s\n", i + 1, history[i]);
        return 1;
    } else if (strcmp(c->argv[0], "jobs") == 0) {
        print_jobs();
        return 1;
    } else if (strcmp(c->argv[0], "fg") == 0) {
        if (!c->argv[1]) { fprintf(stderr, "fg: usage: fg %%jobid or fg jobid\n"); return 1; }
        // Accept %n or n
        const char *a = c->argv[1];
        if (a[0] == '%') a++;
        int id = atoi(a);
        int idx = job_find_index_by_id(id);
        if (idx < 0) { fprintf(stderr, "fg: job not found: %s\n", c->argv[1]); return 1; }
        pid_t pgid = jobs[idx].pgid;
        jobs[idx].state = JOB_RUNNING;
        // bring to foreground
        fg_pgid = pgid;
        tcsetpgrp(STDIN_FILENO, pgid);
        kill(-pgid, SIGCONT);
        // wait
        int status;
        while (1) {
            pid_t w = waitpid(-pgid, &status, WUNTRACED);
            if (w == -1) {
                if (errno == ECHILD) break;
                if (errno == EINTR) continue;
                break;
            }
            if (WIFSTOPPED(status)) {
                jobs[idx].state = JOB_STOPPED;
                break;
            }
        }
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        fg_pgid = 0;
        if (jobs[idx].state == JOB_DONE) job_remove_index(idx);
        return 1;
    } else if (strcmp(c->argv[0], "bg") == 0) {
        if (!c->argv[1]) { fprintf(stderr, "bg: usage: bg %%jobid or bg jobid\n"); return 1; }
        const char *a = c->argv[1];
        if (a[0] == '%') a++;
        int id = atoi(a);
        int idx = job_find_index_by_id(id);
        if (idx < 0) { fprintf(stderr, "bg: job not found: %s\n", c->argv[1]); return 1; }
        pid_t pgid = jobs[idx].pgid;
        jobs[idx].state = JOB_RUNNING;
        kill(-pgid, SIGCONT);
        return 1;
    } else if (strcmp(c->argv[0], "echo") == 0) {
        for (int i = 1; c->argv[i]; ++i) {
            if (i > 1) printf(" ");
            printf("%s", c->argv[i]);
        }
        printf("\n");
        return 1;
    }
    else if (strcmp(c->argv[0], "kill") == 0) {
        if (c->argv[1] == NULL) {
            printf("kill: usage: kill %%jobid or kill pid\n");
            return 1;
        }

        if (c->argv[1][0] == '%') {
            int jobid = atoi(c->argv[1] + 1);
            int idx = job_find_index_by_id(jobid);
            if (idx < 0) {
                printf("kill: job not found: %s\n", c->argv[1]);
                return 1;
            }
            kill(-jobs[idx].pgid, SIGTERM);   // kill entire job group
        } else {
            kill(atoi(c->argv[1]), SIGTERM);
        }
        return 1;
    }

    return 1;
}

// -------------------- Execution --------------------
// Launch a pipeline of segments (segments is array of strings, segc >=1).
// If background != 0, run in background.
void launch_pipeline(char **segments, int segc, int background, const char *fullcmd) {
    int prev_fd = -1;
    int pipefd[2];
    pid_t pgid = 0;

    for (int i = 0; i < segc; ++i) {
        cmd_t cmd;
        parse_segment(segments[i], &cmd);

        if (i < segc - 1) {
            if (pipe(pipefd) < 0) { perror("pipe"); cmd_free(&cmd); return; }
        } else {
            pipefd[0] = pipefd[1] = -1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            cmd_free(&cmd);
            return;
        } else if (pid == 0) {
            // Child process
            // Put in process group
            if (pgid == 0) pgid = getpid();
            setpgid(0, pgid);

            // If foreground, give terminal to pgid
            if (!background) tcsetpgrp(STDIN_FILENO, pgid);

            // Restore default signals in child
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            // Setup stdin from prev_fd if exists
            if (prev_fd != -1) {
                dup2(prev_fd, STDIN_FILENO);
                close(prev_fd);
            }

            // Setup stdout to pipe write if exists
            if (pipefd[1] != -1) {
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[0]);
                close(pipefd[1]);
            }

            // Handle redirections
            if (cmd.infile) {
                int fd = open(cmd.infile, O_RDONLY);
                if (fd < 0) { perror(cmd.infile); exit(1); }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (cmd.outfile) {
                int fd;
                if (cmd.append) fd = open(cmd.outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                else fd = open(cmd.outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) { perror(cmd.outfile); exit(1); }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            if (cmd.argv[0] == NULL) exit(0);
            execvp(cmd.argv[0], cmd.argv);
            // exec failed
            perror("exec");
            exit(127);
        } else {
            // Parent
            // First child becomes pgid leader
            if (pgid == 0) pgid = pid;
            setpgid(pid, pgid);

            // close parent's copies of pipe ends
            if (prev_fd != -1) close(prev_fd);
            if (pipefd[1] != -1) close(pipefd[1]);

            prev_fd = (pipefd[0] != -1) ? pipefd[0] : -1;
        }
        // free parsed strings
        // we used dynamic allocations in parse_segment: free them
        // but cannot call cmd_free here because in parent we didn't keep the struct.
        // So reparse to free? Simpler: call parse_segment again into tmp to free - but costly.
        // Better: modify parse_segment to allocate and return, and call cmd_free in both parent and child.
        // For brevity: we free by parsing into a tmp and freeing — small overhead.
        cmd_free(&cmd);
    }

    // After launching pipeline:
    if (background) {
        job_add(pgid, JOB_RUNNING, fullcmd);
        printf("[%d] %d\n", next_job_id - 1, pgid);
    } else {
        // Foreground: give terminal to pgid and wait
        fg_pgid = pgid;
        tcsetpgrp(STDIN_FILENO, pgid);

        int status;
        // Wait for any process in the process group to change state.
        // Loop waiting; on WIFSTOPPED mark job stopped and add to job list.
        while (1) {
            pid_t w = waitpid(-pgid, &status, WUNTRACED);
            if (w == -1) {
                if (errno == ECHILD) break;
                if (errno == EINTR) continue;
                break;
            }
            if (WIFSTOPPED(status)) {
                // add job as stopped
                job_add(pgid, JOB_STOPPED, fullcmd);
                break;
            }
            // keep waiting until all children done; loop continues
        }

        // Return terminal to shell
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        fg_pgid = 0;
    }
}

// -------------------- Prompt --------------------
void print_prompt() {
    char cwd[PATH_MAX];
    char hostname[HOST_NAME_MAX];
    struct passwd *pw = getpwuid(getuid());
    if (getcwd(cwd, sizeof(cwd)) == NULL) strncpy(cwd, "?", sizeof(cwd));
    gethostname(hostname, sizeof(hostname));
    printf("\033[0;32m┌──(");
    printf("\033[1;34m%s㉿%s", pw ? pw->pw_name : "user", hostname);
    printf("\033[0;32m)-[");
    printf("\033[1;37m%s\033[0m", cwd);
    printf("\033[0;32m]\n└─");
    printf("\033[1;34m$ ");
    printf("\033[0m");
    fflush(stdout);
}

// -------------------- Main --------------------
int main() {
    char line[MAX_LINE];
    char *segments[64];

    // Put shell in its own process group and take terminal
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EACCES) {
        // ignore
    }
    // Grab terminal for shell
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    install_signal_handlers_for_shell();
    enable_raw_mode();
    load_history();

    while (1) {
        printf("\n");
        print_prompt();
        read_line(line);
        if (strlen(line) == 0) continue;
        save_history(line);

        // Check background &
        int background = 0;
        int L = strlen(line);
        int p = L - 1;
        while (p >= 0 && isspace((unsigned char)line[p])) p--;
        if (p >= 0 && line[p] == '&') {
            background = 1;
            line[p] = '\0';
        }

        // Split on pipes (note: naive split, doesn't respect '|' in quotes)
        int segc = 0;
        char *s = line;
        char *saveptr = NULL;
        char *tok = strtok_r(s, "|", &saveptr);
        while (tok && segc < 64) {
            segments[segc++] = trim_copy(tok);
            tok = strtok_r(NULL, "|", &saveptr);
        }
        if (segc == 0) continue;

        // If single segment and builtin without redirection, run builtin in parent
        if (segc == 1) {
            cmd_t c;
            parse_segment(segments[0], &c);
            if (c.argv[0] && is_builtin(c.argv[0]) && !background && !c.infile && !c.outfile) {
                run_builtin(&c, line);
                cmd_free(&c);
                free(segments[0]);
                continue;
            }
            cmd_free(&c);
        }

        // Launch pipeline
        launch_pipeline(segments, segc, background, line);

        for (int i = 0; i < segc; ++i) free(segments[i]);

        // Cleanup done jobs
        for (int i = 0; i < job_count; ) {
            if (jobs[i].state == JOB_DONE) job_remove_index(i);
            else ++i;
        }
    }

    disable_raw_mode();
    return 0;
}
