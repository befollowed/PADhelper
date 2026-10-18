// native_probe —— 游戏进程内的原生探针（三次快照 + 回到原点检测）
//
// 目标：找出「当前选中的 BGM / ドロップ」在内存中的位置，进而定位解锁判定逻辑。
//
// 方法：快照只存「每 4KB 页的 CRC32」，不存原始字节（省内存省时间）。
//   用户操作：状态 A（选中曲1） -> 状态 B（选中曲2） -> 状态 A'（切回曲1）。
//   判据：同一页满足  crc(A) != crc(B) 且 crc(A) == crc(A')
//   即「变了又精确变回原值」。真实状态变量必然满足；堆碎片/缓存等噪声几乎不会
//   精确回到同一 CRC，因此能从数千个变化页里筛出极少数候选。
//
// 命令文件 /data/user/0/jp.gungho.pad/files/padhelper/cmd：
//   snap1 / snap2 / snap3（snap3 触发报告） / clear
//
// 只读内存，不 hook、不 patch。

#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
static int savedata_ready(void);
static int armed(void);
static int addr_mapped(unsigned long a);
#include <errno.h>

#define TAG "PADHelperNative"
#define BASE_DIR "/data/user/0/jp.gungho.pad/files/padhelper"
#define LOG_PATH BASE_DIR "/probe.log"
#define CMD_PATH BASE_DIR "/cmd"

#define PAGE 4096
#define MAX_REGIONS 4096
#define MAX_REPORT 80

static FILE *g_log;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void logline(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_lock);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    if (g_log) {
        fprintf(g_log, "%s\n", buf);
        fflush(g_log);
    }
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------- CRC32 */

static uint32_t g_table[256];
static int g_table_ready;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_table[i] = c;
    }
    g_table_ready = 1;
}

static uint32_t crc32_of(const unsigned char *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c = g_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------- 快照 */

struct region {
    uintptr_t start;
    size_t    size;
    uint32_t *crcs;
    char      label[40];
};

struct snapshot {
    int count;
    struct region regions[MAX_REGIONS];
};

static struct snapshot g_s1, g_s2, g_s3;
static int g_have1, g_have2;

static void snapshot_free(struct snapshot *s) {
    for (int i = 0; i < s->count; i++) {
        free(s->regions[i].crcs);
        s->regions[i].crcs = NULL;
    }
    s->count = 0;
}

static int want_region(const char *perms, const char *path, size_t size) {
    if (perms[0] != 'r') return 0;
    if (size == 0 || size > (size_t) 768 * 1024 * 1024) return 0;
    if (path && strstr(path, "libpadhelper-probe")) return 0;
    if (path && strstr(path, "dalvik")) return 0;         /* Java 堆 / JIT */
    if (path && strstr(path, "stack_and_tls")) return 0;  /* 线程栈：恒变 */
    if (path && strstr(path, "libpad.so")) return 1;      /* 游戏原生库 */
    if (path && strstr(path, "[heap]")) return 1;
    if (!path || path[0] == '\0' || strncmp(path, "[anon", 5) == 0) return 1;
    return 0;
}

static void take_snapshot(struct snapshot *out) {
    snapshot_free(out);
    if (!g_table_ready) crc_init();

    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        logline("snapshot: cannot open maps");
        return;
    }

    char line[512];
    size_t total = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start = 0, end = 0;
        char perms[8] = {0};
        char path[256] = {0};
        int n = sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]",
                       &start, &end, perms, path);
        if (n < 3 || end <= start) continue;

        char *p = path;
        while (*p == ' ') p++;

        if (!want_region(perms, (n >= 4) ? p : "", (size_t) (end - start))) continue;
        if (out->count >= MAX_REGIONS) break;

        size_t size = (size_t) (end - start);
        size_t pages = (size + PAGE - 1) / PAGE;
        uint32_t *crcs = malloc(pages * sizeof(uint32_t));
        if (!crcs) continue;

        const unsigned char *base = (const unsigned char *) (uintptr_t) start;
        for (size_t i = 0; i < pages; i++) {
            size_t off = i * PAGE;
            size_t len = (off + PAGE <= size) ? PAGE : (size - off);
            crcs[i] = crc32_of(base + off, len);
        }

        struct region *r = &out->regions[out->count++];
        r->start = (uintptr_t) start;
        r->size = size;
        r->crcs = crcs;
        snprintf(r->label, sizeof(r->label), "%s", (n >= 4 && p[0]) ? p : "anon");
        total += size;
    }
    fclose(f);
    logline("snapshot: regions=%d bytes=%zu", out->count, total);
}

static struct region *find_region(struct snapshot *s, uintptr_t start, size_t size) {
    for (int i = 0; i < s->count; i++) {
        if (s->regions[i].start == start && s->regions[i].size == size) {
            return &s->regions[i];
        }
    }
    return NULL;
}

static void dump_bytes(uintptr_t addr, size_t len) {
    const unsigned char *p = (const unsigned char *) addr;
    char hex[3 * 33];
    char asc[34];
    size_t n = len > 32 ? 32 : len;
    for (size_t i = 0; i < n; i++) {
        snprintf(hex + i * 3, 4, "%02x ", p[i]);
        asc[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char) p[i] : '.';
    }
    hex[n * 3] = 0;
    asc[n] = 0;
    logline("    %s |%s|", hex, asc);
}

static void report(void) {
    if (!g_have1 || !g_have2) {
        logline("report: 需要 snap1 / snap2 / snap3");
        return;
    }

    int candidates = 0;
    int reported = 0;

    for (int i = 0; i < g_s1.count; i++) {
        struct region *r1 = &g_s1.regions[i];
        struct region *r2 = find_region(&g_s2, r1->start, r1->size);
        struct region *r3 = find_region(&g_s3, r1->start, r1->size);
        if (!r2 || !r3) continue;

        size_t pages = (r1->size + PAGE - 1) / PAGE;
        for (size_t k = 0; k < pages; k++) {
            uint32_t a = r1->crcs[k];
            uint32_t b = r2->crcs[k];
            uint32_t c = r3->crcs[k];
            if (a == b) continue;      /* 没变 */
            if (c != a) continue;      /* 没变回原值 —— 噪声 */

            candidates++;
            if (reported < MAX_REPORT) {
                uintptr_t addr = r1->start + k * PAGE;
                logline("CAND page=%#llx region=%s", (unsigned long long) addr, r1->label);
                dump_bytes(addr, 32);
                reported++;
            }
        }
    }
    logline("=== report: candidates=%d (shown %d) ===", candidates, reported);
}



/* ------------------------------------------------ sSAVEDATA 里的 BGM 条目表 */

/* 反汇编 _nsBgmList(0xe50d98) 得到的结构：
 *   x21 = *(GOT[0x10de320])        // sSAVEDATA*
 *   count = *(u32*)(x21 + 0xc773c4)
 *   arr   = *(u64*)(x21 + 0xc773c8) // 每条 0x34 字节
 *   entry[+0x00] = u16 id
 *   entry[+0x0e] = u16 标志，bit0 = 可在 BGM 菜单中选择
 *   entry[+0x12] = u8  显示标志（_nsBgmList 每次进入会先清成 &0xfc）
 */
static unsigned long libpad_base(void);

static unsigned long libpad_rx_base(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    unsigned long long start = 0, end = 0;
    char perms[8] = {0}, path[256] = {0};
    unsigned long ret = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "libpad.so")) continue;
        path[0] = 0;
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]", &start, &end, perms, path) < 3) continue;
        if (strchr(perms, 'x')) { ret = (unsigned long) start; break; }
    }
    fclose(f);
    return ret;
}

static unsigned long g_base_cached = 0;
static unsigned long libpad_base(void) {
    if (!g_base_cached) g_base_cached = libpad_rx_base();
    return g_base_cached;
}

static void savedata_cmd(int do_patch) {
    unsigned long base = libpad_base();
    if (!base) {
        logline("svd: libpad r-xp not found");
        return;
    }
    void **got = (void **) (base + 0x10de320);
    unsigned long svd = (unsigned long) *got;
    logline("svd: base=%#lx savedata=%#lx patch=%d", base, svd, do_patch);
    if (!svd) return;

    unsigned int count = *(unsigned int *) (svd + 0xc773c4);
    unsigned long arr = *(unsigned long *) (svd + 0xc773c8);
    logline("svd: count=%u arr=%#lx", count, arr);
    if (!arr || count == 0 || count > 4096) {
        logline("svd: count/arr looks wrong, abort");
        return;
    }

    unsigned int minid = 0xffffffff, maxid = 0;
    unsigned int n_flag1 = 0, n_inrange = 0;
    for (unsigned int i = 0; i < count; i++) {
        unsigned char *e = (unsigned char *) (arr + (unsigned long) i * 0x34);
        unsigned int id = *(unsigned short *) e;
        unsigned int flg = *(unsigned short *) (e + 0x0e);
        if (id < minid) minid = id;
        if (id > maxid) maxid = id;
        if (flg & 1) n_flag1++;
        if (((id - 10000u) >> 4) <= 0x270u) n_inrange++;
    }
    logline("svd: minid=%u maxid=%u flag1=%u inRange=%u", minid, maxid, n_flag1, n_inrange);
    {
        char path[256];
        snprintf(path, sizeof(path), BASE_DIR "/svd.bin");
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite((void *) arr, 1, (size_t) count * 0x34, f);
            fclose(f);
            logline("svd: wrote %s (%u bytes)", path, count * 0x34u);
        } else {
            logline("svd: open %s failed", path);
        }
    }
    int patched = 0;
    for (unsigned int i = 0; i < count; i++) {
        unsigned char *e = (unsigned char *) (arr + (unsigned long) i * 0x34);
        unsigned int id = *(unsigned short *) e;
        unsigned int flg = *(unsigned short *) (e + 0x0e);
        if (i < 16) logline("entry[%u] id=%u flg=%#x b12=%#x", i, id, flg, e[0x12]);
        if (do_patch && !(flg & 1)) {
            *(unsigned short *) (e + 0x0e) = (unsigned short) (flg | 1);
            patched++;
        }
    }
    logline("svd: done count=%u patched=%d", count, patched);
}

/* ------------------------------------------------- 卡片表（皮肤 id 就在 card+0x10）
 * cSAVEDATA::updateCard() 里可以读出：count = savedata+0x83d90(u16)，
 * 条目数组 = savedata+0x83d94，步长 0x48，皮肤 id = +0x10(u16)。
 */
#define SVD_CARD_COUNT_OFF 0x83d90UL
#define SVD_CARD_ARR_OFF   0x83d94UL
#define CARD_STRIDE        0x48UL

static unsigned long savedata_ptr(void) {
    unsigned long base = libpad_base();
    if (!base) return 0;
    return (unsigned long) *(void **) (base + 0x10de320);
}

static void cards_cmd(void) {
    unsigned long svd = savedata_ptr();
    if (!svd) { logline("cards: no savedata"); return; }
    unsigned int n = *(unsigned short *) (svd + SVD_CARD_COUNT_OFF);
    unsigned long arr = svd + SVD_CARD_ARR_OFF;
    logline("cards: count=%u arr=%#lx", n, arr);
    if (n > 0xfa00) { logline("cards: count looks wrong, abort"); return; }
    for (unsigned int i = 0; i < n && i < 6; i++) {
        unsigned char *e = (unsigned char *) (arr + (unsigned long) i * CARD_STRIDE);
        logline("card[%u] id=%u f8=%u skin=%u f12=%u flg=%#x",
                i, *(unsigned int *) e, *(unsigned int *) (e + 8),
                *(unsigned short *) (e + 0x10), *(unsigned short *) (e + 0x12),
                *(unsigned short *) (e + 0x24));
    }
    char path[256];
    snprintf(path, sizeof(path), BASE_DIR "/cards.bin");
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite((void *) arr, 1, (size_t) n * CARD_STRIDE, f);
        fclose(f);
        logline("cards: wrote %s (%u bytes)", path, n * (unsigned int) CARD_STRIDE);
    }
}

static void setcard_cmd(unsigned int idx, unsigned int skin) {
    unsigned long svd = savedata_ptr();
    if (!svd) { logline("setcard: no savedata"); return; }
    unsigned int n = *(unsigned short *) (svd + SVD_CARD_COUNT_OFF);
    if (idx >= n) { logline("setcard: idx %u >= count %u", idx, n); return; }
    unsigned char *e = (unsigned char *) (svd + SVD_CARD_ARR_OFF + (unsigned long) idx * CARD_STRIDE);
    unsigned int old = *(unsigned short *) (e + 0x10);
    *(unsigned short *) (e + 0x10) = (unsigned short) skin;
    logline("setcard: card[%u] id=%u skin %u -> %u", idx, *(unsigned int *) e, old, skin);
}

/* 皮肤名表：字符串表 = *(savedata+0xc773d0) + 0x10，条目 +0x14 是名字偏移 */
static void skin_names_cmd(void) {
    unsigned long svd = savedata_ptr();
    if (!svd) { logline("names: no savedata"); return; }
    unsigned int count = *(unsigned int *) (svd + 0xc773c4);
    unsigned long arr = *(unsigned long *) (svd + 0xc773c8);
    unsigned long tbl = *(unsigned long *) (svd + 0xc773d0);
    logline("names: count=%u arr=%#lx tbl=%#lx", count, arr, tbl);
    if (!arr || !tbl || count == 0 || count > 4096) { logline("names: bad pointers"); return; }
    unsigned char *strtab = *(unsigned char **) (tbl + 0x10);
    char path[256];
    snprintf(path, sizeof(path), BASE_DIR "/skin_names.txt");
    FILE *f = fopen(path, "w");
    if (!f) { logline("names: open failed"); return; }
    for (unsigned int i = 0; i < count; i++) {
        unsigned char *e = (unsigned char *) (arr + (unsigned long) i * 0x34);
        unsigned int id = *(unsigned short *) e;
        unsigned int off = *(unsigned int *) (e + 0x14);
        unsigned int boff = *(unsigned int *) (e + 0x1c);
        const char *name = (off < 0x400000) ? (const char *) (strtab + off) : "?";
        const char *b1 = (boff < 0x400000) ? (const char *) (strtab + boff) : "?";
        fprintf(f, "%u\t%s\t%s\n", id, name, b1);
    }
    fclose(f);
    logline("names: wrote %s", path);
}

/* ------------------------------------------- 存档快照/差分：用来定位「换皮肤」写在哪 */
#define SAVE_LEN 0x1d00000UL
static unsigned char *g_save_snap = NULL;
static unsigned long g_save_len = 0;


/* 只在已映射的页里拷贝，避免读越界把游戏打死 */
struct map_range { unsigned long s, e; };
static int load_map_ranges(struct map_range *r, int max) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    unsigned long long a, b;
    char perms[8];
    while (n < max && fgets(line, sizeof(line), f)) {
        perms[0] = 0;
        if (sscanf(line, "%llx-%llx %7s", &a, &b, perms) == 3 && perms[0] == 'r') {
            r[n].s = (unsigned long) a;
            r[n].e = (unsigned long) b;
            n++;
        }
    }
    fclose(f);
    return n;
}

static void safe_copy(void *dst, unsigned long src, unsigned long len) {
    struct map_range *r = (struct map_range *) malloc(sizeof(struct map_range) * 8192);
    unsigned char *d = (unsigned char *) dst;
    memset(d, 0, len);
    if (!r) return;
    int n = load_map_ranges(r, 8192);
    unsigned long done = 0;
    while (done < len) {
        unsigned long a = src + done;
        int hit = 0;
        for (int i = 0; i < n; i++) {
            if (a >= r[i].s && a < r[i].e) {
                unsigned long chunk = r[i].e - a;
                if (chunk > len - done) chunk = len - done;
                memcpy(d + done, (void *) a, chunk);
                done += chunk;
                hit = 1;
                break;
            }
        }
        if (!hit) {
            unsigned long next = 0;
            for (int i = 0; i < n; i++) {
                if (r[i].s > a && (next == 0 || r[i].s < next)) next = r[i].s;
            }
            if (!next || next <= a) break;
            done = next - src;
        }
    }
    free(r);
}

static unsigned long save_span(unsigned long svd) {
    struct map_range *r = (struct map_range *) malloc(sizeof(struct map_range) * 8192);
    if (!r) return 0;
    int n = load_map_ranges(r, 8192);
    unsigned long end = 0;
    for (int i = 0; i < n; i++) {
        if (svd >= r[i].s && svd < r[i].e) { end = r[i].e; break; }
    }
    free(r);
    return end;
}

static void find_cmd(unsigned long val, int size) {
    unsigned long svd = savedata_ptr();
    if (!svd) { logline("find: no savedata"); return; }
    struct map_range *r = (struct map_range *) malloc(sizeof(struct map_range) * 8192);
    if (!r) return;
    int n = load_map_ranges(r, 8192);
    unsigned long limit = svd + SAVE_LEN;
    int hits = 0;
    for (int i = 0; i < n && hits < 40; i++) {
        unsigned long s = r[i].s > svd ? r[i].s : svd;
        unsigned long e = r[i].e < limit ? r[i].e : limit;
        if (e <= s) continue;
        unsigned char *p = (unsigned char *) s;
        for (unsigned long o = 0; o + (unsigned long) size <= e - s && hits < 40; o += (size == 2 ? 2 : 4)) {
            if (size == 2) {
                unsigned short v; memcpy(&v, p + o, 2);
                if (v == (unsigned short) val) { logline("find: off=%#lx", (s + o) - svd); hits++; }
            } else {
                unsigned int v; memcpy(&v, p + o, 4);
                if (v == (unsigned int) val) { logline("find: off=%#lx", (s + o) - svd); hits++; }
            }
        }
    }
    free(r);
    logline("find: val=%#lx size=%d hits=%d", val, size, hits);
}

static void save_snap_cmd(void) {
    unsigned long svd = savedata_ptr();
    if (!svd) { logline("snap: no savedata"); return; }
    if (!g_save_snap) {
        g_save_snap = (unsigned char *) malloc(SAVE_LEN);
        if (!g_save_snap) { logline("snap: malloc %lu failed", (unsigned long) SAVE_LEN); return; }
    }
    unsigned long len = SAVE_LEN;
    unsigned long end = save_span(svd);
    if (end > svd && end - svd < len) len = end - svd;
    memcpy(g_save_snap, (void *) svd, len);
    g_save_len = len;
    logline("snap: saved %lu bytes @ %#lx", g_save_len, svd);
}


/* ==== 区域差分：用来找出「某次操作改了存档里哪个字段」 ====
   只盯 [DS_REGION_OFF, +DS_REGION_LEN) 这一段，按 4 字节对齐比较，
   避免全存档差分时被计数器/堆邻居噪声淹没。 */
#define DS_REGION_OFF 0x4f0000UL
#define DS_REGION_LEN 0x60000UL
static unsigned char *g_ds_snap = NULL;

static void ds_snap_cmd(void) {
    unsigned long sv = savedata_ptr();
    if (!sv || !armed() || !savedata_ready()) { logline("dsnap: not ready"); return; }
    if (!g_ds_snap) g_ds_snap = (unsigned char *) malloc(DS_REGION_LEN);
    if (!g_ds_snap) return;
    if (!addr_mapped(sv + DS_REGION_OFF) || !addr_mapped(sv + DS_REGION_OFF + DS_REGION_LEN - 8)) {
        logline("dsnap: region unmapped");
        return;
    }
    memcpy(g_ds_snap, (void *) (sv + DS_REGION_OFF), DS_REGION_LEN);
    logline("dsnap: saved %lu bytes @ +%#lx", (unsigned long) DS_REGION_LEN, (unsigned long) DS_REGION_OFF);
}

static void ds_diff_cmd(void) {
    unsigned long sv = savedata_ptr();
    if (!sv || !g_ds_snap || !armed() || !savedata_ready()) { logline("sdiff: no snapshot"); return; }
    unsigned int *cur = (unsigned int *) (sv + DS_REGION_OFF);
    unsigned int *old = (unsigned int *) g_ds_snap;
    int n = 0;
    unsigned long cnt = DS_REGION_LEN / 4;
    for (unsigned long i = 0; i < cnt && n < 200; i++) {
        if (cur[i] != old[i]) {
            logline("dd: off=%#lx old=%#x new=%#x", (unsigned long) (DS_REGION_OFF + i * 4), old[i], cur[i]);
            n++;
        }
    }
    logline("dd: total %d", n);
}

static void save_diff_cmd(void) {
    unsigned long svd = savedata_ptr();
    if (!svd || !g_save_snap) { logline("diff: take snap first"); return; }
    unsigned char *cur = (unsigned char *) svd;
    unsigned long i = 0;
    int runs = 0;
    char path[256];
    snprintf(path, sizeof(path), "%s/diff.txt", BASE_DIR);
    FILE *f = fopen(path, "w");
    while (i < g_save_len && runs < 5000) {
        if (cur[i] == g_save_snap[i]) { i++; continue; }
        unsigned long j = i;
        while (j < g_save_len && cur[j] != g_save_snap[j]) j++;
        char old_s[80], new_s[80];
        int k, p = 0;
        int n = (int) ((j - i) < 8 ? (j - i) : 8);
        for (k = 0; k < n; k++) p += snprintf(old_s + p, sizeof(old_s) - (size_t) p, "%02x", g_save_snap[i + k]);
        p = 0;
        for (k = 0; k < n; k++) p += snprintf(new_s + p, sizeof(new_s) - (size_t) p, "%02x", cur[i + k]);
        if (f) fprintf(f, "%lx len=%lu old=%s new=%s\n", i, j - i, old_s, new_s);
        runs++;
        i = j;
    }
    if (f) fclose(f);
    logline("diff: %d runs -> %s", runs, path);
}


/* BGM 拥有位图：hasBgm() 在 savedata+0x5312f0（正 id）/ +0x5311f0（负 id，索引 id+10000）里取位 */
#define BGM_BITMAP_POS 0x5312f0
#define BGM_BITMAP_NEG 0x5311f0
#define BGM_BITMAP_LEN 0x100

/* 「New」角标：hasNewBgm() 对 (0x5312f0 & 0x531370)、(0x5311f0 & 0x531270) 两两做与、
   再 OR 归约，非零就在列表里显示闪烁的金色 New。也就是说 0x531370 / 0x531270 是
   「未读」位图，0x5312f0 / 0x5311f0 是「拥有」位图。
   我们把人造解锁的条目设成「未读」，原本就拥有的条目清掉未读位，就不会每条都闪。 */
#define NEW_BITMAP_POS 0x531370
#define NEW_BITMAP_NEG 0x531270

static unsigned char g_orig_pos[BGM_BITMAP_LEN];
static unsigned char g_orig_neg[BGM_BITMAP_LEN];
static int g_have_orig;
/* New 角标开关。默认 0=完全不碰「未读位图」。
   实测 1/3/4/5 四种方案都会让游戏把「刚获得」的皮肤重新登记一遍：
   每 50ms 稳定出现 reapply: restored 173 fields，CPU 180%，启动阶段卡在
   「通信中です…」数分钟甚至反复重载。所以默认关闭，仅保留代码供以后研究。 */
static int g_new_mode = 0;
/* 不能每 50ms 重刷：实测游戏会跟着把条目准入位清掉，双方 20Hz 对刷，
   进程 CPU 181% 且界面卡在「通信中です…」。改成「每次存档被重置后打一次」。 */
static volatile int g_new_dirty = 1;
/* 拥有位图（0x5311f0/0x5312f0）是否反复写 0xff。
   实测每 50ms 重写会触发游戏把「所有拥有但还没见过的皮肤」重新 addSkin，
   于是每个条目都变成 New。置 0 可停止这种重加，用于验证角标来源。 */
static int g_bits_mode = 1;
/* bits 2 = 拥有位图只在启动时写一次，之后不再重写。
   实测每 50ms 重写成 0xff 会让游戏持续把这些皮肤当「刚获得」重新登记，
   每 tick 清掉 173 个条目准入位，形成 20Hz 对刷。 */
static volatile int g_bits_dirty = 1;

static void bgm_bits_cmd(int do_patch) {
    unsigned long base = libpad_base();
    if (!base) { logline("bits: no libpad"); return; }
    unsigned long svd = (unsigned long) *(void **) (base + 0x10de320);
    if (!svd) { logline("bits: savedata null"); return; }
    unsigned char *pos = (unsigned char *) (svd + BGM_BITMAP_POS);
    unsigned char *neg = (unsigned char *) (svd + BGM_BITMAP_NEG);
    char h1[2 * 32 + 1], h2[2 * 32 + 1];
    for (int i = 0; i < 32; i++) {
        snprintf(h1 + i * 2, 3, "%02x", pos[i]);
        snprintf(h2 + i * 2, 3, "%02x", neg[i]);
    }
    logline("bits: pos[0:32]=%s", h1);
    logline("bits: neg[0:32]=%s", h2);
    unsigned int nz = 0;
    for (int i = 0; i < BGM_BITMAP_LEN; i++) if (pos[i]) nz++;
    logline("bits: pos nonzero bytes=%u/%#x", nz, BGM_BITMAP_LEN);
    if (do_patch && g_bits_mode) {
        if (!g_have_orig) {
            memcpy(g_orig_pos, pos, BGM_BITMAP_LEN);
            memcpy(g_orig_neg, neg, BGM_BITMAP_LEN);
            g_have_orig = 1;
        }
        memset(pos, 0xff, BGM_BITMAP_LEN);
        memset(neg, 0xff, BGM_BITMAP_LEN);
        logline("bits: patched pos+neg -> 0xff len=%#x", BGM_BITMAP_LEN);
    }
}



/* 后台静默重打：存档被服务器覆盖后 3 秒内恢复 */
static unsigned int g_reapply_count = 0;
static time_t g_boot;

/* 真正的门槛是「存档已就绪」：savedata_ready() 通过（条目数与数组指针都合理）
   并且连续稳定 3 秒，再加启动后 10 秒的硬下限。
   早期版本用固定 60 秒是因为当时还没有 savedata_ready() 校验，只能靠时间猜；
   现在存档一加载完就能动手，冷启动一般十几秒即可生效。 */
static time_t g_ready_since;

static int armed(void) {
    if (!g_boot || (time(NULL) - g_boot) < 10) return 0;
    if (!savedata_ready()) {
        g_ready_since = 0;
        return 0;
    }
    if (!g_ready_since) g_ready_since = time(NULL);
    return (time(NULL) - g_ready_since) >= 3;
}

static void quiet_reapply(void) {
    if (!armed() || !savedata_ready()) return;
    unsigned long base = libpad_base();
    if (!base) return;
    unsigned long svd = (unsigned long) *(void **) (base + 0x10de320);
    if (!svd) return;
    unsigned int count = *(unsigned int *) (svd + 0xc773c4);
    unsigned long arr = *(unsigned long *) (svd + 0xc773c8);
    unsigned int changed = 0;
    if (arr && count && count <= 4096) {
        for (unsigned int i = 0; i < count; i++) {
            unsigned short *f = (unsigned short *) (arr + (unsigned long) i * 0x34 + 0x0e);
            if (!(*f & 1)) { *f |= 1; changed++; }
        }
    }
    unsigned char *pos = (unsigned char *) (svd + BGM_BITMAP_POS);
    unsigned char *neg = (unsigned char *) (svd + BGM_BITMAP_NEG);
    if (!g_have_orig) {
        memcpy(g_orig_pos, pos, BGM_BITMAP_LEN);
        memcpy(g_orig_neg, neg, BGM_BITMAP_LEN);
        g_have_orig = 1;
    }
    if (g_bits_mode == 2 && g_bits_dirty) {
        for (int i = 0; i < BGM_BITMAP_LEN; i++) {
            if (pos[i] != 0xff) { pos[i] = 0xff; changed++; }
            if (neg[i] != 0xff) { neg[i] = 0xff; changed++; }
        }
        g_bits_dirty = 0;
    } else if (g_bits_mode == 1) {
        for (int i = 0; i < BGM_BITMAP_LEN; i++) {
            if (pos[i] != 0xff) { pos[i] = 0xff; changed++; }
            if (neg[i] != 0xff) { neg[i] = 0xff; changed++; }
        }
    }
    g_reapply_count++;
    if (changed) {
        logline("reapply: restored %u fields (tick %u)", changed, g_reapply_count);
        g_new_dirty = 1;
    }
}

/* New 角标：hasNewBgm() 判定的是「拥有位 & 未读位」。
   我们把人造解锁的条目的未读位设成 1（= 原拥有位图的补集），
   原本就拥有的条目清 0，于是只有强制解锁的条目会闪金色 New。
   mode: 0=不干预（游戏自己怎么标就怎么标） 1=只标强制解锁的 2=一个都不标 */
static int new_apply(void) {
    if (g_new_mode == 0) return 0;
    if (g_new_mode != 4 && !g_have_orig) return 0;
    if (!armed() || !savedata_ready()) return 0;
    unsigned long base = libpad_base();
    if (!base) return 0;
    unsigned long svd = (unsigned long) *(void **) (base + 0x10de320);
    if (!svd) return 0;
    unsigned long ap = svd + NEW_BITMAP_POS, an = svd + NEW_BITMAP_NEG;
    if (!addr_mapped(ap) || !addr_mapped(an + BGM_BITMAP_LEN - 8)) return 0;
    unsigned char *np = (unsigned char *) ap;
    unsigned char *nn = (unsigned char *) an;
    if (g_new_mode == 5) {
        /* 5 = 只「清除」本来就有拥有权的条目的未读位（用打补丁前的快照做 AND-NOT），
           绝不把某位从 0 写成 1 —— 因为「0→1」会被游戏当成新获得，触发 addSkin 再标记 New。 */
        for (int i = 0; i < BGM_BITMAP_LEN; i++) {
            np[i] = (unsigned char) (np[i] & ~g_orig_pos[i]);
            nn[i] = (unsigned char) (nn[i] & ~g_orig_neg[i]);
        }
    } else if (g_new_mode == 4) {
        /* 4 = 把「拥有位图」覆盖到的条目的未读位全部清掉：
           本来拥有的皮肤不再闪 New，只有我们强制解锁（拥有位=0）的条目还留着 New，
           玩家在列表里选中一次后由游戏自己清掉，符合游戏原本的语义。
           不做任何 50ms 重刷，所以不会和游戏对刷。 */
        unsigned char *op = (unsigned char *) (svd + BGM_BITMAP_POS);
        unsigned char *on = (unsigned char *) (svd + BGM_BITMAP_NEG);
        if (!addr_mapped((unsigned long) op) || !addr_mapped((unsigned long) on + BGM_BITMAP_LEN - 8)) return 0;
        for (int i = 0; i < BGM_BITMAP_LEN; i++) {
            np[i] = (unsigned char) (np[i] & ~op[i]);
            nn[i] = (unsigned char) (nn[i] & ~on[i]);
        }
    } else {
        /* 3 = 只动皮肤那一组（0x531270），BGM 组不动，便于分开验证是哪一组引起对刷 */
        for (int i = 0; i < BGM_BITMAP_LEN; i++) {
            unsigned char wp = (g_new_mode == 1) ? (unsigned char) ~g_orig_pos[i] : np[i];
            unsigned char wn = (g_new_mode == 1 || g_new_mode == 3)
                                   ? (unsigned char) ~g_orig_neg[i] : nn[i];
            if (np[i] != wp) np[i] = wp;
            if (nn[i] != wn) nn[i] = wn;
        }
    }
    logline("new: applied mode=%d pos=%02x%02x neg=%02x%02x (orig neg=%02x%02x)",
            g_new_mode, np[0], np[1], nn[0], nn[1], g_orig_neg[0], g_orig_neg[1]);
    return 1;
}

/* ---------------------------------------------- 代码补丁（不受存档重载影响） */

static int g_auto = 1;   /* 默认开启：服务器每次回推存档都会把解锁刷掉 */   /* 1 = 后台周期性重打数据补丁 */

static int patch_words(unsigned long addr, const unsigned int *words, int n) {
    long pg = sysconf(_SC_PAGESIZE);
    unsigned long page = addr & ~((unsigned long) pg - 1);
    size_t len = (size_t) (((addr + (unsigned long) n * 4 + (unsigned long) pg - 1)
                            & ~((unsigned long) pg - 1)) - page);
    if (mprotect((void *) page, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        logline("patch: mprotect rwx %#lx failed: %s", page, strerror(errno));
        return -1;
    }
    memcpy((void *) addr, words, (size_t) n * 4);
    __builtin___clear_cache((char *) addr, (char *) addr + n * 4);
    mprotect((void *) page, len, PROT_READ | PROT_EXEC);
    return 0;
}

/* hasBgm(int) -> 1；_nsBgmList 两处 entry 标志门槛 -> NOP */
static void code_patch_cmd(void) {
    unsigned long base = libpad_base();
    if (!base) { logline("cpatch: no libpad"); return; }
    static const unsigned int ret1[] = {0x52800020u, 0xd65f03c0u};  /* mov w0,#1 ; ret */
    static const unsigned int nop[] = {0xd503201fu};
    int r1 = patch_words(base + 0xa70c9c, ret1, 2);
    int r2 = patch_words(base + 0xe50f4c, nop, 1);
    int r3 = patch_words(base + 0xe5103c, nop, 1);
    logline("cpatch: hasBgm=%d gate1=%d gate2=%d", r1, r2, r3);
}

/* ------------------------------------------------------- 整段导出 libpad.so */

/* dumppad：把 libpad.so 的每个内存段原样写到 padhelper/ 目录。
 * 为什么在进程内做：外部读 /proc/<pid>/mem 会触发游戏的反调试自退出
 * （实测 open 后进程立刻消失，无 tombstone、无 SIGSEGV），只有进程内读自己才安全。 */
static void dump_libpad(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        logline("dumppad: cannot open maps");
        return;
    }
    char line[512];
    int idx = 0;
    size_t total = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start = 0, end = 0;
        char perms[8] = {0};
        char path[256] = {0};
        int n = sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]",
                       &start, &end, perms, path);
        if (n < 3 || end <= start) continue;
        char *p = path;
        while (*p == ' ') p++;
        if (!strstr(p, "libpad.so")) continue;
        size_t size = (size_t) (end - start);
        if (size > (size_t) 96 * 1024 * 1024) continue;

        char out[128];
        snprintf(out, sizeof(out), BASE_DIR "/pad_%02d_%s.bin", idx, perms);
        FILE *o = fopen(out, "wb");
        if (!o) {
            logline("dumppad: cannot create %s", out);
            continue;
        }
        const unsigned char *src = (const unsigned char *) (uintptr_t) start;
        size_t written = 0;
        const size_t CH = 1u << 20;
        while (written < size) {
            size_t chunk = size - written;
            if (chunk > CH) chunk = CH;
            if (fwrite(src + written, 1, chunk, o) != chunk) break;
            written += chunk;
        }
        fclose(o);
        logline("dumppad: %s %#llx-%#llx size=%zu written=%zu -> %s",
                perms, start, end, size, written, out);
        total += written;
        idx++;
    }
    fclose(f);
    logline("dumppad: done segments=%d total=%zu bytes", idx, total);
}

/* --------------------------------------------------------------- 命令循环 */

static char *read_cmd(void) {
    static char buf[64];
    FILE *f = fopen(CMD_PATH, "r");
    if (!f) return NULL;
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = 0;
    }
    return buf;
}


static int addr_mapped(unsigned long a) {
    a &= 0x00ffffffffffffffUL;   /* MTE 把 tag 放在高 8 位，比对映射表前必须剥掉 */
    struct map_range *r = (struct map_range *) malloc(sizeof(struct map_range) * 8192);
    if (!r) return 0;
    int n = load_map_ranges(r, 8192);
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (a >= r[i].s && a + 8 <= r[i].e) { ok = 1; break; }
    }
    free(r);
    return ok;
}

/* 通用读写：直接按 savedata 偏移poke（用于验证「皮肤选择字段」） */
static void mem_read_cmd(unsigned long off, unsigned long len) {
    unsigned long svd = savedata_ptr();
    if (!svd) { logline("r: no savedata"); return; }
    if (!addr_mapped(svd + off)) { logline("r: off=%#lx unmapped", off); return; }
    if (!addr_mapped(svd + off + len - 1)) { logline("r: tail unmapped, clamp"); len = 8; }
    unsigned char *p = (unsigned char *) (svd + off);
    char buf[200];
    int n = 0;
    if (len > 64) len = 64;
    for (unsigned long i = 0; i < len; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t) n, "%02x", p[i]);
    logline("r: off=%#lx [%lu] %s", off, len, buf);
}

static void mem_write_cmd(int width, unsigned long off, unsigned long val) {
    unsigned long svd = savedata_ptr();
    if (!svd) { logline("w: no savedata"); return; }
    if (!addr_mapped(svd + off)) { logline("w: off=%#lx unmapped", off); return; }
    if (width == 2) {
        unsigned short old = *(unsigned short *) (svd + off);
        *(unsigned short *) (svd + off) = (unsigned short) val;
        logline("w16: off=%#lx %#x -> %#x", off, old, (unsigned) val);
    } else {
        unsigned int old = *(unsigned int *) (svd + off);
        *(unsigned int *) (svd + off) = (unsigned int) val;
        logline("w32: off=%#lx %#x -> %#x", off, old, (unsigned) val);
    }
}

/* ==== 影子配置：每 50ms 把指定 savedata 偏移强制成「合法值」====
   游戏或服务器写回之后立刻纠正，使「上报合法、本地显示目标」同时成立。 */
#define PIN_MAX 8
struct pin_ent { unsigned long off; int width; unsigned long val; int active; };
static struct pin_ent g_pins[PIN_MAX];

static void pin_apply(void) {
    if (!armed()) return;   /* 存档没准备好之前绝不写：会踩坏游戏初始化 */
    unsigned long svd = savedata_ptr();
    if (!svd) return;
    for (int i = 0; i < PIN_MAX; i++) {
        if (!g_pins[i].active) continue;
        unsigned long a = svd + g_pins[i].off;
        if (g_pins[i].width == 2) {
            unsigned short *p = (unsigned short *) a;
            unsigned short want = (unsigned short) g_pins[i].val;
            if (*p != want) *p = want;
        } else {
            unsigned int *p = (unsigned int *) a;
            unsigned int want = (unsigned int) g_pins[i].val;
            if (*p != want) *p = want;
        }
    }
}

static void pin_set_cmd(int width, unsigned long off, unsigned long val) {
    for (int i = 0; i < PIN_MAX; i++) {
        if (g_pins[i].active && g_pins[i].off == off && g_pins[i].width == width) {
            g_pins[i].val = val;
            logline("pin: off=%#lx w=%d -> %#lx (updated)", off, width, val);
            return;
        }
    }
    for (int i = 0; i < PIN_MAX; i++) {
        if (!g_pins[i].active) {
            g_pins[i].off = off; g_pins[i].width = width;
            g_pins[i].val = val; g_pins[i].active = 1;
            logline("pin: off=%#lx w=%d val=%#lx (slot %d)", off, width, val, i);
            return;
        }
    }
    logline("pin: full");
}

static void pin_clear_cmd(void) {
    for (int i = 0; i < PIN_MAX; i++) g_pins[i].active = 0;
    logline("pin: cleared");
}

/* savedata 何时才算「加载完成」：条目计数合理且数组指针落在已映射区。
   启动早期 GOT 里可能已经有非空指针，但结构体还没初始化，此时写入会踩坏游戏。 */
static int savedata_ready(void) {
    unsigned long sv = savedata_ptr();
    if (!sv || !addr_mapped(sv + 0xc773c8)) return 0;
    unsigned int cnt = *(unsigned int *) (sv + 0xc773c4);
    if (cnt < 100 || cnt > 2000) return 0;
    unsigned long arr = *(unsigned long *) (sv + 0xc773c8);
    if (!arr) return 0;
    unsigned long arr_u = arr & 0x00ffffffffffffffUL;
    if (!addr_mapped(arr_u) || !addr_mapped(arr_u + (unsigned long) cnt * 0x34 - 1)) return 0;
    return 1;
}

/* ==== 皮肤影子配置（按队伍） ====
   实测：每支队伍在同一条 0x60 字节的记录里存着自己的皮肤索引，
   队伍 1 = savedata+0x52f3d8，队伍 2 = +0x52f438（步长 0x60），以此类推。
   服务器只在「进入选关画面」时校验，显示要等关卡加载时才读，
   所以平时把每个槽位强制成该队自己的「合法值」，按音量键时临时切成目标值。 */
#define SKIN_BASE 0x52f3d8UL
#define SKIN_STRIDE 0x60UL
#define SKIN_TEAMS 10
static int g_skin_mode;

/* ==== 体力触发 ====
   实测存档里 +0x52b1d8 是 u16「当前体力」、+0x52b1da 是「最大体力」
   （239/239 时读到 `ef00 ef00`）。进关卡点「挑戦する」时只会变小，
   而自然恢复/吃药只会变大，所以「数值变小」就是「玩家确认进关」的精确信号，
   不依赖玩家点了屏幕哪个位置。 */
#define STA_OFF 0x52b1d8UL
#define STA_HOLD_MS 15000UL
static int g_sta_prev = -1;
static unsigned long g_hold_until_ms;

static unsigned long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long) ts.tv_sec * 1000UL + (unsigned long) (ts.tv_nsec / 1000000L);
}

static void sta_watch(void) {
    unsigned long svd = savedata_ptr();
    if (!svd || !armed() || !savedata_ready()) return;
    if (!addr_mapped(svd + STA_OFF)) return;
    int v = (int) *(unsigned short *) (svd + STA_OFF);
    if (g_sta_prev < 0) {
        logline("sta: init=%d", v);
    } else if (v < g_sta_prev) {
        g_hold_until_ms = now_ms() + STA_HOLD_MS;
        logline("sta: %d -> %d, 进入关卡 -> 目标皮肤 %lums", g_sta_prev, v, STA_HOLD_MS);
    }
    g_sta_prev = v;
}
static unsigned int g_legal[SKIN_TEAMS];
static unsigned int g_target[SKIN_TEAMS];
static int g_have[SKIN_TEAMS];

static void skin_apply(void) {
    unsigned long svd = savedata_ptr();
    if (!svd || !armed() || !savedata_ready()) return;
    for (int i = 0; i < SKIN_TEAMS; i++) {
        unsigned long off = SKIN_BASE + (unsigned long) i * SKIN_STRIDE;
        if (!addr_mapped(svd + off)) continue;
        unsigned int *p = (unsigned int *) (svd + off);
        unsigned int v = *p;
        if (!g_have[i]) {
            if (v == 0 || v >= 0x400) continue;
            g_legal[i] = v;
            g_have[i] = 1;
            logline("skin[%d]: legal=%u", i + 1, v);
            continue;
        }
        if (v != g_legal[i] && v > 0 && v < 0x400 && v != g_target[i]) {
            g_target[i] = v;
            logline("skin[%d]: captured target=%u", i + 1, v);
        }
        unsigned int want = ((g_skin_mode || now_ms() < g_hold_until_ms) && g_target[i]) ? g_target[i] : g_legal[i];
        if (v != want) *p = want;
    }
}

/* 影子配置：skin.cfg 由 Java 侧写入（音量键切换），探针按文件内容重建 pin 列表。
   格式：每行一条 `pin <hexoff> <2|4> <val>`，其余行忽略。 */
#define SKIN_CFG BASE_DIR "/skin.cfg"
static time_t g_cfg_mtime;
static long g_cfg_size = -1;

static void load_skin_cfg(void) {
    struct stat st;
    if (stat(SKIN_CFG, &st) != 0) return;
    if (st.st_mtime == g_cfg_mtime && (long) st.st_size == g_cfg_size) return;
    g_cfg_mtime = st.st_mtime;
    g_cfg_size = (long) st.st_size;
    FILE *f = fopen(SKIN_CFG, "r");
    if (!f) return;
    for (int i = 0; i < PIN_MAX; i++) g_pins[i].active = 0;
    char line[160];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long off = 0, val = 0;
        int w = 4;
        if (strncmp(line, "bits ", 5) == 0) {
            g_bits_mode = atoi(line + 5);
            if (g_bits_mode < 0 || g_bits_mode > 2) g_bits_mode = 1;
            g_bits_dirty = 1;
            logline("cfg: bits mode=%d", g_bits_mode);
            continue;
        }
        if (strncmp(line, "new ", 4) == 0) {
            int m = atoi(line + 4);
            if (m >= 0 && m <= 5) {
                g_new_mode = m;
                g_new_dirty = 1;
                logline("cfg: new mode=%d", m);
            }
            continue;
        }
        if (strncmp(line, "mode ", 5) == 0) {
            g_skin_mode = (strncmp(line + 5, "target", 6) == 0) ? 1 : 0;
            logline("cfg: skin mode=%s", g_skin_mode ? "target" : "legal");
            continue;
        }
        if (sscanf(line, "pin %lx %d %lx", &off, &w, &val) == 3) {
            if (n < PIN_MAX) {
                g_pins[n].off = off;
                g_pins[n].width = w;
                g_pins[n].val = val;
                g_pins[n].active = 1;
                n++;
            }
        }
    }
    fclose(f);
    logline("cfg: loaded %d pins", n);
}

static void *worker(void *arg) {
    (void) arg;
    char last[64] = {0};
    g_boot = time(NULL);
    logline("worker started, watching %s", CMD_PATH);

    for (;;) {
        /* 注入会命中多个进程；只有真正加载了 libpad 的那个才处理命令，
           且必须等到 libpad 被 dlopen 之后（PAD 启动后才会加载）。 */
        if (!libpad_base()) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 200L * 1000L * 1000L;
            nanosleep(&ts, NULL);
            continue;
        }
        /* 启动后自动解锁一次：等待 savedata 真正加载完成（count 合理）再动手 */
        {
            static int unlocked = 0;
            if (!unlocked) {
                if (armed() && savedata_ready()) {
                    bgm_bits_cmd(1);
                    savedata_cmd(1);
                    unlocked = 1;
                    logline("auto-unlock done");
                }
            }
        }
        char *cmd = read_cmd();
        if (cmd && cmd[0] && strcmp(cmd, last) != 0) {
            snprintf(last, sizeof(last), "%s", cmd);
            logline("cmd=%s", cmd);
            if (strcmp(cmd, "snap1") == 0) {
                take_snapshot(&g_s1);
                g_have1 = 1;
                logline("snap1 saved");
            } else if (strcmp(cmd, "snap2") == 0) {
                take_snapshot(&g_s2);
                g_have2 = 1;
                logline("snap2 saved");
            } else if (strcmp(cmd, "snap3") == 0) {
                take_snapshot(&g_s3);
                report();
            } else if (strcmp(cmd, "svd") == 0) {
                savedata_cmd(0);
            } else if (strcmp(cmd, "patchcode") == 0) {
                code_patch_cmd();
            } else if (strcmp(cmd, "auto") == 0) {
                g_auto = 1;
                logline("auto: on");
            } else if (strcmp(cmd, "svdbits") == 0) {
                bgm_bits_cmd(0);
            } else if (strcmp(cmd, "unlockbits") == 0) {
                bgm_bits_cmd(1);
            } else if (strcmp(cmd, "unlockbgm") == 0) {
                savedata_cmd(1);
            } else if (strcmp(cmd, "dumppad") == 0) {
                dump_libpad();
            } else if (strncmp(cmd, "r ", 2) == 0) {
                unsigned long off = 0, len = 8;
                if (sscanf(cmd + 2, "%lx %lx", &off, &len) >= 1) mem_read_cmd(off, len);
            } else if (strncmp(cmd, "w16 ", 4) == 0) {
                unsigned long off = 0, val = 0;
                if (sscanf(cmd + 4, "%lx %lx", &off, &val) == 2) mem_write_cmd(2, off, val);
            } else if (strncmp(cmd, "w32 ", 4) == 0) {
                unsigned long off = 0, val = 0;
                if (sscanf(cmd + 4, "%lx %lx", &off, &val) == 2) mem_write_cmd(4, off, val);
            } else if (strncmp(cmd, "find ", 5) == 0) {
                unsigned long val = 0; int size = 2;
                if (sscanf(cmd + 5, "%lx %d", &val, &size) >= 1) find_cmd(val, size);
            } else if (strcmp(cmd, "savsnap") == 0) {
                save_snap_cmd();
            } else if (strcmp(cmd, "savdiff") == 0) {
                save_diff_cmd();
            } else if (strcmp(cmd, "dsnap") == 0) {
                ds_snap_cmd();
            } else if (strcmp(cmd, "ddiff") == 0) {
                ds_diff_cmd();
            } else if (strcmp(cmd, "cards") == 0) {
                cards_cmd();
            } else if (strcmp(cmd, "svdnames") == 0) {
                skin_names_cmd();
            } else if (strncmp(cmd, "setcard ", 8) == 0) {
                unsigned int idx = 0, sk = 0;
                if (sscanf(cmd + 8, "%u %u", &idx, &sk) == 2) setcard_cmd(idx, sk);
                else logline("setcard: usage setcard <idx> <skin>");
            } else if (strncmp(cmd, "pin ", 4) == 0) {
                unsigned long off = 0, val = 0; int w = 4;
                if (sscanf(cmd + 4, "%lx %d %lx", &off, &w, &val) == 3) pin_set_cmd(w, off, val);
                else logline("pin: usage pin <off> <2|4> <val>");
            } else if (strncmp(cmd, "newclear", 8) == 0) {
                unsigned long sv = savedata_ptr();
                if (sv && addr_mapped(sv + NEW_BITMAP_POS) && addr_mapped(sv + NEW_BITMAP_NEG)) {
                    memset((void *) (sv + NEW_BITMAP_POS), 0, BGM_BITMAP_LEN);
                    memset((void *) (sv + NEW_BITMAP_NEG), 0, BGM_BITMAP_LEN);
                    logline("newclear: both new bitmaps zeroed");
                } else {
                    logline("newclear: savedata not ready");
                }
            } else if (strcmp(cmd, "unpin") == 0) {
                pin_clear_cmd();
            } else if (strncmp(cmd, "sta", 3) == 0) {
                unsigned long sv = savedata_ptr();
                int v = (sv && addr_mapped(sv + STA_OFF)) ? (int) *(unsigned short *) (sv + STA_OFF) : -1;
                long left = (g_hold_until_ms > now_ms()) ? (long) (g_hold_until_ms - now_ms()) : 0;
                logline("sta: pid=%d cur=%d prev=%d hold_left=%ldms mode=%d",
                        (int) getpid(), v, g_sta_prev, left, g_skin_mode);
            } else if (strncmp(cmd, "new ", 4) == 0) {
                int m = -1;
                if (sscanf(cmd + 4, "%d", &m) == 1 && m >= 0 && m <= 5) {
                    g_new_mode = m;
                    g_new_dirty = 1;
                    logline("new: mode=%d", m);
                } else {
                    logline("new: usage new <0|1|2>");
                }
            } else if (strcmp(cmd, "clear") == 0) {
                snapshot_free(&g_s1);
                snapshot_free(&g_s2);
                snapshot_free(&g_s3);
                g_have1 = g_have2 = 0;
                logline("cleared");
            }
        }
        /* 50ms 一轮：把「服务器覆盖 -> 我们改回」的窗口压到最小 */
        if (g_auto) quiet_reapply();
        if (g_new_mode == 5) {
            static time_t last5 = 0;
            time_t now5 = time(NULL);
            if (now5 - last5 >= 2) { last5 = now5; g_new_dirty = 1; }
        }
        if (g_new_dirty && new_apply()) g_new_dirty = 0;
        load_skin_cfg();
        pin_apply();
        skin_apply();
        sta_watch();
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 50L * 1000L * 1000L;
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

__attribute__((constructor))
static void padhelper_native_init(void) {
    mkdir(BASE_DIR, 0770);
    g_log = fopen(LOG_PATH, "a");
    logline("native probe loaded: pid=%d uid=%d", (int) getpid(), (int) getuid());

    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL) == 0) {
        pthread_detach(th);
    } else {
        logline("failed to start worker");
    }
}
