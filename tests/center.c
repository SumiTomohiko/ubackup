#include <stdio.h>

static void
print_bar(char mark, int width)
{
    int i;
    for (i = 0; i < width; i++) {
        printf("%c", mark);
    }
}

int
main(int argc, const char* argv[])
{
    char mark = argv[1][0];
    const char* msg = argv[2];
    size_t len = strlen(msg);
    int width = 80;
    int bar_width = (width - (len + 2)) / 2;

    print_bar(mark, bar_width);
    printf(" %s ", msg);
    print_bar(mark, bar_width + len % 2);

    return 0;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
