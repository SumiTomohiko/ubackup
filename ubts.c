#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PATH_SIZE 4096

#define TRACE(fmt, ...) do { \
    fprintf(stderr, "%s:%u " fmt "\n", __FILE__, __LINE__, __VA_ARGS__); \
} while (0)

static void
record_log(int priority, const char* fmt, va_list ap)
{
    vsyslog(priority, fmt, ap);
}

static void
print_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    record_log(LOG_ERR, fmt, ap);

    FILE* out = stderr;
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");

    va_end(ap);
}

static void
print_info(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    record_log(LOG_INFO, fmt, ap);
    va_end(ap);
}

static void
print_errno(const char* msg, int e, const char* info)
{
    const char* s = strerror(e);
    if (info == NULL) {
        print_error("%s - %s", msg, s);
        return;
    }
    print_error("%s - %s - %s", msg, s, info);
}

struct Server {
    char dest_dir[PATH_SIZE];
    char prev_dir[PATH_SIZE];
    char current_file[PATH_SIZE];
};

typedef struct Server Server;

enum Type {
    CMD_BODY,
    CMD_DIR,
    CMD_FILE,
    CMD_SYMLINK,
    CMD_THANK_YOU,
};

typedef enum Type Type;

struct Command {
    Type type;
    union {
        struct {
            size_t size;
        } body;
        struct {
            char path[PATH_SIZE];
            mode_t mode;
            uid_t uid;
            gid_t gid;
            time_t ctime;
        } dir;
        struct {
            char path[PATH_SIZE];
            mode_t mode;
            uid_t uid;
            gid_t gid;
            time_t mtime;
            time_t ctime;
        } file;
        struct {
            char path[PATH_SIZE];
            mode_t mode;
            uid_t uid;
            gid_t gid;
            time_t ctime;
            char src[PATH_SIZE];
        } symlink;
    } u;
};

typedef struct Command Command;

static void
send(const char* msg)
{
    print_info("Send: %s", msg);
    printf("%s\r\n", msg);
    fflush(stdout);
}

#define IMPLEMENT_SEND(name, msg) \
    static void \
    name() \
    { \
        send(msg); \
    }
IMPLEMENT_SEND(send_ng, "NG")
IMPLEMENT_SEND(send_ok, "OK")

static bool
do_mkdir(const char* path)
{
    if (mkdir(path, 0755) != 0) {
        print_errno("mkdir failed", errno, path);
        return false;
    }
    return true;
}

#define META_DIR ".meta"

static void
join(char* dest, size_t size, const char* front, const char* rear)
{
    snprintf(dest, size, "%s/%s", front, rear);
}

static bool
make_meta_dir(const char* path)
{
    size_t size = strlen(path) + strlen(META_DIR) + 2;
    char buf[size];
    join(buf, size, path, META_DIR);
    return do_mkdir(buf);
}

static bool
make_backup_dir(const char* path)
{
    return do_mkdir(path) && make_meta_dir(path);
}

#define META_EXT ".meta"

static void
print_link_error(const char* name, int e, const char* src, const char* dest)
{
    size_t size = 32;
    char msg[size];
    snprintf(msg, size, "%s failed", name);

    const char* arrow = " -> ";
    char info[strlen(src) + strlen(dest) + strlen(arrow) + 1];
    sprintf(info, "%s%s%s", dest, arrow, src);

    print_errno(msg, e, info);
}

static bool
make_link(const char* src, const char* dest)
{
    if (link(src, dest) != 0) {
        print_link_error("link", errno, src, dest);
        return false;
    }
    return true;
}

static bool
check_file_changed(const Server* server, const char* path, time_t timestamp)
{
    if (server->prev_dir[0] == '\0') {
        return true;
    }

    struct stat sb;
    if (lstat(path, &sb) != 0) {
        return true;
    }
    return sb.st_mtime < timestamp;
}

static bool
save_meta_data(const Server* server, const char* path, mode_t mode, uid_t uid, gid_t gid, time_t ctime)
{
    char dir[PATH_SIZE];
    strcpy(dir, dirname(path));
    const char* s = strcmp(dir, "/") == 0 ? "" : dir;
    size_t meta_dir_size = strlen(s) + strlen(META_DIR) + 2;
    char meta_dir[meta_dir_size];
    join(meta_dir, meta_dir_size, s, META_DIR);

    const char* name = basename(path);
    char meta_name[strlen(name) + strlen(META_EXT) + 1];
    sprintf(meta_name, "%s%s", name, META_EXT);

    size_t meta_path_size = strlen(meta_dir) + strlen(meta_name) + 2;
    char meta_path[meta_path_size];
    join(meta_path, meta_path_size, meta_dir, meta_name);

    size_t prev_dir_size = strlen(server->prev_dir);
    char prev_path[prev_dir_size + strlen(meta_path) + 1];
    sprintf(prev_path, "%s%s", server->prev_dir, meta_path);
    char abspath[prev_dir_size + strlen(meta_path) + 1];
    sprintf(abspath, "%s%s", server->dest_dir, meta_path);

    if (!check_file_changed(server, prev_path, ctime)) {
        return make_link(prev_path, abspath);
    }

    FILE* fp = fopen(abspath, "w");
    if (fp == NULL) {
        print_errno("fopen failed", errno, abspath);
        return false;
    }
    fprintf(fp, "%o\n", mode);
    fprintf(fp, "%u\n", uid);
    fprintf(fp, "%u", gid);
    fclose(fp);

    return true;
}

static bool
do_dir(const Server* server, const Command* cmd)
{
    char path[strlen(server->dest_dir) + strlen(cmd->u.dir.path) + 1];
    sprintf(path, "%s%s", server->dest_dir, cmd->u.dir.path);
    if (!make_backup_dir(path)) {
        send_ng();
        return false;
    }
    mode_t mode = cmd->u.dir.mode;
    uid_t uid = cmd->u.dir.uid;
    gid_t gid = cmd->u.dir.gid;
    time_t ctime = cmd->u.dir.ctime;
    if (!save_meta_data(server, cmd->u.dir.path, mode, uid, gid, ctime)) {
        send_ng();
        return false;
    }
    send_ok();
    return true;
}

#define array_sizeof(a) (sizeof(a) / sizeof(a[0]))

static bool
do_file(Server* server, const Command* cmd)
{
    char* current_file = server->current_file;
    const char* path = cmd->u.file.path;
    snprintf(current_file, PATH_SIZE, "%s%s", server->dest_dir, path);

    mode_t mode = cmd->u.file.mode;
    uid_t uid = cmd->u.file.uid;
    gid_t gid = cmd->u.file.gid;
    if (!save_meta_data(server, path, mode, uid, gid, cmd->u.file.ctime)) {
        send_ng();
        return false;
    }

    size_t size = strlen(server->prev_dir) + strlen(path) + 1;
    char prev_path[size];
    sprintf(prev_path, "%s%s", server->prev_dir, path);
    if (check_file_changed(server, prev_path, cmd->u.file.mtime)) {
        send("CHANGED");
        return true;
    }

    if (!make_link(prev_path, current_file)) {
        send_ng();
        return false;
    }

    send("UNCHANGED");
    return true;
}

static bool
do_body(const Server* server, const Command* cmd)
{
    const char* path = server->current_file;
    FILE* fp = fopen(path, "w");
    if (fp == NULL) {
        print_errno("fopen failed", errno, path);
        send_ng();
        return false;
    }
    size_t rest = cmd->u.body.size;
    while (0 < rest) {
        int max = 4096;
        char buf[max];
        size_t nbytes = fread(buf, 1, max < rest ? max : rest, stdin);
        fwrite(buf, 1, nbytes, fp);
        rest -= nbytes;
    }
    fclose(fp);

    send_ok();
    return true;
}

static bool
do_symlink(const Server* server, const Command* cmd)
{
    const char* path = cmd->u.symlink.path;
    mode_t mode = cmd->u.symlink.mode;
    uid_t uid = cmd->u.symlink.uid;
    gid_t gid = cmd->u.symlink.gid;
    time_t ctime = cmd->u.symlink.ctime;
    if (!save_meta_data(server, path, mode, uid, gid, ctime)) {
        send_ng();
        return false;
    }
    size_t size = strlen(server->dest_dir) + strlen(path) + 2;
    char buf[size];
    join(buf, size, server->dest_dir, path);
    if (symlink(cmd->u.symlink.src, buf) != 0) {
        print_link_error("symlink", errno, cmd->u.symlink.src, buf);
        send_ng();
        return false;
    }
    send_ok();
    return true;
}

static void
trim(char* s)
{
    char* p = strrchr(s, '\r');
    if (p == NULL) {
        return;
    }
    *p = '\0';
}

struct Name2Type {
    const char* name;
    Type type;
};

typedef struct Name2Type Name2Type;

static bool
get_type_of_name(Type* type, const char* name, Name2Type* n2t)
{
    if (strcmp(name, n2t->name) != 0) {
        return false;
    }
    *type = n2t->type;
    return true;
}

static void
skip(const char** p, int (*pred)(int))
{
    while (pred(**p)) {
        (*p)++;
    }
}

static void
skip_whitespace(const char** p)
{
    skip(p, isblank);
}

static int
is_command_char(int c)
{
    return isalpha(c) || (c == '_');
}

static int
parse_type(Type* type, const char** p)
{
    const char* from = *p;
    skip(p, is_command_char);
    size_t size = *p - from;
    char name[size + 1];
    memcpy(name, from, size);
    name[size] = '\0';

    Name2Type name2type[] = {
        { "BODY", CMD_BODY },
        { "DIR", CMD_DIR },
        { "FILE", CMD_FILE },
        { "SYMLINK", CMD_SYMLINK },
        { "THANK_YOU", CMD_THANK_YOU }};
    bool found = false;
    int i;
    for (i = 0; !found && (i < array_sizeof(name2type)); i++) {
        found = get_type_of_name(type, name, &name2type[i]);
    }
    if (!found) {
        return 1;
    }

    return 0;
}

static int
isoctal(int c)
{
    return ('0' <= c) && (c < '8');
}

static int
parse_mode(mode_t* dest, const char** p)
{
    skip_whitespace(p);

    const char* from = *p;
    skip(p, isoctal);
    size_t size = *p - from;
    char buf[size + 1];
    memcpy(buf, from, size);
    buf[size] = '\0';
    *dest = strtoul(buf, NULL, 8);
    return 0;
}

static int
parse_decimal(size_t* dest, const char** p)
{
    skip_whitespace(p);

    const char* from = *p;
    skip(p, isdigit);
    size_t size = *p - from;
    char buf[size + 1];
    memcpy(buf, from, size);
    buf[size] = '\0';
    *dest = strtoul(buf, NULL, 10);
    return 0;
}

static int
parse_body(Command* cmd, const char* params)
{
    const char* p = params;
    return parse_decimal(&cmd->u.body.size, &p);
}

static void
skip_double_quote(const char** p)
{
    assert(**p == '\"');
    (*p)++;
}

static int
parse_string(char* dest, const char** p)
{
    skip_whitespace(p);
    skip_double_quote(p);

    char* q = dest;
    while (**p != '\"') {
        if (**p == '\\') {
            (*p)++;
        }
        *q = **p;
        (*p)++;
        q++;
    }
    *q = '\0';

    skip_double_quote(p);
    return 0;
}

static int
parse_timestamp(time_t* dest, const char** p)
{
    skip_whitespace(p);

    struct tm tm;
    bzero(&tm, sizeof(tm));
    const char* q = strptime(*p, "%Y-%m-%dT%H:%M:%S", &tm);
    if (q == NULL) {
        return 1;
    }
    *dest = mktime(&tm);
    *p = q;
    return 0;
}

static int
parse_symlink(Command* cmd, const char* params)
{
    const char* p = params;
    if (parse_string(cmd->u.symlink.path, &p) != 0) {
        return 1;
    }
    if (parse_mode(&cmd->u.symlink.mode, &p) != 0) {
        return 1;
    }
    if (parse_decimal(&cmd->u.symlink.uid, &p) != 0) {
        return 1;
    }
    if (parse_decimal(&cmd->u.symlink.gid, &p) != 0) {
        return 1;
    }
    if (parse_timestamp(&cmd->u.symlink.ctime, &p) != 0) {
        return 1;
    }
    if (parse_string(cmd->u.symlink.src, &p) != 0) {
        return 1;
    }
    return 0;
}

static int
parse_file(Command* cmd, const char* params)
{
    const char* p = params;
    if (parse_string(cmd->u.file.path, &p) != 0) {
        return 1;
    }
    if (parse_mode(&cmd->u.file.mode, &p) != 0) {
        return 1;
    }
    if (parse_decimal(&cmd->u.file.uid, &p) != 0) {
        return 1;
    }
    if (parse_decimal(&cmd->u.file.gid, &p) != 0) {
        return 1;
    }
    if (parse_timestamp(&cmd->u.file.mtime, &p) != 0) {
        return 1;
    }
    if (parse_timestamp(&cmd->u.file.ctime, &p) != 0) {
        return 1;
    }
    return 0;
}

static int
parse_dir(Command* cmd, const char* params)
{
    const char* p = params;
    if (parse_string(cmd->u.dir.path, &p) != 0) {
        return 1;
    }
    if (parse_mode(&cmd->u.dir.mode, &p) != 0) {
        return 1;
    }
    if (parse_decimal(&cmd->u.dir.uid, &p) != 0) {
        return 1;
    }
    if (parse_decimal(&cmd->u.dir.gid, &p) != 0) {
        return 1;
    }
    if (parse_timestamp(&cmd->u.dir.ctime, &p) != 0) {
        return 1;
    }
    return 0;
}

static int
parse(Command* cmd, const char* line)
{
    const char* p = line;
    if (parse_type(&cmd->type, &p) != 0) {
        return 1;
    }
    switch (cmd->type) {
    case CMD_BODY:
        return parse_body(cmd, p);
    case CMD_DIR:
        return parse_dir(cmd, p);
    case CMD_FILE:
        return parse_file(cmd, p);
    case CMD_SYMLINK:
        return parse_symlink(cmd, p);
    case CMD_THANK_YOU:
        return 0;
    default:
        break;
    }
    return 1;
}

static bool
run_command(Server* server, const char* line)
{
    Command cmd;
    if (parse(&cmd, line) != 0) {
        send_ng();
        return true;
    }
    switch (cmd.type) {
    case CMD_BODY:
        do_body(server, &cmd);
        break;
    case CMD_DIR:
        do_dir(server, &cmd);
        break;
    case CMD_FILE:
        do_file(server, &cmd);
        break;
    case CMD_SYMLINK:
        do_symlink(server, &cmd);
        break;
    case CMD_THANK_YOU:
    default:
        return false;
    }

    return true;
}

static bool
make_timestamp(char* dest, size_t maxsize)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        print_errno("gettimeofday failed", errno, NULL);
        return false;
    }
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    strftime(dest, maxsize, "%Y-%m-%dT%H:%M:%S", &tm);
    char millisecond[5];
    sprintf(millisecond, ",%03lu", tv.tv_usec / 1000);
    strcat(dest, millisecond);
    return true;
}

static void
update_prev(const char* name, struct timeval* last, char* buf, size_t bufsize)
{
    bool found = false;
    const char* ignored[] = { ".", "..", "meta" };
    int i;
    for (i = 0; !found && (i < array_sizeof(ignored)); i++) {
        found = strcmp(ignored[i], name) == 0 ? true : false;
    }
    if (found) {
        return;
    }

    char timestamp[strlen(name) + 1];
    strcpy(timestamp, name);
    char* p = strchr(timestamp, ',');
    if (p == NULL) {
        return;
    }
    *p = '\0';
    struct tm tm;
    if (strptime(timestamp, "%Y-%m-%dT%H:%M:%S", &tm) == NULL) {
        return;
    }
    struct timeval tv = { mktime(&tm), atoi(p + 1) };
    if (tv.tv_sec < last->tv_sec) {
        return;
    }
    if ((tv.tv_sec == last->tv_sec) && (tv.tv_usec < last->tv_usec)) {
        return;
    }
    memcpy(last, &tv, sizeof(tv));
    strncpy(buf, name, bufsize);
}

static bool
find_prev(char* dest, const char* dir)
{
    DIR* dirp = opendir(dir);
    if (dirp == NULL) {
        print_errno("opendir failed", errno, dir);
        return false;
    }
    char buf[PATH_SIZE];
    struct timeval last = { 0, 0 };
    struct dirent* e;
    while ((e = readdir(dirp)) != NULL) {
        update_prev(e->d_name, &last, buf, PATH_SIZE);
    }
    closedir(dirp);
    strcpy(dest, buf);
    return true;
}

static void
set_prev_dir(char* dest, size_t size, const char* backup_dir, const char* name)
{
    if (name[0] == '\0') {
        dest[0] = '\0';
        return;
    }
    join(dest, size, backup_dir, name);
}

int
main(int argc, const char* argv[])
{
    const char* s = basename(argv[0]);
    if (argc < 2) {
        print_error("Usage: %s <backup_dir>", s);
        return 1;
    }
    char ident[strlen(s) + 1];
    strcpy(ident, s);
    openlog(ident, LOG_PID, LOG_LOCAL0);

    const char* backup_dir = argv[1];
    size_t maxsize = strlen("yyyy-mm-ddThh:nn:ss,000");
    char prev[maxsize + 1];
    if (!find_prev(prev, backup_dir)) {
        return 1;
    }

    char timestamp[maxsize + 1];
    if (!make_timestamp(timestamp, maxsize)) {
        return 1;
    }

    Server server;
    join(server.dest_dir, PATH_SIZE, backup_dir, timestamp);
    set_prev_dir(server.prev_dir, PATH_SIZE, backup_dir, prev);
    server.current_file[0] = '\0';
    print_info("New backup: %s", server.dest_dir);
    print_info("Prev backup: %s", server.prev_dir);
    if (!make_backup_dir(server.dest_dir)) {
        return 1;
    }

    size_t size = 4096;
    char buf[size];
    bool status = true;
    while (status && (fgets(buf, size, stdin) != NULL)) {
        trim(buf);
        print_info("Recv: %s", buf);
        status = run_command(&server, buf);
    }

    closelog();

    return status ? 0 : 1;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
