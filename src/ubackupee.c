#include <ubackup/config.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PATH_SIZE 4096
#define BUF_SIZE PATH_SIZE

#define TRACE(fmt, ...) do { \
    fprintf(stderr, "%s:%u " fmt "\n", __FILE__, __LINE__, __VA_ARGS__); \
} while (0)

struct Client {
    FILE* in;
    FILE* out;
    char root[PATH_SIZE];
    struct {
        int num_files;
        int num_changed;
        uint64_t send_bytes;
        int num_skipped;
        int num_dir;
        int num_symlinks;
        time_t start_time;
    } stat;
    struct {
        bool block_special;
        bool char_special;
        bool pipe;
        bool socket;
        bool whiteout;
    } disable_skipped_warning;
};

typedef struct Client Client;

static void
print_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    FILE* out = stderr;
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");
    va_end(ap);
}

static void
print_errno(const char* msg, int e, const char* info)
{
    print_error("%s: %s: %s", msg, strerror(e), info);
}

#define PRINT_ERRNO(msg, info) print_errno((msg), errno, (info))

static void
print_errno2(const char* msg, int e)
{
    print_error("%s: %s", msg, strerror(e));
}

#define PRINT_ERRNO2(msg) print_errno2((msg), errno)

static int
recv_changed(Client* client)
{
    size_t size = 4096;
    char buf[size];
    if (fgets(buf, size, client->in) == NULL) {
        PRINT_ERRNO2("Receiving \"CHANGED\" failed");
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
        PRINT_ERRNO2("Receiving \"OK\" failed");
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
        PRINT_ERRNO("lstat directory failed", path);
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
        PRINT_ERRNO("dirname failed", path);
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
        PRINT_ERRNO("lstat symlink failed", path);
        return;
    }

    size_t src_size = 4096;
    char src[src_size];
    ssize_t size = readlink(path, src, src_size);
    if (size == -1) {
        PRINT_ERRNO("readlink failed", path);
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
        PRINT_ERRNO("lstat file failed", path);
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
    client->stat.num_changed++;

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
    client->stat.send_bytes += size;
}

static void
send_file(Client* client, const char* path)
{
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        PRINT_ERRNO("fopen failed", path);
        return;
    }
    int fd = fileno(fp);
    if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
        PRINT_ERRNO("flock to lock failed", path);
        fclose(fp);
        return;
    }
    send_locked_file(client, path, fp);
    if (flock(fd, LOCK_UN) != 0) {
        PRINT_ERRNO("flock to unlock failed", path);
    }
    fclose(fp);
}

static void backup_dir(Client*, const char*);

static void
print_skipped_warning(bool disabled, const char* path, const char* name)
{
    if (disabled) {
        return;
    }
    fprintf(stderr, "%s is a %s, skipped.\n", path, name);
}

static bool
is_ignored(const char* path, const char* name)
{
    if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0)) {
        return true;
    }
    if (strcmp(name, ".meta") == 0) {
        print_error("Warning: Ignored %s/%s", path, name);
        return true;
    }
    return false;
}

static void
send_dir_entry(Client* client, const char* path, const char* name)
{
    if (is_ignored(path, name)) {
        return;
    }

    char fullpath[strlen(path) + strlen(name) + 1];
    sprintf(fullpath, "%s/%s", path, name);
    struct stat sb;
    if (lstat(fullpath, &sb) != 0) {
        PRINT_ERRNO("lstat directory entry failed", fullpath);
        return;
    }
    mode_t mode = sb.st_mode;
    if (S_ISREG(sb.st_mode)) {
        client->stat.num_files++;
        send_file(client, fullpath);
        return;
    }
    if (S_ISDIR(mode)) {
        client->stat.num_dir++;
        send_dir(client, fullpath);
        backup_dir(client, fullpath);
        return;
    }
    if (S_ISLNK(mode)) {
        client->stat.num_symlinks++;
        send_symlink(client, fullpath);
        return;
    }
    client->stat.num_skipped++;
#define INFO(pred, desc, flag) do { \
    if (pred(mode)) { \
        bool disabled = client->disable_skipped_warning.flag; \
        print_skipped_warning(disabled, fullpath, desc); \
        return; \
    } \
} while (0)
    INFO(S_ISBLK, "block special file", block_special);
    INFO(S_ISCHR, "character special file", char_special);
    INFO(S_ISFIFO, "pipe for FIFO special file", pipe);
    INFO(S_ISSOCK, "socket", socket);
    INFO(S_ISWHT, "whiteout", whiteout);
#undef INFO
    /* NOTREACHED */
    assert(42 != 42);
}

static void
backup_dir(Client* client, const char* path)
{
    DIR* dirp = opendir(path);
    if (dirp == NULL) {
        PRINT_ERRNO("opendir failed", path);
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
        PRINT_ERRNO2("pipe failed");
        abort();
    }
}

#define READ 0
#define WRITE 1

static void
dup_fd(int old, int new)
{
    if (dup2(old, new) == -1) {
        PRINT_ERRNO2("dup2 failed");
        abort();
    }
}

static FILE*
do_fdopen(int fd, const char* mode)
{
    FILE* fp = fdopen(fd, mode);
    if (fp == NULL) {
        PRINT_ERRNO2("fdopen failed");
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
        PRINT_ERRNO2("fork failed");
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
        PRINT_ERRNO2("execv failed");
        abort();
    }
    /* NOTREACHED */
    return 0;
}

static void
usage(const char* ident)
{
    printf("%s [--command=cmd] [--root=root] src_dir ... dest_dir\n", ident);
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

struct Pair {
    const char* name;
    const char* value;
};

typedef struct Pair Pair;

static const char*
get_value_of_pair(Pair* pairs, int size, const char* name)
{
    int result = !0;
    int i;
    for (i = 0; (result != 0) && (i < size); i++) {
        result = strcmp(name, pairs[i].name);
    }
    return result == 0 ? pairs[i - 1].value : NULL;
}

static const char*
get_template_value(const char* name, const char* hostname, const char* ubts_path, const char* dest_dir)
{
    Pair name2value[] = {
        { "dest_dir", dest_dir },
        { "hostname", hostname },
        { "ubts_path", ubts_path }};
    return get_value_of_pair(name2value, array_sizeof(name2value), name);
}

static void
print_blank_template_value_error(const char* name)
{
    Pair name2opt[] = {
        { "hostname", "hostname" },
        { "ubts_path", "ubts-path" }};
    const char* opt = get_value_of_pair(name2opt, array_sizeof(name2opt), name);
    print_error("You must give --%s option.", opt);
}

static int
replace_template(char* dest, const char* destend, const char* tmpl, const char* hostname, const char* ubts_path, const char* dest_dir)
{
    const char* p = tmpl;
    char* q = dest;
#define TEMPLATE_FINISH(p) (*(p) == '\0')
#define DEST_FINISH(p) (destend <= (p))
    while (!TEMPLATE_FINISH(p) && (*p != '{') && !DEST_FINISH(q)) {
        *q = *p;
        p++;
        q++;
    }
    if (TEMPLATE_FINISH(p)) {
        return 0;
    }
    if (DEST_FINISH(q)) {
        print_error("Too long template");
        return 1;
    }
#undef TEMPLATE_FINISH

    p++;
    const char* pend = strchr(p, '}');
    if (pend == NULL) {
        print_error("You must close a template variable with '}'");
        return 1;
    }
    size_t size = pend - p;
    char name[size + 1];
    memcpy(name, p, size);
    name[size] = '\0';
    const char* value = get_template_value(name, hostname, ubts_path, dest_dir);
    if (value == NULL) {
        print_error("Unknown a template variable: %s", name);
        return 1;
    }
    if (strcmp(value, "") == 0) {
        print_blank_template_value_error(name);
        return 1;
    }
    q = stpncpy(q, value, destend - q);
    return replace_template(q, destend, pend + 1, hostname, ubts_path, dest_dir);
}

static int
make_command(char* dest, size_t destsize, const char* tmpl, const char* hostname, const char* ubts_path, const char* dest_dir)
{
    return replace_template(dest, dest + destsize, tmpl, hostname, ubts_path, dest_dir);
}

static void
print_version()
{
    printf("%s of ubackup %s\n", getprogname(), UBACKUP_VERSION);
}

static int
query(Client* client, const char* name, char* value)
{
    send(client, name);

    size_t size = BUF_SIZE;
    char buf[BUF_SIZE];
    if (fgets(buf, size, client->in) == NULL) {
        PRINT_ERRNO("Failed quering", name);
        return 1;
    }
    const char* head = "OK ";
    size_t len = strlen(head);
    if (strncmp(buf, head, len) != 0) {
        PRINT_ERRNO("Server responsed NG in querying", name);
        return 1;
    }
    char* p = buf + len;
    char* q = strchr(p, '\r');
    *q = '\0';
    strncpy(value, p, BUF_SIZE);
    return 0;
}

static int
query_uint64(Client* client, const char* name, uint64_t* value)
{
    char buf[BUF_SIZE];
    if (query(client, name, buf) != 0) {
        return 1;
    }
    *value = strtoull(buf, NULL, 10);
    return 0;
}

static void
make_timestamp(char* buf, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, BUF_SIZE, "%Y-%m-%dT%H:%M:%S", &tm);
}

static int
do_print_stat(Client* client)
{
    char start_time[BUF_SIZE];
    make_timestamp(start_time, client->stat.start_time);
    time_t t = time(NULL);
    char end_time[BUF_SIZE];
    make_timestamp(end_time, t);
    time_t sec = t - client->stat.start_time;
    int min = sec / 60;
    int hour = min / 60;

    char name[BUF_SIZE];
    if (query(client, "NAME", name) != 0) {
        return 1;
    }
    uint64_t disk_total;
    if (query_uint64(client, "DISK_TOTAL", &disk_total) != 0) {
        return 1;
    }
    uint64_t disk_usage;
    if (query_uint64(client, "DISK_USAGE", &disk_usage) != 0) {
        return 1;
    }
    uint64_t disk_available = disk_total - disk_usage;
#define GIGA(n) ((n) / (1024 * 1024 * 1024))
    printf("Backup name: %s\n\
Number of files: %d\n\
Number of changed files: %d\n\
Number of unchanged files: %d\n\
Number of skipped files: %d\n\
Send bytes: %lu\n\
Number of symbolic links: %d\n\
Number of directories: %d\n\
Start time: %s\n\
End time: %s\n\
Time: %ld[sec] (%d[hour] %d[min] %ld[sec])\n\
Disk total: %lu[Gbyte]\n\
Disk usage: %lu[Gbyte] (%lu%%)\n\
Disk available: %lu[Gbyte] (%lu%%)\n", name, client->stat.num_files, client->stat.num_changed, client->stat.num_files - client->stat.num_changed, client->stat.num_skipped, client->stat.send_bytes, client->stat.num_symlinks, client->stat.num_dir, start_time, end_time, sec, hour, min % 60, sec % 60, GIGA(disk_total), GIGA(disk_usage), (100 * disk_usage) / disk_total, GIGA(disk_available), (100 * disk_available) / disk_total);
#undef GIGA
    return 0;
}

static void
do_remove_old(Client* client)
{
    send(client, "REMOVE_OLD");
    recv_ok(client);
}

int
main(int argc, char* argv[])
{
    Client client;
    bzero(&client, sizeof(client));
    client.disable_skipped_warning.block_special = false;
    client.disable_skipped_warning.char_special = false;
    client.disable_skipped_warning.pipe = false;
    client.disable_skipped_warning.socket = false;
    client.disable_skipped_warning.whiteout = false;

    struct option opts[] = {
        { "command", required_argument, NULL, 'c' },
        { "disable-skipped-socket-warning", no_argument, NULL, 1 },
        { "hostname", required_argument, NULL, 'h' },
        { "local", no_argument, NULL, 'l' },
        { "print-statistics", no_argument, NULL, 's' },
        { "root", required_argument, NULL, 'r' },
        { "ubts-path", required_argument, NULL, 'u' },
        { "version", no_argument, NULL, 'v' },
        { NULL, 0, NULL, 0 }
    };

#define USAGE() usage(basename(argv[0]))
    const char* hostname = "";
    const char* root = "/";
    const char* tmpl = "ssh {hostname} {ubts_path} {dest_dir}";
    const char* ubts_path = "ubts";
    bool print_stat = false;
    int opt;
    while ((opt = getopt_long(argc, argv, "v", opts, NULL)) != -1) {
        switch (opt) {
        case 1:
            client.disable_skipped_warning.socket = true;
            break;
        case 'c':
            tmpl = optarg;
            break;
        case 'h':
            hostname = optarg;
            break;
        case 'l':
            tmpl = "{ubts_path} {dest_dir}";
            break;
        case 'r':
            root = optarg;
            break;
        case 's':
            print_stat = true;
            break;
        case 'u':
            ubts_path = optarg;
            break;
        case 'v':
            print_version();
            return 0;
        default:
            USAGE();
            return 1;
        }
    }
    if (argc - 1 <= optind) {
        print_error("Give both source directories and a destination directory.");
        USAGE();
        return 1;
    }
#undef USAGE

    if ((client.stat.start_time = time(NULL)) == (time_t)(-1)) {
        print_error("time(3) failed.");
        return 1;
    }

    normalize_path(client.root, PATH_SIZE, root);
    char cmd[4096];
    const char* dest_dir = argv[argc - 1];
    if (make_command(cmd, array_sizeof(cmd), tmpl, hostname, ubts_path, dest_dir) != 0) {
        return 1;
    }

    pid_t pid = exec_server(&client, cmd);

    int i;
    for (i = optind; i < argc - 1; i++) {
        char abs_path[PATH_SIZE];
        normalize_path(abs_path, array_sizeof(abs_path), argv[i]);
        backup_tree(&client, abs_path);
    }
    do_remove_old(&client);
    if (print_stat && (do_print_stat(&client) != 0)) {
        print_error("Cannot print statistics.");
    }
    send(&client, "THANK_YOU");

    int status;
    waitpid(pid, &status, 0);

    return 0;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */