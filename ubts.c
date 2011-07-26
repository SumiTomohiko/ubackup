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

static const char* dest_dir = "/backup/nymphenburg";
static char current_file[PATH_SIZE];

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

static int
do_dir(const Command* cmd)
{
    size_t size = 4096;
    char buf[size];
    snprintf(buf, size, "%s%s", dest_dir, cmd->u.dir.path);
    if (mkdir(buf, 0755) != 0) {
        fprintf(stderr, "mkdir failed - %s - %s", strerror(errno), buf);
        return 1;
    }
    return 0;
}

#define array_sizeof(a) (sizeof(a) / sizeof(a[0]))

static int
do_file(const Command* cmd)
{
    size_t size = array_sizeof(current_file);
    snprintf(current_file, size, "%s/%s", dest_dir, cmd->u.file.path);
    return 0;
}

static int
do_body(const Command* cmd)
{
    FILE* fp = fopen(current_file, "w");
    if (fp == NULL) {
        fprintf(stderr, "Cannot open %s", current_file);
        return 1;
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
    return 0;
}

static int
do_symlink(const Command* cmd)
{
    /* TODO */
    return 0;
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

static int
run_command(const char* line)
{
    Command cmd;
    if (parse(&cmd, line) != 0) {
        return 1;
    }
    switch (cmd.type) {
    case CMD_BODY:
        return do_body(&cmd);
    case CMD_DIR:
        return do_dir(&cmd);
    case CMD_FILE:
        return do_file(&cmd);
    case CMD_SYMLINK:
        return do_symlink(&cmd);
    default:
        break;
    }

    return 1;
}

int
main(int argc, const char* argv[])
{
    openlog(basename(argv[0]), LOG_PID, LOG_LOCAL0);

    size_t size = 4096;
    char buf[size];
    int status = 0;
    while ((status == 0) && (fgets(buf, size, stdin) != NULL)) {
        trim(buf);
        syslog(LOG_INFO, "Recv: %s", buf);
        status = run_command(buf);
    }

    closelog();

    return status;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
