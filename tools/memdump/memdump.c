// memdump <pid> <start_hex> <len_hex> <outfile>
// Reads another process's memory through /proc/<pid>/mem (needs root).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s pid start len out\n", argv[0]); return 2; }
    int pid = atoi(argv[1]);
    unsigned long long start = strtoull(argv[2], NULL, 0);
    unsigned long long len   = strtoull(argv[3], NULL, 0);
    char path[64]; snprintf(path, sizeof path, "/proc/%d/mem", pid);

    int in = open(path, O_RDONLY);
    if (in < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 1; }
    int out = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { fprintf(stderr, "open %s: %s\n", argv[4], strerror(errno)); return 1; }

    unsigned long long off = start, end = start + len;
    static unsigned char buf[1 << 20];
    unsigned long long written = 0;
    while (off < end) {
        size_t want = (size_t)((end - off) > sizeof buf ? sizeof buf : (end - off));
        ssize_t n = pread(in, buf, want, (off_t)off);
        if (n < 0) { fprintf(stderr, "pread @0x%llx: %s\n", off, strerror(errno)); break; }
        if (n == 0) break;
        if (write(out, buf, (size_t)n) != n) { fprintf(stderr, "write: %s\n", strerror(errno)); break; }
        off += (unsigned long long)n;
        written += (unsigned long long)n;
    }
    fprintf(stderr, "wrote %llu bytes\n", written);
    close(out); close(in);
    return written == len ? 0 : 1;
}
