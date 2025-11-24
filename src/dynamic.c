/*
 * myshell_no_readline.c -- Advanced POSIX-style shell (NO READLINE)
 *
 * Converted from a readline-based version to use getline().
 * - Persistent history file (~/.myshell_history) via simple append.
 * - Keeps variable expansion, command substitution, pipes, redirection,
 *   job control, builtins (cd, exit, pwd, mkdir, touch, history, jobs, fg, bg, kill).
 *
 * Compile:
 *   gcc -std=gnu11 -Wall -Wextra -o myshell_no_readline myshell_no_readline.c
 *
 * Author: ChatGPT (modified to remove readline)
 */

#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <pwd.h>
#include <stdint.h>

#define MAX_TOKENS 512
#define MAX_JOBS 128
#define HISTORY_FILE ".myshell_history"
#define HISTORY_MAX 50000
#define MAX_LINE_LEN 16384

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } job_state_t;

typedef struct job {
    int id;
    pid_t pgid;
    char *cmdline;
    job_state_t state;
} job_t;

static job_t jobs[MAX_JOBS];
static int next_job_id = 1;
static int job_count = 0;

static pid_t shell_pgid;
static int shell_terminal;

/* history */
static char *history_arr[HISTORY_MAX];
static int history_count = 0;
static char histpath_global[4096] = {0};

/* forward */
static void init_shell(void);
static void sigchld_handler(int sig);
static void install_signal_handlers(void);
static char *run_command_capture(const char *cmd);
static char *expand_variables_and_subst(const char *input);
static char **tokenize(const char *s, int *argc_out);
static int is_builtin(const char *cmd);
static int run_builtin(char **argv);
static int execute_pipeline(char **cmds, int ncmds, const char *fullcmd, int background);
static void add_job(pid_t pgid, const char *cmdline, job_state_t state);
static job_t *find_job_by_pgid(pid_t pgid);
static job_t *find_job_by_id(int id);
static void remove_job(job_t *j);
static void print_jobs(void);

/* helpers */
static char *xstrdup(const char *s) { if (!s) return NULL; return strdup(s); }

/* --- History helpers --- */
static void load_history_file(const char *histpath) {
    FILE *f = fopen(histpath, "r");
    if (!f) return;
    char *line = NULL;
    size_t n = 0;
    while (getline(&line, &n, f) != -1) {
        // strip newline
        size_t L = strlen(line);
        if (L && (line[L-1]=='\n' || line[L-1]=='\r')) line[L-1] = '\0';
        if (history_count < HISTORY_MAX) history_arr[history_count++] = strdup(line);
    }
    free(line);
    fclose(f);
}
static void append_history_file(const char *histpath, const char *line) {
    FILE *f = fopen(histpath, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}
static void add_history_inmem_and_file(const char *line) {
    if (!line || !*line) return;
    if (history_count < HISTORY_MAX) history_arr[history_count++] = strdup(line);
    else {
        // rotate: free oldest, move everything left by one (rare)
        free(history_arr[0]);
        memmove(history_arr, history_arr+1, sizeof(char*)*(HISTORY_MAX-1));
        history_arr[HISTORY_MAX-1] = strdup(line);
    }
    if (histpath_global[0]) append_history_file(histpath_global, line);
}

/* --- Job management --- */
static void add_job(pid_t pgid, const char *cmdline, job_state_t state) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].id == 0) {
            jobs[i].id = next_job_id++;
            jobs[i].pgid = pgid;
            jobs[i].cmdline = xstrdup(cmdline);
            jobs[i].state = state;
            job_count++;
            return;
        }
    }
    fprintf(stderr, "jobs: table full\n");
}

static job_t *find_job_by_pgid(pid_t pgid) {
    for (int i = 0; i < MAX_JOBS; ++i) if (jobs[i].id && jobs[i].pgid == pgid) return &jobs[i];
    return NULL;
}
static job_t *find_job_by_id(int id) {
    for (int i = 0; i < MAX_JOBS; ++i) if (jobs[i].id == id) return &jobs[i];
    return NULL;
}
static void remove_job(job_t *j) {
    if (!j) return;
    free(j->cmdline);
    j->cmdline = NULL;
    j->id = 0;
    j->pgid = 0;
    j->state = JOB_DONE;
    job_count--;
}
static void print_jobs(void) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].id) {
            printf("[%d] %d ", jobs[i].id, (int)jobs[i].pgid);
            if (jobs[i].state == JOB_RUNNING) printf("Running ");
            else if (jobs[i].state == JOB_STOPPED) printf("Stopped ");
            else printf("Done ");
            printf("%s\n", jobs[i].cmdline);
        }
    }
}

/* --- Signal handling --- */
static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        pid_t pgid = getpgid(pid);
        job_t *j = find_job_by_pgid(pgid);
        if (!j) continue;
        if (WIFSTOPPED(status)) {
            j->state = JOB_STOPPED;
            fprintf(stderr, "\n[%d]+ Stopped\t%s\n", j->id, j->cmdline);
        } else if (WIFCONTINUED(status)) {
            j->state = JOB_RUNNING;
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            j->state = JOB_DONE;
            fprintf(stderr, "\n[%d]+ Done\t%s\n", j->id, j->cmdline);
            remove_job(j);
        }
    }
}

static void sigint_handler(int sig) { (void)sig; /* shell ignores; children receive */ }
static void sigtstp_handler(int sig) { (void)sig; }

static void install_signal_handlers(void) {
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    signal(SIGINT, SIG_IGN);   /* shell ignores Ctrl-C */
    signal(SIGTSTP, SIG_IGN);  /* shell ignores Ctrl-Z */
}

/* --- Helpers: run command capture (for command substitution) --- */
static char *run_command_capture(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return xstrdup("");
    size_t cap = 4096;
    char *out = malloc(cap);
    size_t len = 0;
    while (1) {
        if (len + 1024 >= cap) { cap *= 2; out = realloc(out, cap); }
        size_t r = fread(out + len, 1, 1024, fp);
        if (r == 0) break;
        len += r;
    }
    if (len==0) out[0]='\0'; else out[len]='\0';
    pclose(fp);
    while (len>0 && (out[len-1]=='\n' || out[len-1]=='\r')) { out[len-1]='\0'; len--; }
    return out;
}

/* Expand $VAR ${VAR} and simple command substitution $(...) and `...` (single-pass). */
static char *expand_variables_and_subst(const char *input) {
    size_t cap = strlen(input) + 1;
    char *out = malloc(cap);
    size_t oi = 0;
    for (size_t i = 0; input[i]; ++i) {
        if (input[i] == '\\') {
            if (input[i+1]) { if (oi+2>cap) { cap*=2; out=realloc(out,cap);} out[oi++]=input[i+1]; i++; }
        } else if (input[i] == '\'') {
            ++i;
            while (input[i] && input[i] != '\'') {
                if (oi+2>cap){cap*=2; out=realloc(out,cap);} out[oi++]=input[i++];
            }
            // closing quote skip handled by loop
        } else if (input[i]=='"') {
            ++i;
            while (input[i] && input[i] != '"') {
                if (input[i] == '$') {
                    size_t j = i+1;
                    if (input[j]=='{') { j++; size_t start=j; while (input[j] && input[j] != '}') j++; char *name = strndup(input+start, j-start); char *val = getenv(name); free(name); if (!val) val=""; size_t L=strlen(val); if (oi+L+1>cap){cap=cap+L+1024; out=realloc(out,cap);} memcpy(out+oi, val, L); oi+=L; if (input[j]=='}') i=j; else i=j-1; }
                    else { size_t start=j; while (input[j] && (isalnum((unsigned char)input[j]) || input[j]=='_')) j++; char *name = strndup(input+start, j-start); char *val = getenv(name); free(name); if (!val) val=""; size_t L=strlen(val); if (oi+L+1>cap){cap=cap+L+1024; out=realloc(out,cap);} memcpy(out+oi, val, L); oi+=L; i=j-1; }
                } else if (input[i]=='$' && input[i+1]=='(') {
                    size_t j=i+2; int depth=1;
                    while (input[j] && depth>0) { if (input[j]=='(') depth++; else if (input[j]==')') depth--; j++; }
                    char *inner = strndup(input + i + 2, j - (i+2) - 1 + 1);
                    char *res = run_command_capture(inner);
                    free(inner);
                    size_t L=strlen(res);
                    if (oi+L+1>cap){cap=cap+L+1024; out=realloc(out,cap);} memcpy(out+oi,res,L); oi+=L; free(res);
                    i = j-1;
                } else if (input[i]=='`') {
                    size_t j=i+1; while (input[j] && input[j] != '`') j++;
                    char *inner = strndup(input+i+1, j - (i+1));
                    char *res = run_command_capture(inner);
                    free(inner);
                    size_t L=strlen(res);
                    if (oi+L+1>cap){cap=cap+L+1024; out=realloc(out,cap);} memcpy(out+oi,res,L); oi+=L; free(res);
                    i = (input[j] ? j : j-1);
                } else {
                    if (oi+2>cap){cap*=2; out=realloc(out,cap);} out[oi++]=input[i++];
                    i--;
                }
            }
        } else if (input[i] == '$') {
            if (input[i+1] == '{') {
                size_t j = i+2; while (input[j] && input[j] != '}') j++; char *name = strndup(input+i+2, j-(i+2)); char *val = getenv(name); free(name); if (!val) val=""; size_t L=strlen(val); if (oi+L+1>cap){cap=cap+L+1024; out=realloc(out,cap);} memcpy(out+oi, val, L); oi+=L; i = (input[j] ? j : j-1);
            } else if (input[i+1] == '(') {
                size_t j=i+2; int depth=1; while (input[j] && depth>0) { if (input[j]=='(') depth++; else if (input[j]==')') depth--; j++; }
                char *inner = strndup(input + i + 2, j - (i+2) - 1 + 1);
                char *res = run_command_capture(inner);
                free(inner);
                size_t L = strlen(res);
                if (oi+L+1>cap){cap=cap+L+1024; out=realloc(out,cap);} memcpy(out+oi,res,L); oi+=L; free(res);
                i = j-1;
            } else {
                size_t j = i+1; while (input[j] && (isalnum((unsigned char)input[j]) || input[j]=='_')) j++;
                if (j == i+1) { out[oi++]='$'; } else {
                    char *name = strndup(input+i+1, j-(i+1)); char *val = getenv(name); free(name); if (!val) val=""; size_t L=strlen(val); if (oi+L+1>cap){cap=cap+L+1024; out=realloc(out,cap);} memcpy(out+oi,val,L); oi+=L; i=j-1;
                }
            }
        } else if (input[i]=='`') {
            size_t j=i+1; while (input[j] && input[j] != '`') j++;
            char *inner = strndup(input+i+1, j-(i+1)); char *res = run_command_capture(inner); free(inner);
            size_t L=strlen(res); if (oi+L+1>cap){cap=cap+L+1024; out=realloc(out,cap);} memcpy(out+oi,res,L); oi+=L; free(res); i = (input[j] ? j : j-1);
        } else {
            if (oi+2>cap) { cap *= 2; out = realloc(out, cap); }
            out[oi++] = input[i];
        }
    }
    out[oi] = '\0';
    return out;
}

/* --- Tokenize by whitespace respecting quotes and escapes --- */
static char **tokenize(const char *s, int *argc_out) {
    char **argv = calloc(MAX_TOKENS, sizeof(char*));
    int argc = 0;
    size_t i = 0, n = strlen(s);
    while (i < n) {
        while (i < n && isspace((unsigned char)s[i])) i++;
        if (i >= n) break;
        char buf[4096]; size_t bi = 0;
        if (s[i] == '"') {
            i++;
            while (i < n && s[i] != '"') {
                if (bi < MAX_TOKENS - 1) buf[bi++] = s[i++];
                else break; // prevent overflow
            }
            if (i < n && s[i] == '"') i++;
        }
        else if (s[i] == '\'') {
            i++;
            while (i < n && s[i] != '\'') {
                buf[bi++] = s[i++];
            }
            if (i < n && s[i] == '\'') i++;
        } else {
            while (i < n && !isspace((unsigned char)s[i])) {
                buf[bi++] = s[i++];
            }
        }
        buf[bi] = '\0';
        argv[argc++] = strdup(buf);
    }
    argv[argc] = NULL;
    if (argc_out) *argc_out = argc;
    return argv;
}

/* Check builtin */
static int is_builtin(const char *cmd) {
    if (!cmd) return 0;
    const char *b[] = {"cd","exit","pwd","mkdir","touch","history","jobs","fg","bg","kill", NULL};
    for (int i=0;b[i];++i) if (strcmp(cmd,b[i])==0) return 1;
    return 0;
}

/* Run builtin in shell process when appropriate */
static int run_builtin(char **argv) {
    if (!argv || !argv[0]) return 0;
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = argv[1] ? argv[1] : getenv("HOME");
        if (!dir) dir = "/";
        if (chdir(dir) < 0) perror("cd");
        return 1;
    } else if (strcmp(argv[0], "pwd") == 0) {
        char cwd[4096]; if (getcwd(cwd,sizeof(cwd))) puts(cwd); else perror("pwd"); return 1;
    } else if (strcmp(argv[0], "exit") == 0) {
        int code = argv[1] ? atoi(argv[1]) : 0; exit(code);
    } else if (strcmp(argv[0], "mkdir") == 0) { if (!argv[1]) { fprintf(stderr,"mkdir: missing operand\n"); return 1;} if (mkdir(argv[1],0755)<0) perror("mkdir"); return 1; }
    else if (strcmp(argv[0], "touch") == 0) { if (!argv[1]) { fprintf(stderr,"touch: missing operand\n"); return 1;} int fd=open(argv[1],O_CREAT|O_WRONLY,0644); if (fd<0) perror("touch"); else close(fd); return 1; }
    else if (strcmp(argv[0], "history") == 0) { for (int i=0;i<history_count;i++) printf("%4d  %s\n", i+1, history_arr[i]); return 1; }
    else if (strcmp(argv[0], "jobs") == 0) { print_jobs(); return 1; }
    else if (strcmp(argv[0], "fg") == 0 || strcmp(argv[0], "bg") == 0) {
        int bg = (strcmp(argv[0], "bg") == 0);
        int jid = 0;
        if (argv[1]) jid = atoi(argv[1]);
        if (!jid) {
            // pick last job
            int last = -1; for (int i=0;i<MAX_JOBS;i++) if (jobs[i].id) last=i;
            if (last==-1) { fprintf(stderr, "fg/bg: no jobs\n"); return 1; }
            jid = jobs[last].id;
        }
        job_t *j = find_job_by_id(jid);
        if (!j) { fprintf(stderr, "fg/bg: job %d not found\n", jid); return 1; }
        if (bg) {
            if (kill(-j->pgid, SIGCONT) < 0) perror("kill (SIGCONT)"); else j->state = JOB_RUNNING;
        } else {
            if (tcsetpgrp(shell_terminal, j->pgid) < 0) perror("tcsetpgrp");
            if (kill(-j->pgid, SIGCONT) < 0) perror("kill (SIGCONT)");
            j->state = JOB_RUNNING;
            int status; waitpid(-j->pgid, &status, WUNTRACED);
            tcsetpgrp(shell_terminal, shell_pgid);
        }
        return 1;
    } else if (strcmp(argv[0], "kill") == 0) {
        if (!argv[1]) { fprintf(stderr, "kill: usage: kill [-SIGNAL] pid|%%job\n"); return 1; }
        int sig = SIGTERM;
        char *target = argv[1];
        if (target[0]=='-' && isdigit((unsigned char)target[1])) { sig = atoi(target+1); if (argv[2]) target = argv[2]; else { fprintf(stderr, "kill: missing target\n"); return 1; } }
        if (target[0]=='%') {
            int jid = atoi(target+1);
            job_t *j = find_job_by_id(jid);
            if (!j) { fprintf(stderr, "kill: no such job %s\n", target); return 1; }
            if (kill(-j->pgid, sig) < 0) perror("kill");
        } else {
            pid_t pid = (pid_t)atoi(target);
            if (pid<=0) { fprintf(stderr, "kill: invalid pid\n"); return 1; }
            if (kill(pid, sig) < 0) perror("kill");
        }
        return 1;
    }
    return 0;
}

/* --- Execution: parse commands, handle redirection, pipes, background --- */

/* Split line by pipes, respecting quotes */
static char **split_pipes(const char *line, int *count) {
    char **parts = calloc(MAX_TOKENS, sizeof(char*));
    int np = 0;
    size_t len = strlen(line);
    size_t start = 0;
    int in_sq = 0, in_dq = 0;
    for (size_t i=0;i<=len;i++) {
        char c = line[i];
        if (c == '\0' || (c == '|' && !in_sq && !in_dq)) {
            size_t partlen = i - start;
            // trim
            while (partlen>0 && isspace((unsigned char)line[start])) { start++; partlen--; }
            while (partlen>0 && isspace((unsigned char)line[start+partlen-1])) partlen--;
            char *part = strndup(line+start, partlen);
            parts[np++] = part;
            start = i+1;
        } else if (c == '\'') in_sq = !in_sq;
        else if (c == '"') in_dq = !in_dq;
    }
    parts[np] = NULL;
    *count = np;
    return parts;
}

static int execute_pipeline(char **cmds, int ncmds, const char *fullcmd, int background) {
    int prev_fd = -1;
    int pipefd[2];
    pid_t pgid = 0;

    for (int i = 0; i < ncmds; ++i) {
        char *expanded = expand_variables_and_subst(cmds[i]);
        int argc = 0;
        char **argv = tokenize(expanded, &argc);
        free(expanded);
        if (argc == 0) { free(argv); continue; }

        int in_fd = -1, out_fd = -1;
        char **cleanargv = calloc(argc+1, sizeof(char*));
        int ci = 0;
        for (int j=0;j<argc;++j) {
            if (strcmp(argv[j], "<") == 0) {
                if (argv[j+1]) { in_fd = open(argv[j+1], O_RDONLY); if (in_fd<0) perror("open"); j++; }
                else { fprintf(stderr, "syntax error near '<'\n"); }
            } else if (strcmp(argv[j], ">") == 0) {
                if (argv[j+1]) { out_fd = open(argv[j+1], O_WRONLY|O_CREAT|O_TRUNC, 0644); if (out_fd<0) perror("open"); j++; }
                else { fprintf(stderr, "syntax error near '>'\n"); }
            } else if (strcmp(argv[j], ">>") == 0) {
                if (argv[j+1]) { out_fd = open(argv[j+1], O_WRONLY|O_CREAT|O_APPEND, 0644); if (out_fd<0) perror("open"); j++; }
                else { fprintf(stderr, "syntax error near '>>'\n"); }
            } else { cleanargv[ci++] = argv[j]; }
        }
        cleanargv[ci]=NULL;

        if (i < ncmds-1) { if (pipe(pipefd) < 0) { perror("pipe"); return -1; } }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return -1; }
        else if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            if (pgid == 0) pgid = getpid();
            setpgid(0, pgid);

            if (!background) tcsetpgrp(shell_terminal, pgid);

            if (prev_fd != -1) { dup2(prev_fd, STDIN_FILENO); close(prev_fd); }
            else if (in_fd != -1) { dup2(in_fd, STDIN_FILENO); close(in_fd); }

            if (i < ncmds-1) { close(pipefd[0]); dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]); }
            else if (out_fd != -1) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }

            if (is_builtin(cleanargv[0])) {
                if (strcmp(cleanargv[0], "pwd") == 0 || strcmp(cleanargv[0], "mkdir") == 0 || strcmp(cleanargv[0], "touch") == 0 || strcmp(cleanargv[0], "history") == 0 || strcmp(cleanargv[0], "jobs") == 0) {
                    run_builtin(cleanargv);
                    _exit(0);
                }
            }

            execvp(cleanargv[0], cleanargv);
            fprintf(stderr, "%s: %s\n", cleanargv[0], strerror(errno));
            _exit(127);
        } else {
            if (pgid == 0) pgid = pid;
            setpgid(pid, pgid);

            if (prev_fd != -1) close(prev_fd);
            if (i < ncmds-1) { close(pipefd[1]); prev_fd = pipefd[0]; }
            else prev_fd = -1;

            if (in_fd != -1) close(in_fd);
            if (out_fd != -1) close(out_fd);

            free(cleanargv);
            free(argv);
        }
    }

    if (background) {
        add_job(pgid, fullcmd, JOB_RUNNING);
        job_t *j = find_job_by_pgid(pgid);
        if (j) printf("[%d] %d\n", j->id, (int)pgid);
    } else {
        if (tcsetpgrp(shell_terminal, pgid) < 0) perror("tcsetpgrp");
        int status;
        pid_t w;
        do {
            w = waitpid(-pgid, &status, WUNTRACED);
            if (w == -1 && errno != EINTR) break;
            if (WIFSTOPPED(status)) {
                add_job(pgid, fullcmd, JOB_STOPPED);
                break;
            }
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
        tcsetpgrp(shell_terminal, shell_pgid);
    }

    return 0;
}

static void init_shell(void) {
    shell_terminal = STDIN_FILENO;

    /* Ignore terminal stop signals */
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    /* Put shell in its own process group */
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0) {
        perror("setpgid");
        exit(1);
    }

    /* Take control of the terminal */
    if (tcsetpgrp(shell_terminal, shell_pgid) < 0) {
        perror("tcsetpgrp");
        exit(1);
    }

    /* Install handlers AFTER taking terminal */
    install_signal_handlers();
}


/* --- Main REPL --- */
int main(int argc, char **argv) {
    init_shell();

    /* prepare history path and load history */
    const char *homedir = getenv("HOME");
    if (!homedir) homedir = getpwuid(getuid())->pw_dir;
    snprintf(histpath_global, sizeof(histpath_global), "%s/%s", homedir, HISTORY_FILE);
    load_history_file(histpath_global);

    while (1) {
        char cwd[4096]; if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
        char prompt[512]; snprintf(prompt, sizeof(prompt), "\033[1;32mmyshell\033[0m:\033[1;34m%s\033[0m$ ", cwd);
        // print prompt and read line using getline
        printf("%s", prompt);
        fflush(stdout);

        char *line = NULL;
        size_t len = 0;
        ssize_t nread = getline(&line, &len, stdin);
        if (nread == -1) {
            if (feof(stdin)) { printf("\n"); break; }
            free(line);
            continue;
        }
        // trim newline
        if (nread>0 && line[nread-1]=='\n') line[nread-1] = '\0';
        char *trim = line;
        while (*trim && isspace((unsigned char)*trim)) trim++;
        if (*trim == '\0') { free(line); continue; }

        add_history_inmem_and_file(trim);

        // detect background '&' at end
        int background = 0;
        size_t L = strlen(trim);
        while (L>0 && isspace((unsigned char)trim[L-1])) L--;
        if (L>0 && trim[L-1]=='&') { background = 1; trim[L-1] = '\0'; while (L>0 && isspace((unsigned char)trim[L-1])) { trim[L-1]='\0'; L--; } }

        // split by pipes
        int ncmds = 0;
        char **parts = split_pipes(trim, &ncmds);

        // single builtin that affects shell: run directly
        if (ncmds == 1) {
            int argc2 = 0;
            char *expanded = expand_variables_and_subst(parts[0]);
            char **tmp = tokenize(expanded, &argc2);
            free(expanded);
            if (argc2 > 0 && is_builtin(tmp[0])) {
                if (strcmp(tmp[0], "cd")==0 || strcmp(tmp[0], "exit")==0) {
                    run_builtin(tmp);
                    for (int i=0;i<argc2;i++) free(tmp[i]); free(tmp);
                    free(parts[0]); free(parts); free(line);
                    continue;
                }
            }
            for (int i=0;i<argc2;i++) free(tmp[i]); free(tmp);
        }

        execute_pipeline(parts, ncmds, trim, background);

        for (int i=0;i<ncmds;i++) free(parts[i]); free(parts); free(line);
    }

    /* cleanup history memory */
    for (int i=0;i<history_count;i++) free(history_arr[i]);
    return 0;
}
