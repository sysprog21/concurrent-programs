#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Display prompt */
static void prompt()
{
    write(2, "$ ", 2);
}

/* Display error message, optionally - exit */
static void fatal(int retval, int leave)
{
    if (retval >= 0)
        return;
    write(2, "?\n", 2);
    if (leave)
        exit(1);
}

/* Helper functions to detect token class */
static inline int is_delim(int c)
{
    return c == 0 || c == '|';
}

static inline int is_redir(int c)
{
    return c == '>' || c == '<';
}

static inline int is_blank(int c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

static int is_special(int c)
{
    return is_delim(c) || is_redir(c) || is_blank(c);
}

/* Recursively run right-most part of the command line printing output to the
 * file descriptor @t
 */
static void run(char *c, int t)
{
    char *redir_stdin = NULL, *redir_stdout = NULL;
    int pipefds[2] = {0, 0}, outfd = 0;
    char *v[99] = {0};
    char **u = &v[98]; /* end of words */
    for (;;) {
        c--;
        if (is_delim(*c)) /* if NULL (start of string) or pipe: break */
            break;
        if (!is_special(*c)) {
            c++;    /* Copy word of regular chars into previous u */
            *c = 0; /* null-terminator */
            for (; !is_special(*--c);)
                ;
            *--u = ++c;
        }
        if (is_redir(*c)) { /* If < or > */
            if (*c == '<')
                redir_stdin = *u;
            else
                redir_stdout = *u;
            if ((u - v) != 98)
                u++;
        }
    }
    if ((u - v) == 98) /* empty input */
        return;

    if (!strcmp(*u, "cd")) { /* built-in command: cd */
        fatal(chdir(u[1]), 0);
        return; /* actually, should run() again */
    }

    if (*c) {
        pipe(pipefds);
        outfd = pipefds[1]; /* write end of the pipe */
    }

    pid_t pid = fork();
    if (pid) { /* Parent or error */
        fatal(pid, 1);
        if (outfd) {
            run(c, outfd);     /* parse the rest of the cmdline */
            close(outfd);      /* close output fd */
            close(pipefds[0]); /* close read end of the pipe */
        }
        wait(0);
        return;
    }

    if (outfd) {
        dup2(pipefds[0], 0); /* dup read fd to stdin */
        close(pipefds[0]);   /* close read fd */
        close(outfd);        /* close output */
    }

    if (redir_stdin) {
        close(0); /* replace stdin with redir_stdin */
        fatal(open(redir_stdin, 0), 1);
    }

    if (t) {
        dup2(t, 1); /* replace stdout with t */
        close(t);
    }

    if (redir_stdout) {
        close(1);
        fatal(creat(redir_stdout, 438), 1); /* replace stdout with redir */
    }
    fatal(execvp(*u, u), 1);
}

int main()
{
    while (1) {
        prompt();
        char buf[512] = {0}; /* input buffer */
        char *c = buf;
        if (!fgets(c + 1, sizeof(buf) - 1, stdin))
            exit(0);
        for (; *++c;) /* skip to end of line */
            ;
        run(c, 0);
    }
    return 0;
}
