// fswatch —— 用 fanotify 监视指定进程打开了哪些文件（root 下运行）
//
// 用途：确认游戏的某个界面到底读取了哪些资源文件，不需要往游戏进程注入任何代码。
//
// 用法:
//   fswatch <路径前缀> [只关心的 pid]
//   fswatch /data/data/jp.gungho.pad/files 8142
//
// 输出（stdout）:
//   OPEN|ACCESS|EXEC  pid  路径
//
// 说明:
//   * NDK 的 bionic 不带 sys/fanotify.h，这里手写 ABI 定义（内核 uapi 稳定）。
//   * 用 FAN_MARK_FILESYSTEM 标记 /data，再在用户态按路径前缀过滤；
//     FAN_MARK_MOUNT 要求路径本身是挂载点，/data/data/xxx 不是。
//   * 需要 CAP_SYS_ADMIN（root）。

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ---- fanotify uapi（bionic 未提供）---- */
#define FAN_CLASS_NOTIF      0x00000000
#define FAN_CLOEXEC          0x00000001
#define FAN_MARK_ADD         0x00000001
#define FAN_MARK_MOUNT       0x00000010
#define FAN_MARK_FILESYSTEM  0x00000100
#define FAN_ACCESS           0x00000001
#define FAN_OPEN             0x00000020
#define FAN_OPEN_EXEC        0x00001000

struct fanotify_event_metadata {
    uint32_t event_len;
    uint8_t  vers;
    uint8_t  reserved;
    uint16_t metadata_len;
    uint64_t mask;
    int32_t  fd;
    int32_t  pid;
};

#define FAN_EVENT_METADATA_LEN ((int)sizeof(struct fanotify_event_metadata))
#define FAN_EVENT_OK(meta, len) \
    ((long)(len) >= (long)FAN_EVENT_METADATA_LEN && \
     (meta)->event_len >= (uint32_t)FAN_EVENT_METADATA_LEN && \
     (meta)->event_len <= (uint32_t)(len))
#define FAN_EVENT_NEXT(meta, len) \
    ((len) -= (meta)->event_len, \
     (struct fanotify_event_metadata *)(((char *)(meta)) + (meta)->event_len))

#ifndef __NR_fanotify_init
#define __NR_fanotify_init 262
#endif
#ifndef __NR_fanotify_mark
#define __NR_fanotify_mark 263
#endif

static int fanotify_init_(unsigned int flags, unsigned int event_f_flags) {
    return (int)syscall(__NR_fanotify_init, flags, event_f_flags);
}

static int fanotify_mark_(int fd, unsigned int flags, uint64_t mask,
                          int dirfd, const char *path) {
    return (int)syscall(__NR_fanotify_mark, fd, flags, mask, dirfd, path);
}

static const char *mask_name(uint64_t m) {
    if (m & FAN_OPEN_EXEC) return "EXEC";
    if (m & FAN_OPEN) return "OPEN";
    if (m & FAN_ACCESS) return "ACCESS";
    return "OTHER";
}

int main(int argc, char **argv) {
    const char *prefix = (argc > 1) ? argv[1] : "/data/data/jp.gungho.pad/files";
    int want_pid = (argc > 2) ? atoi(argv[2]) : 0;

    int fd = fanotify_init_(FAN_CLASS_NOTIF | FAN_CLOEXEC, O_RDONLY | O_LARGEFILE);
    if (fd < 0) {
        fprintf(stderr, "fanotify_init failed: %s\n", strerror(errno));
        return 2;
    }

    uint64_t mask = FAN_OPEN | FAN_ACCESS | FAN_OPEN_EXEC;

    if (fanotify_mark_(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, mask, AT_FDCWD, "/data") < 0) {
        fprintf(stderr, "FAN_MARK_FILESYSTEM /data failed: %s\n", strerror(errno));
        if (fanotify_mark_(fd, FAN_MARK_ADD | FAN_MARK_MOUNT, mask, AT_FDCWD, "/data") < 0) {
            fprintf(stderr, "FAN_MARK_MOUNT /data failed: %s\n", strerror(errno));
            if (fanotify_mark_(fd, FAN_MARK_ADD, mask, AT_FDCWD, prefix) < 0) {
                fprintf(stderr, "inode mark %s failed: %s\n", prefix, strerror(errno));
                return 3;
            }
            fprintf(stderr, "marked inode: %s\n", prefix);
        } else {
            fprintf(stderr, "marked mount: /data\n");
        }
    } else {
        fprintf(stderr, "marked filesystem: /data\n");
    }
    fprintf(stderr, "watching prefix=%s pid=%d\n", prefix, want_pid);
    fflush(stderr);

    char buf[8192] __attribute__((aligned(8)));
    char link[PATH_MAX];

    for (;;) {
        ssize_t len = read(fd, buf, sizeof(buf));
        if (len <= 0) {
            if (len < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            fprintf(stderr, "read failed: %s\n", strerror(errno));
            break;
        }
        struct fanotify_event_metadata *meta = (struct fanotify_event_metadata *)buf;
        while (FAN_EVENT_OK(meta, len)) {
            if (meta->fd >= 0) {
                char proc[64];
                snprintf(proc, sizeof(proc), "/proc/self/fd/%d", meta->fd);
                ssize_t n = readlink(proc, link, sizeof(link) - 1);
                if (n > 0) {
                    link[n] = '\0';
                    int pid_ok = (want_pid == 0) || ((int)meta->pid == want_pid);
                    if (pid_ok && strncmp(link, prefix, strlen(prefix)) == 0) {
                        printf("%s %d %s\n", mask_name(meta->mask), (int)meta->pid, link);
                        fflush(stdout);
                    }
                }
                close(meta->fd);
            }
            meta = FAN_EVENT_NEXT(meta, len);
        }
    }
    return 0;
}
