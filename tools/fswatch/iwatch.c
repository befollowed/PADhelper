// iwatch —— 递归给目录下所有文件挂 inotify，打印“被打开/被读取”的文件
//
// 为什么用 inotify：本机内核没编译 fanotify（fanotify_init 返回 ENOSYS），
// 而 inotify 的 IN_OPEN / IN_ACCESS 是挂在文件本身的，正好能回答
// “某个界面到底读了哪些本地资源”。
//
// 用法:
//   iwatch <目录> [输出文件]
//   iwatch /data/data/jp.gungho.pad/files/mon2 /data/local/tmp/iwatch.log
//
// 输出每行: <epoch_ms> <OPEN|ACCESS|CLOSE> <完整路径>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <sys/stat.h>

#define MAX_WATCH 200000

static char **g_paths;      /* wd -> path */
static int    g_cap;
static int    g_count;
static FILE  *g_out;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void add_watch(int ifd, const char *path) {
    if (g_count >= g_cap) return;
    int wd = inotify_add_watch(ifd, path, IN_OPEN | IN_ACCESS | IN_CLOSE_NOWRITE | IN_MODIFY);
    if (wd < 0) {
        if (errno != ENOSPC) fprintf(stderr, "watch %s failed: %s\n", path, strerror(errno));
        return;
    }
    if (wd < g_cap) {
        if (g_paths[wd]) free(g_paths[wd]);
        g_paths[wd] = strdup(path);
        g_count++;
    }
}

static void walk(int ifd, const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "opendir %s failed: %s\n", dir, strerror(errno));
        return;
    }
    struct dirent *e;
    char path[PATH_MAX];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(path, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 8) walk(ifd, path, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            add_watch(ifd, path);
        }
    }
    closedir(d);
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : "/data/data/jp.gungho.pad/files/mon2";
    g_out = (argc > 2) ? fopen(argv[2], "a") : stdout;
    if (!g_out) {
        fprintf(stderr, "open output failed: %s\n", strerror(errno));
        return 2;
    }

    g_cap = MAX_WATCH;
    g_paths = calloc(g_cap, sizeof(char *));
    if (!g_paths) return 2;

    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) {
        fprintf(stderr, "inotify_init1 failed: %s\n", strerror(errno));
        return 2;
    }

    walk(ifd, dir, 0);
    fprintf(stderr, "watching %d files under %s\n", g_count, dir);
    fflush(stderr);

    char buf[64 * 1024] __attribute__((aligned(8)));
    for (;;) {
        ssize_t len = read(ifd, buf, sizeof(buf));
        if (len <= 0) {
            if (len < 0 && (errno == EAGAIN || errno == EINTR)) {
                usleep(20000);
                continue;
            }
            break;
        }
        for (char *p = buf; p < buf + len; ) {
            struct inotify_event *ev = (struct inotify_event *)p;
            const char *name = (ev->wd >= 0 && ev->wd < g_cap) ? g_paths[ev->wd] : NULL;
            if (name && ev->len == 0) {
                const char *kind = (ev->mask & IN_OPEN) ? "OPEN"
                                 : (ev->mask & IN_ACCESS) ? "ACCESS"
                                 : (ev->mask & IN_MODIFY) ? "MODIFY" : "CLOSE";
                fprintf(g_out, "%lld %s %s\n", now_ms(), kind, name);
                fflush(g_out);
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return 0;
}
