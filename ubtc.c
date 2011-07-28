#include <assert.h>
#include <dirent.h>
#include <getopt.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH_SIZE 4096

#define TRACE(fmt, ...) do { \
    fprintf(stderr, "%s:%u " fmt "\n", __FILE__, __LINE__, __VA_ARGS__); \
} while (0)

struct Client {
    FILE* in;
    FILE* out;
    char root[PATH_SIZE];
};

typedef struct Client Client;

static int
recv_changed(Client* client)
{
    size_t size = 4096;
    char buf[size];
    if (fgets(buf, size, client->in) == NULL) {
        perror("fgets failed");
        abort();
    }
    const char* expected = "CHANGED";
    return strncmp(expected, buf, strlen(expected));
}

static void
recv_ok(Client* client)
{
    size_t size = 4096;
    char buf[size];
    if (fgets(buf, size, client->in) == NULL) {
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
send(Client* client, const char* fmt, ...)
{
    FILE* fp = client->out;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\r\n");
    fflush(fp);
}

static void
quote(char* dest, const char* path)
{
    char* q = dest;
    *q = '\"';
    q++;

    const char* p;
    for (p = path; *p != '\0'; p++) {
        char c = *p;
        if ((c == '\"') || (c == '\\')) {
            *q = '\\';
            q++;
        }
        *q = c;
        q++;
    }
    strcpy(q, "\"");
}

static void
get_path_from_root(char* dest, const char* root, const char* path)
{
    strcpy(dest, path + (strcmp(root, "/") == 0 ? 0 : strlen(root)));
}

#define ISO_8601_MAXSIZE (strlen("yyyy-mm-ddThh:nn:ss") + 1)

static void
to_iso8601(char* buf, size_t bufsize, const time_t* clock)
{
    struct tm tm;
    localtime_r(clock, &tm);
    strftime(buf, bufsize, "%Y-%m-%dT%H:%M:%S", &tm);
}

static void
send_dir(Client* client, const char* path)
{
    struct stat sb;
    if (lstat(path, &sb) != 0) {
        perror("lstat failed");
        return;
    }
    char path_from_root[strlen(path) + 1];
    get_path_from_root(path_from_root, client->root, path);
    char buf[2 * strlen(path_from_root) + 3];
    quote(buf, path_from_root);
    size_t maxsize = ISO_8601_MAXSIZE;
    char ctime[maxsize];
    to_iso8601(ctime, maxsize, &sb.st_ctime);
    const char* fmt = "DIR %s %o %d %d %s";
    send(client, fmt, buf, 0777 & sb.st_mode, sb.st_uid, sb.st_gid, ctime);
    recv_ok(client);
}

#define array_sizeof(a) (sizeof(a) / sizeof(a[0]))

static void
backup_parent(Client* client, const char* path)
{
    if (strcmp(client->root, path) == 0) {
        return;
    }

    const char* parent = dirname(path);
    if (parent == NULL) {
        perror("dirname failed");
        return;
    }
    char dir[strlen(parent) + 1];
    strcpy(dir, parent);
    backup_parent(client, dir);

    send_dir(client, path);
}

static void
send_symlink(Client* client, const char* path)
{
    char path_from_root[strlen(path) + 1];
    get_path_from_root(path_from_root, client->root, path);
    char quoted_path[2 * strlen(path_from_root) + 3];
    quote(quoted_path, path_from_root);

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
    size_t maxsize = ISO_8601_MAXSIZE;
    char ctime[maxsize];
    to_iso8601(ctime, maxsize, &sb.st_ctime);

    const char* fmt = "SYMLINK %s %o %u %u %s %s";
    mode_t mode = 0777 & sb.st_mode;
    uid_t uid = sb.st_uid;
    gid_t gid = sb.st_gid;
    send(client, fmt, quoted_path, mode, uid, gid, ctime, quoted_src);
    recv_ok(client);
}

static void
send_locked_file(Client* client, const char* path, FILE* fp)
{
    char path_from_root[strlen(path) + 1];
    get_path_from_root(path_from_root, client->root, path);
    char buf[2 * strlen(path_from_root) + 3];
    quote(buf, path_from_root);

    struct stat sb;
    if (lstat(path, &sb) != 0) {
        perror("lstat failed");
        return;
    }

    size_t maxsize = ISO_8601_MAXSIZE;
    char mtime[maxsize];
    to_iso8601(mtime, maxsize, &sb.st_mtime);
    char ctime[maxsize];
    to_iso8601(ctime, maxsize, &sb.st_ctime);

    const char* fmt = "FILE %s %o %u %u %s %s";
    mode_t mode = 0777 & sb.st_mode;
    send(client, fmt, buf, mode, sb.st_uid, sb.st_gid, mtime, ctime);

    if (recv_changed(client) != 0) {
        return;
    }

    size_t size = sb.st_size;
    send(client, "BODY %zu", size);
    size_t rest = size;
    while (0 < rest) {
        size_t size = 4096;
        char buf[size];
        size_t n = size < rest ? size : rest;
        size_t nbytes = fread(buf, 1, n, fp);
        fwrite(buf, 1, nbytes, client->out);
        fflush(client->out);
        rest -= nbytes;
    }
    recv_ok(client);
}

static void
send_file(Client* client, const char* path)
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
    send_locked_file(client, path, fp);
    if (flock(fd, LOCK_UN) != 0) {
        perror("flock failed");
    }
    fclose(fp);
}

static void backup_dir(Client*, const char*);

static void
send_dir_entry(Client* client, const char* path, const char* name)
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
        send_dir(client, fullpath);
        backup_dir(client, fullpath);
        return;
    }
    if (S_ISLNK(sb.st_mode)) {
        send_symlink(client, fullpath);
        return;
    }
    send_file(client, fullpath);
}

static void
backup_dir(Client* client, const char* path)
{
    DIR* dirp = opendir(path);
    if (dirp == NULL) {
        perror(path);
        return;
    }
    struct dirent* e;
    while ((e = readdir(dirp)) != NULL) {
        send_dir_entry(client, path, e->d_name);
    }
    closedir(dirp);
}

static void
backup_tree(Client* client, const char* path)
{
    backup_parent(client, path);
    backup_dir(client, path);
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

static FILE*
do_fdopen(int fd, const char* mode)
{
    FILE* fp = fdopen(fd, mode);
    if (fp == NULL) {
        perror("fdopen failed");
        abort();
    }
    return fp;
}

static pid_t
exec_server(Client* client, char* cmd)
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
        close(s2c[WRITE]);
        close(c2s[READ]);
        client->in = do_fdopen(s2c[READ], "r");
        client->out = do_fdopen(c2s[WRITE], "w");
        return pid;
    }
    dup_fd(c2s[READ], 0);
    dup_fd(s2c[WRITE], 1);
    close(c2s[WRITE]);
    close(s2c[READ]);
    char* argv[] = { "/bin/sh", "-c", cmd, NULL };
    if (execv(argv[0], argv) == -1) {
        perror("execv failed");
    }
    /* NOTREACHED */
    return 0;
}

static void
usage(const char* ident)
{
    printf("%s [--command=cmd] [--root=root] dir ...\n", ident);
}

static void
absolutize_path(char* dest, size_t size, const char* path)
{
    if (path[0] == '/') {
        strcpy(dest, path);
        return;
    }
    char* cwd = getcwd(NULL, 0);
    snprintf(dest, size, "%s/%s", cwd, path);
    free(cwd);
}

static void
skip_path_separators(const char** p)
{
    while (**p == '/') {
        (*p)++;
    }
}

static void
get_next_path_element(char* dest, size_t size, const char* path)
{
    const char* p = strchr(path, '/');
    if (p == NULL) {
        strncpy(dest, path, size);
        return;
    }
    size_t name_size = p - path;
    if (size < name_size + 1) {
        fprintf(stderr, "Name too long: %s", path);
        abort();
    }
    memcpy(dest, path, name_size);
    dest[name_size] = '\0';
}

static void
remove_parent(char** p, const char* top)
{
    char* q = strrchr(*p, '/');
    if (q == top) {
        return;
    }
    *p = q;
}

static void
remove_trailing_path_separators(char* p, const char* limit)
{
    char* q = p - 1;
    while ((limit < q) && (*q == '/')) {
        *q = '\0';
        q--;
    }
}

static void
normalize_path(char* dest, size_t size, const char* path)
{
    char abs_path[PATH_SIZE];
    absolutize_path(abs_path, array_sizeof(abs_path), path);

    char* end = dest + size;
    const char* from = abs_path;
    char* to = dest;
    while (*from != '\0') {
        if (*from == '/') {
            *to = *from;
            skip_path_separators(&from);
            to++;
            continue;
        }
        char name[PATH_SIZE];
        get_next_path_element(name, array_sizeof(name), from);
        if (strcmp(name, ".") == 0) {
            from += strlen(name);
            skip_path_separators(&from);
            continue;
        }
        if (strcmp(name, "..") == 0) {
            remove_parent(&to, path);
            from += strlen(name);
            skip_path_separators(&from);
            continue;
        }
        size_t len = strlen(name);
        if (end <= to + len) {
            fprintf(stderr, "Path too long: %s", path);
            abort();
        }
        strcpy(to, name);
        from += len;
        to += len;
    }
    *to = *from;
    if (strcmp(dest, "/") == 0) {
        return;
    }
    remove_trailing_path_separators(to, path);
}

int
main(int argc, char* argv[])
{
    struct option opts[] = {
        { "command", 1, NULL, 'c' },
        { "root", 1, NULL, 'r' },
        { NULL, 0, NULL, 0 }
    };

    /**
     * Parameters:
     * username
     * hostname
     * ubts_path
     * dest
     */
    char* cmd = "ssh tom@windsor ~/projects/UnnamedBackupTool/ubts /backup/nymphenburg";
    const char* root = "/";
    int opt;
    while ((opt = getopt_long_only(argc, argv, "", opts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            cmd = optarg;
            break;
        case 'r':
            root = optarg;
            break;
        default:
            usage(basename(argv[0]));
            return 1;
        }
    }

    Client client;
    normalize_path(client.root, PATH_SIZE, root);

    pid_t pid = exec_server(&client, cmd);

    int i;
    for (i = optind; i < argc; i++) {
        char abs_path[PATH_SIZE];
        normalize_path(abs_path, array_sizeof(abs_path), argv[i]);
        backup_tree(&client, abs_path);
    }
    send(&client, "THANK_YOU");

    int status;
    waitpid(pid, &status, 0);

    return 0;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
