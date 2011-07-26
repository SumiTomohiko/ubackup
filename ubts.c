#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>

#define PATH_SIZE 4096

static void
print_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
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
    char current_file[PATH_SIZE];
};

typedef struct Server Server;

enum Type {
    CMD_BODY,
    CMD_DIR,
    CMD_FILE,
    CMD_SYMLINK,
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
        } dir;
        struct {
            char path[PATH_SIZE];
            mode_t mode;
            uid_t uid;
            gid_t gid;
            time_t mtime;
        } file;
        struct {
            char path[PATH_SIZE];
            mode_t mode;
            uid_t uid;
            gid_t gid;
            char src[PATH_SIZE];
        } symlink;
    } u;
};

typedef struct Command Command;

static void
send(const char* msg)
{
    printf("%s\r\n", msg);
}

static void
send_ok()
{
    send("OK");
}

static bool
do_dir(const Server* server, const Command* cmd)
{
    size_t size = 4096;
    char buf[size];
    snprintf(buf, size, "%s%s", server->dest_dir, cmd->u.dir.path);
    if (mkdir(buf, 0755) != 0) {
        print_errno("mkdir failed", errno, buf);
        return false;
    }
    send_ok();
    return true;
}

#define array_sizeof(a) (sizeof(a) / sizeof(a[0]))

static bool
do_file(Server* server, const Command* cmd)
{
    char* path = server->current_file;
    snprintf(path, PATH_SIZE, "%s/%s", server->dest_dir, cmd->u.file.path);

    struct stat sb;
    if (lstat(path, &sb) != 0) {
        print_errno("lstat failed", errno, path);
        return false;
    }
    const char* msg = sb.st_mtime < cmd->u.file.mtime ? "CHANGED" : "UNCHANGED";
    send(msg);
    return true;
}

static bool
do_body(const Server* server, const Command* cmd)
{
    const char* path = server->current_file;
    FILE* fp = fopen(path, "w");
    if (fp == NULL) {
        print_errno("fopen failed", errno, path);
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
    /* TODO */
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
    while (!pred(**p)) {
        (*p)++;
    }
}

static void
skip_whitespace(const char** p)
{
    skip(p, isblank);
}

static int
parse_type(Type* type, const char** p)
{
    const char* from = *p;
    skip_whitespace(p);
    size_t size = *p - from;
    char name[size + 1];
    memcpy(name, p, size);
    name[size] = '\0';

    Name2Type name2type[] = {
        { "BODY", CMD_BODY },
        { "DIR", CMD_DIR },
        { "FILE", CMD_FILE },
        { "SYMLINK", CMD_SYMLINK }};
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
    }

    skip_double_quote(p);
    return 0;
}

static int
parse_mtime(time_t* dest, const char** p)
{
    struct tm tm;
    if (strptime(*p, "%Y-%m-%dT%H:%M:%S", &tm) == NULL) {
        return 1;
    }
    *dest = mktime(&tm);
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
    if (parse_mtime(&cmd->u.file.mtime, &p) != 0) {
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
        return false;
    }
    switch (cmd.type) {
    case CMD_BODY:
        return do_body(server, &cmd);
    case CMD_DIR:
        return do_dir(server, &cmd);
    case CMD_FILE:
        return do_file(server, &cmd);
    case CMD_SYMLINK:
        return do_symlink(server, &cmd);
    default:
        break;
    }

    return false;
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
    sprintf(millisecond, ".%03lu", tv.tv_usec / 1000);
    strcat(dest, millisecond);
    return true;
}

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

static bool
make_metadir(const char* path)
{
    char buf[strlen(path) + strlen(META_DIR) + 1];
    sprintf(buf, "%s/%s", path, META_DIR);
    return do_mkdir(buf);
}

static bool
make_backup_dir(const char* path)
{
    return do_mkdir(path) && make_metadir(path);
}

int
main(int argc, const char* argv[])
{
    const char* ident = basename(argv[0]);
    if (argc < 2) {
        print_error("Usage: %s <backup_dir>", ident);
        return 1;
    }
    openlog(ident, LOG_PID, LOG_LOCAL0);

    size_t maxsize = strlen("yyyy-mm-ddThh:nn:ss.000");
    char timestamp[maxsize + 1];
    if (!make_timestamp(timestamp, maxsize)) {
        return 1;
    }

    Server server;
    sprintf(server.dest_dir, "%s/%s", argv[1], timestamp);
    server.current_file[0] = '\0';
    syslog(LOG_INFO, "New backup: %s", server.dest_dir);
    if (!make_backup_dir(server.dest_dir)) {
        return 1;
    }

    size_t size = 4096;
    char buf[size];
    bool status = true;
    while (status && (fgets(buf, size, stdin) != NULL)) {
        trim(buf);
        syslog(LOG_INFO, "Recv: %s", buf);
        status = run_command(&server, buf);
    }

    closelog();

    return status ? 0 : 1;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
