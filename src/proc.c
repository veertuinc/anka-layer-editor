#include "proc.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* Descriptors the parent opens must not leak into children: a child that
 * holds the read end of its own stdout pipe never gets SIGPIPE. */
static int set_cloexec(int fd) {
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int cloexec_pipe(int fds[2]) {
    if (pipe(fds) == -1)
        return -1;
    set_cloexec(fds[0]);
    set_cloexec(fds[1]);
    return 0;
}

static void child_exec(const proc_spec *spec, int stdout_fd, int stderr_fd,
                       int exec_error_fd) {
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGPIPE, SIG_DFL);
    int stdin_fd = spec->stdin_fd >= 0 ? spec->stdin_fd
                                       : open("/dev/null", O_RDONLY);
    if (stdin_fd != -1 && dup2(stdin_fd, STDIN_FILENO) != -1 &&
        dup2(stdout_fd, STDOUT_FILENO) != -1 &&
        dup2(stderr_fd, STDERR_FILENO) != -1 &&
        (!spec->cwd || chdir(spec->cwd) == 0) &&
        (!spec->env_assign || putenv((char *)spec->env_assign) == 0))
        execvp(spec->argv[0], (char *const *)spec->argv);
    int child_errno = errno;
    ssize_t ignored = write(exec_error_fd, &child_errno, sizeof(child_errno));
    (void)ignored;
    _exit(127);
}

/* Fork and exec. An exec failure comes back through a close-on-exec pipe, so
 * "not found" is an error here and not an exit status. */
static pid_t spawn(const proc_spec *spec, int stdout_fd, int stderr_fd,
                   char *err, size_t errlen) {
    int exec_error_pipe[2];
    if (cloexec_pipe(exec_error_pipe) == -1) {
        le_seterr(err, errlen, "pipe: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid == -1) {
        le_seterr(err, errlen, "fork: %s", strerror(errno));
        close(exec_error_pipe[0]);
        close(exec_error_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        close(exec_error_pipe[0]);
        child_exec(spec, stdout_fd, stderr_fd, exec_error_pipe[1]);
    }
    close(exec_error_pipe[1]);
    int child_errno = 0;
    ssize_t got;
    do
        got = read(exec_error_pipe[0], &child_errno, sizeof(child_errno));
    while (got == -1 && errno == EINTR);
    close(exec_error_pipe[0]);
    if (got != (ssize_t)sizeof(child_errno))
        return pid;
    while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
        ;
    le_seterr(err, errlen, "%s: %s", spec->argv[0], strerror(child_errno));
    return -1;
}

static int wait_exit_status(pid_t pid, const char *command_name, char *err,
                            size_t errlen) {
    int status;
    while (waitpid(pid, &status, 0) == -1)
        if (errno != EINTR) {
            le_seterr(err, errlen, "%s: wait: %s", command_name, strerror(errno));
            return -1;
        }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    le_seterr(err, errlen, "%s: stopped by signal %d", command_name,
              WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return -1;
}

static FILE *open_capture(char *err, size_t errlen) {
    FILE *capture = tmpfile();
    if (!capture) {
        le_seterr(err, errlen, "temp file: %s", strerror(errno));
        return NULL;
    }
    set_cloexec(fileno(capture));
    return capture;
}

/* Read all of capture into a new string and close it. NULL on error. */
static char *take_capture(FILE *capture) {
    char *text = NULL;
    long size;
    if (fseek(capture, 0, SEEK_END) == 0 && (size = ftell(capture)) >= 0 &&
        fseek(capture, 0, SEEK_SET) == 0 && (text = malloc((size_t)size + 1))) {
        size_t got = fread(text, 1, (size_t)size, capture);
        text[got] = '\0';
    }
    fclose(capture);
    return text;
}

static int take_both_captures(FILE *stdout_capture, FILE *stderr_capture,
                              char **stdout_text, char **stderr_text,
                              char *err, size_t errlen) {
    *stdout_text = take_capture(stdout_capture);
    *stderr_text = take_capture(stderr_capture);
    if (*stdout_text && *stderr_text)
        return 0;
    free(*stdout_text);
    free(*stderr_text);
    *stdout_text = *stderr_text = NULL;
    le_seterr(err, errlen, "out of memory reading command output");
    return -1;
}

int proc_run(const proc_spec *spec, char **stdout_text, char **stderr_text,
             char *err, size_t errlen) {
    *stdout_text = *stderr_text = NULL;
    FILE *stdout_capture = open_capture(err, errlen);
    FILE *stderr_capture = stdout_capture ? open_capture(err, errlen) : NULL;
    if (!stderr_capture) {
        if (stdout_capture)
            fclose(stdout_capture);
        return -1;
    }
    int status = -1;
    pid_t pid = spawn(spec, fileno(stdout_capture), fileno(stderr_capture),
                      err, errlen);
    if (pid != -1)
        status = wait_exit_status(pid, spec->argv[0], err, errlen);
    if (take_both_captures(stdout_capture, stderr_capture, stdout_text,
                           stderr_text, err, errlen) == -1)
        return -1;
    return status;
}

int proc_stream_start(const proc_spec *spec, proc_stream *stream,
                      char *err, size_t errlen) {
    memset(stream, 0, sizeof(*stream));
    stream->stdout_fd = -1;
    strlcpy(stream->command_name, spec->argv[0], sizeof(stream->command_name));
    int stdout_pipe[2];
    if (cloexec_pipe(stdout_pipe) == -1) {
        le_seterr(err, errlen, "pipe: %s", strerror(errno));
        return -1;
    }
    stream->stderr_capture = open_capture(err, errlen);
    if (stream->stderr_capture)
        stream->pid = spawn(spec, stdout_pipe[1],
                            fileno(stream->stderr_capture), err, errlen);
    close(stdout_pipe[1]);
    if (!stream->stderr_capture || stream->pid == -1) {
        close(stdout_pipe[0]);
        if (stream->stderr_capture)
            fclose(stream->stderr_capture);
        stream->stderr_capture = NULL;
        return -1;
    }
    stream->stdout_fd = stdout_pipe[0];
    return 0;
}

int proc_stream_finish(proc_stream *stream, char **stderr_text,
                       char *err, size_t errlen) {
    int status = wait_exit_status(stream->pid, stream->command_name, err, errlen);
    *stderr_text = take_capture(stream->stderr_capture);
    stream->stderr_capture = NULL;
    if (!*stderr_text && status != -1) {
        le_seterr(err, errlen, "out of memory reading command output");
        return -1;
    }
    return status;
}
