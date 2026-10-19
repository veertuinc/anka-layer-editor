#pragma once

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/*
 * Run child processes. The child ignores SIGINT and SIGQUIT, so Ctrl-C does
 * not stop a command that changes a disk half way.
 */

typedef struct {
    const char *const *argv;       /* argv[0] is looked up on PATH */
    const char        *cwd;        /* or NULL */
    const char        *env_assign; /* "NAME=value" added to the environment, or NULL */
    int                stdin_fd;   /* or -1 for /dev/null */
} proc_spec;

/* Run to the end. stdout_text and stderr_text get what the child wrote
 * (NUL-terminated; the caller frees them). Returns the exit status, or -1
 * with err set when the command cannot run or a signal stops it. */
int proc_run(const proc_spec *spec, char **stdout_text, char **stderr_text,
             char *err, size_t errlen);

typedef struct {
    pid_t pid;
    int   stdout_fd;          /* read end of the child's stdout */
    FILE *stderr_capture;
    char  command_name[64];
} proc_stream;

/* Start the child with its stdout on a pipe. The caller reads stdout_fd and
 * closes it (or gives it to a reader that closes it) before
 * proc_stream_finish. Returns 0, or -1 with err set. */
int proc_stream_start(const proc_spec *spec, proc_stream *stream,
                      char *err, size_t errlen);

/* Wait for the child. stderr_text is as for proc_run. Returns the exit status,
 * or -1 with err set. */
int proc_stream_finish(proc_stream *stream, char **stderr_text,
                       char *err, size_t errlen);
