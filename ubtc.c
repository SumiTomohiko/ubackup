#include <assert.h>
#include <dirent.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int
recv_changed()
{
    size_t size = 4096;
    char buf[size];
    if (fgets(buf, size, stdin) == NULL) {
        perror("fgets failed");
        abort();
    }
    const char* expected = "CHANGED";
    return strncmp(expected, buf, strlen(expected));
}

static void
recv_ok()
{
    size_t size = 4096;
    char buf[size];
    if (fgets(buf, size, stdin) == NULL) {
        perror("fgets failed");
        abort();
    }
#if 0
    const char* expected = "OK";
    if (strncmp(expected, buf, strlen(expected)) != 0) {
        abort();
    }
#endif
}

static void
send(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\r\n");
    fflush(stdout);
}

static void
quote(char* dest, const char* path)
{
    const char* p;
    char* q;
    for (p = path, q = dest; *p != '\0'; p++) {
        char c = *p;
        if ((c == '\"') || (c == '\\')) {
            *q = '\\';
            q++;
        }
        *q = c;
        q++;
    }
}

static void
send_dir(const char* path)
{
    struct stat sb;
    if (lstat(path, &sb) != 0) {
        perror("lstat failed");
        return;
    }
    char buf[2 * strlen(path) + 3];
    quote(buf, path);
    send("DIR %s %o %d %d", buf, 0777 & sb.st_mode, sb.st_uid, sb.st_gid);
    recv_ok();
}

static void
backup_parent(const char* path)
{
    const char* parent = dirname(path);
    if (parent == NULL) {
        perror("dirname failed");
        return;
    }
    if (strcmp(parent, "/") == 0) {
        return;
    }
    char dir[strlen(parent) + 1];
    strcpy(dir, parent);
    backup_parent(dir);

    send_dir(dir);
}

static void
send_symlink(const char* path)
{
    char quoted_path[2 * strlen(path) + 3];
    quote(quoted_path, path);

    struct stat sb;
    if (lstat(path, &sb) != 0) {
        perror("lstat failed");
        return;
    }

    size_t src_size = 4096;
    char src[src_size];
    ssize_t size = readlink(path, src, src_size);
    if (size == -1) {
        perror("readlink failed");
        return;
    }
    src[size] = '\0';
    char quoted_src[2 * strlen(src) + 3];
    quote(quoted_src, src);

    const char* fmt = "SYMLINK %s %o %u %u %s";
    mode_t mode = 0777 & sb.st_mode;
    send(fmt, quoted_path, mode, sb.st_uid, sb.st_gid, quoted_src);
    recv_ok();
}

static void
send_locked_file(const char* path, FILE* fp)
{
    char buf[2 * strlen(path) + 3];
    quote(buf, path);

    struct stat sb;
    if (lstat(path, &sb) != 0) {
        perror("lstat failed");
        return;
    }

    struct tm tm;
    localtime_r(&sb.st_mtime, &tm);
    size_t maxsize = strlen("yyyy-mm-ddThh:nn:ss") + 1;
    char mtime[maxsize];
    strftime(mtime, maxsize, "%Y-%m-%dT%H:%M:%S", &tm);

    const char* fmt = "FILE %s %o %u %u %s";
    send(fmt, buf, 0777 & sb.st_mode, sb.st_uid, sb.st_gid, mtime);

    if (recv_changed() != 0) {
        return;
    }

    size_t size = sb.st_size;
    send("BODY %zu", size);
    size_t rest = size;
    while (0 < rest) {
        size_t size = 4096;
        char buf[size];
        size_t n = size < rest ? size : rest;
        size_t nbytes = fread(buf, 1, n, fp);
        fwrite(buf, 1, nbytes, stdout);
        rest -= nbytes;
    }
    recv_ok();
}

static void
send_file(const char* path)
{
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        perror(path);
        return;
    }
    int fd = fileno(fp);
    if (flock(fd, LOCK_SH) != 0) {
        perror("flock failed");
        fclose(fp);
        return;
    }
    send_locked_file(path, fp);
    if (flock(fd, LOCK_UN) != 0) {
        perror("flock failed");
    }
    fclose(fp);
}

static void backup_dir(const char*);

static void
send_dir_entry(const char* path, const char* name)
{
    if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0)) {
        return;
    }
    char fullpath[strlen(path) + strlen(name) + 1];
    sprintf(fullpath, "%s/%s", path, name);
    struct stat sb;
    if (lstat(fullpath, &sb) != 0) {
        perror("lstat failed");
        return;
    }
    if (S_ISDIR(sb.st_mode)) {
        backup_dir(fullpath);
        return;
    }
    if (S_ISLNK(sb.st_mode)) {
        send_symlink(fullpath);
        return;
    }
    send_file(fullpath);
}

static void
backup_dir(const char* path)
{
    send_dir(path);

    DIR* dirp = opendir(path);
    if (dirp == NULL) {
        perror(path);
        return;
    }
    struct dirent* e;
    while ((e = readdir(dirp)) != NULL) {
        send_dir_entry(path, e->d_name);
    }
    closedir(dirp);
}

static void
backup_tree(const char* path)
{
    backup_parent(path);
    backup_dir(path);
}

static void
create_pipe(int fildes[2])
{
    if (pipe(fildes) != 0) {
        perror("pipe failed");
        abort();
    }
}

#define READ 0
#define WRITE 1

static void
dup_fd(int old, int new)
{
    if (dup2(old, new) == -1) {
        perror("dup2 failed");
        abort();
    }
}

static void
exec_ssh()
{
    int c2s[2];
    create_pipe(c2s);
    int s2c[2];
    create_pipe(s2c);

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed");
        abort();
    }
    if (0 < pid) {
        close(c2s[READ]);
        close(s2c[WRITE]);
        return;
    }
    close(c2s[WRITE]);
    close(s2c[READ]);
    char* args[] = {
        "/usr/bin/ssh",
        "windsor",
        "/home/tom/projects/ubts",
        NULL };
    if (execvp(args[0], args) == -1) {
        perror("execvp failed");
        return;
    }
    dup_fd(c2s[READ], 0);
    dup_fd(s2c[WRITE], 1);
}

int
main(int argc, const char* argv[])
{
    exec_ssh();

    int i;
    for (i = 1; i < argc; i++) {
        backup_tree(argv[i]);
    }
    return 0;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
