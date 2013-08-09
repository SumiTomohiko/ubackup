#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void
pipe_or_die(int fds[2])
{
    if (pipe(fds) != 0) {
        err(1, "pipe(2) failed");
    }
}

static void
close_or_die(int d)
{
    if (close(d) != 0) {
        err(1, "close(2) for %d failed", d);
    }
}

static void
dup2_or_die(int oldd, int newd)
{
    if (dup2(oldd, newd) == -1) {
        err(1, "dup2(2) failed: oldd=%d, newd=%d", oldd, newd);
    }
}

#define R   0
#define W   1

static void
exec_backuper(const char* hostname, const char* destdir, const int er2ee[2], const int ee2er[2])
{
    close_or_die(er2ee[R]);
    close_or_die(ee2er[W]);
    dup2_or_die(ee2er[R], fileno(stdin));
    dup2_or_die(er2ee[W], fileno(stdout));

    const char* cmd = "ssh";
    execlp(cmd, cmd, hostname, "ubackuper", destdir, NULL);
    err(1, "cannot execute ubackuper: host=%s, dir=%s", hostname, destdir);
}

int
main(int argc, char* argv[])
{
    int er2ee[2];   /* backuper to backupee */
    pipe_or_die(er2ee);
    int ee2er[2];   /* backupee to backuper */
    pipe_or_die(ee2er);

    pid_t pid = fork();
    switch (pid) {
    case -1:
        err(1, "fork(2) failed");
        break;
    case 0:
        exec_backuper(argv[1], argv[argc - 1], er2ee, ee2er);
        break;
    default:
        break;
    }

    close_or_die(ee2er[R]);
    close_or_die(er2ee[W]);
    dup2_or_die(er2ee[R], fileno(stdin));
    dup2_or_die(ee2er[W], fileno(stdout));

    int nargs = argc - 1;
    char** args = (char**)alloca(sizeof(char*) * nargs);
    args[0] = "ubackupee";
    int i;
    for (i = 1; i < nargs - 1; i++) {
        args[i] = argv[i + 1];
    }
    args[nargs - 1] = NULL;

    execvp(args[0], args);
    err(1, "cannot execute ubackupee");

    return 1;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
