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
#include <link.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
static int savedata_ready(void);
static int armed(void);
static int addr_mapped(unsigned long a);
/* 当前账号凭据哈希（data048.bin 的 FNV-1a）。定义在文件后半段的 user_slot 区块，
   这里提前声明：多账号的本地皮肤值要按账号分开。 */
static unsigned long long cred_hash(void);
/* 每账号只做一次的徽章相关状态，同样要在换账号时重置，所以提前声明。 */
static volatile int g_badge_once;
static volatile long g_quiet_until;
#include <errno.h>
#include <dirent.h>

#define TAG "PADHelperNative"

/* ============ 三个功能开关（skin.cfg 的 skin / bgm / badge） ============
   默认全关：用户不显式打开，探针就什么都不做，行为和「没装模块」一致。

   三个开关走三条独立的路径，各自只碰自己那一半：
     skin  → neg 段拥有位图 + 皮肤条目的准入位（entry+0x0e bit0，id < 10000）
     bgm   → pos 段拥有位图 + BGM 条目的准入位（id >= 10000）
     badge → 队伍记录的牌子位（0x5f）与 hasBadge 钩子

   v2.4 及以前把 skin/bgm 合成一个 (skin||bgm) 门控去驱动上述两段，
   于是「只开皮肤」会把 BGM 位图也打满 0xff，「只开 BGM」会把皮肤准入位全置 1，
   用户看到的就是「开关怎么拨都一起生效」。v2.5 起严格分开。

   v2.6 起两段位图也分开碰（见下面 bitmap 说明）。v2.6 第一次分配时把两边弄反了
   （真机实测：关 skin 消失的是 BGM 的解锁），v2.6.1 更正为 skin→neg、bgm→pos。
   如果以后又发现反了，在 skin.cfg 里加 `bitmapswap 1` 即可对调，不用重发版。 */
static int g_skin_enabled = 0;
static int g_bgm_enabled = 0;
static int g_badge_enabled = 0;

/* 数据目录：优先外部存储（用户可直接用文件管理器改 cmd/skin.cfg，也避开游戏私有目录）。
   按顺序探测，哪个能写就用哪个；都失败才回落到私有目录。 */
#define BASE_DIR_INTERNAL "/data/user/0/jp.gungho.pad/files/padhelper"
static char g_base[256];
static const char *base_dir(void) { return g_base[0] ? g_base : BASE_DIR_INTERNAL; }
static const char *log_path(void) {
    static char p[320];
    snprintf(p, sizeof(p), "%s/probe.log", base_dir());
    return p;
}
static const char *cmd_path(void) {
    static char p[320];
    snprintf(p, sizeof(p), "%s/cmd", base_dir());
    return p;
}

#define PAGE 4096
#define MAX_REGIONS 4096
#define MAX_REPORT 80

static FILE *g_log;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* 日志轮转：probe.log 是纯追加的，不限制的话长年累月会无限涨
   （真机实测 52 次启动 = 1.45 MB，其中 92% 是 qd_report 每 5 秒刷的重复行）。
   超过 PROBE_LOG_MAX 就滚动成 probe.log.1，只保留上一代，总量上限约 2×PROBE_LOG_MAX。
   轮转在 logline 内部做，因为所有写入都从这里过，天然串行（外面有 g_lock）。 */
#define PROBE_LOG_MAX (1024L * 1024L)
static long g_log_bytes;

static void log_rotate(void) {
    /* 必须在 g_lock 内调用 */
    char path[320], old[330];
    snprintf(path, sizeof(path), "%s", log_path());
    snprintf(old, sizeof(old), "%s.1", path);
    if (g_log) { fclose(g_log); g_log = NULL; }
    remove(old);                 /* 只留一代，旧的直接丢 */
    rename(path, old);           /* rename 失败也无所谓，下面重开就是接着写 */
    g_log = fopen(path, "a");
    g_log_bytes = 0;
    if (g_log) {
        fprintf(g_log, "----- log rotated, previous saved as %s -----\n", old);
        fflush(g_log);
    }
}

static void logline(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_lock);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    if (!g_log) {
        /* 轮转失败/首次打开失败后重试一次，否则整个进程就再没有文件日志了 */
        g_log = fopen(log_path(), "a");
        g_log_bytes = 0;
    }
    if (g_log) {
        int n = fprintf(g_log, "%s\n", buf);
        fflush(g_log);
        if (n > 0) g_log_bytes += n;
        if (g_log_bytes > PROBE_LOG_MAX) log_rotate();
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
        snprintf(path, sizeof(path), "%s/svd.bin", base_dir());
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
        /* 准入位（flg bit0）：v2.5 的真机 svd.bin 证明它**不是只属于皮肤**——
           378 条里 198 条皮肤（id 0..197）+ 180 条 BGM（id>=10000）全部被置成了 1。
           也就是说这份表的每一条都有「已准入」位，谁被置 1 谁就在对应列表里可选。
           所以按 id 分流，各管各的列表：
             - skin 开关 → 只给皮肤条目（id < 10000）置位；
             - bgm  开关 → 只给 BGM 条目（id >= 10000）置位，另外还要打位图。
           以前这里既写成 (skin||bgm)，又没判 id，两个 bug 叠在一起，
           表现就是「只开一个开关，另一个列表也全解锁」。 */
        int want = (id < 10000u) ? g_skin_enabled : g_bgm_enabled;
        if (do_patch && want && !(flg & 1)) {
            *(unsigned short *) (e + 0x0e) = (unsigned short) (flg | 1);
            patched++;
        }
    }
    logline("svd: done count=%u patched=%d (skin=%d bgm=%d)", count, patched,
            g_skin_enabled, g_bgm_enabled);
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
    snprintf(path, sizeof(path), "%s/cards.bin", base_dir());
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
    snprintf(path, sizeof(path), "%s/skin_names.txt", base_dir());
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

/* bfind <u32> [off] [len]：在 [off, off+len) 里找 u32，
   并打印命中处前后 32 字节，用来验证「某字段在存档里存了几份、周边长什么样」。
   和 find 的区别：find 全存档扫、2/4 字节混用；bfind 只在指定区间、按 u32 对齐、带上下文。 */
static void bfind_cmd(unsigned long val, unsigned long off, unsigned long len) {
    unsigned long svd = savedata_ptr();
    if (!svd) { logline("bfind: no savedata"); return; }
    if (!addr_mapped(svd + off)) { logline("bfind: off=%#lx unmapped", off); return; }
    if (!addr_mapped(svd + off + len - 1)) { len = 0x1000; logline("bfind: tail unmapped, clamp len=%#lx", len); }
    unsigned char *base = (unsigned char *) (svd + off);
    int hits = 0;
    for (unsigned long o = 0; o + 4 <= len && hits < 40; o += 4) {
        unsigned int v; memcpy(&v, base + o, 4);
        if (v != (unsigned int) val) continue;
        hits++;
        char buf[200]; int n = 0;
        unsigned long c0 = (o >= 16) ? o - 16 : 0;
        for (unsigned long i = c0; i < o + 16 && n < 190; i++)
            n += snprintf(buf + n, sizeof(buf) - (size_t) n, "%c%02x", (i == o) ? '[' : ' ', base[i]);
        logline("bfind: off=%#lx  %s", off + o, buf);
    }
    logline("bfind: val=%#lx off=%#lx len=%#lx hits=%d", val, off, len, hits);
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
    snprintf(path, sizeof(path), "%s/diff.txt", base_dir());
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


/* 两段拥有位图（各 0x100 字节）。
 *
 * 注意：早期笔记（docs/阶段性总结-2026-10-18.md）把 0x5312f0 记成「正 id 拥有位图」、
 * 0x5311f0 记成「负 id」—— 这个标注是反的，v2.6 照着它做就翻车了，2.6.1 已更正。
 * 真正能对上的证据是 New 位图：0x531370 = 0x5312f0 + 0x80、0x531270 = 0x5311f0 + 0x80，
 * 两对是同一侧；而 isNew(id) 明确写「id < 10000 查 0x531270，否则查 0x531370」，
 * 即 正 id(皮肤) ↔ 0x...270 这一侧 ↔ 本文件的 NEG 段，
 *    负 id(BGM)  ↔ 0x...370 这一侧 ↔ 本文件的 POS 段。 */
#define BGM_BITMAP_POS 0x5312f0
#define BGM_BITMAP_NEG 0x5311f0
#define BGM_BITMAP_LEN 0x100

/* 两段位图分别对应哪一边 —— 这是 2.6.1 用真机反馈定下来的，和 2.6 正好相反。
 *
 * v2.6 的假设是「pos(0x5312f0) 归皮肤、neg(0x5311f0) 归 BGM」（照
 * docs/阶段性总结-2026-10-18.md 的 isNew 位号推断），真机实测**反了**：
 *   只把 skin 关掉时，游戏里消失的是 BGM 的解锁（BGM 列表变成只显示已拥有），
 *   而皮肤列表照样能选未拥有的，选中后服务器不认、报错重置；
 *   把 bgm 关掉则反过来 —— 皮肤锁定、BGM 还开着。
 * 「选中未拥有项会报错重置」说明准入位那一层是对的（id<10000 才是皮肤），
 * 唯一错的就是这两段位图谁归谁：pos 其实归 BGM，neg 才归皮肤。
 *
 * 也正因为皮肤列表在 skin 关掉后仍然可选，可证「皮肤可选」靠的就是 neg 段的位：
 * 关 skin 时我们只清了 pos + 皮肤条目的准入位，neg 一直保持 0xff，皮肤就还开着。
 *
 * 所以 v2.6.1 起：
 *     skin → neg(0x5311f0)      bgm → pos(0x5312f0)
 * 如果以后哪个版本又发现反了，skin.cfg 里加一行 `bitmapswap 1` 就能对调回去，
 * 不用重新发版。（默认 0 就是上面这套已经验证过的映射。） */
static int g_bitmap_swap = 0;

/* 「New」角标：hasNewBgm() 对 (0x5312f0 & 0x531370)、(0x5311f0 & 0x531270) 两两做与、
   再 OR 归约，非零就在列表里显示闪烁的金色 New。也就是说 0x531370 / 0x531270 是
   「未读」位图，0x5312f0 / 0x5311f0 是「拥有」位图。
   我们把人造解锁的条目设成「未读」，原本就拥有的条目清掉未读位，就不会每条都闪。 */
#define NEW_BITMAP_POS 0x531370
#define NEW_BITMAP_NEG 0x531270

/* 默认：皮肤 = neg(0x5311f0)，BGM = pos(0x5312f0)。理由见上面 2.6.1 的说明。 */
static unsigned char *skin_bitmap(unsigned long svd) {
    return (unsigned char *) (svd + (g_bitmap_swap ? BGM_BITMAP_POS : BGM_BITMAP_NEG));
}
static unsigned char *bgm_bitmap(unsigned long svd) {
    return (unsigned char *) (svd + (g_bitmap_swap ? BGM_BITMAP_NEG : BGM_BITMAP_POS));
}
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

/* ================= 关闭开关时不做任何「还原」 =================
 *
 * 2.6.1 及以前会在第一次打补丁前把拥有位图和条目准入位存到
 * bits_orig[_<cred>].bin，关掉开关时再写回去。多账号下这是错的：
 *   - 旧版遗留的 bits_orig.bin 会被任意账号「认领」；
 *   - 按账号的快照被判定污染时，还会拿另一个账号的 bits_orig.bin 去「自愈」。
 * 结果就是把 A 账号的清单写进 B 账号的存档，B 里能选到未拥有的皮肤/BGM，
 * 选中即被服务器拒绝、报错重置——这正是 2.6.2 要修的问题。
 *
 * 现在改成：只在开关打开时打补丁；开关一关就**停手**，不读也不写任何
 * 「原始清单」。游戏下次启动会从服务器重新下发当前账号的真实数据
 * （真机日志里，659 账号的真实皮肤段只有 3 个非零字节、BGM 段 129 个，
 * 而不是我们刷的 0xff），所以不需要模块去「还原」。
 * 代价是关掉开关后要重启游戏才生效，设置界面会明确提示。
 */
/* 皮肤那一侧的位图（2.6.1 起是 neg 段，见文件头 bitmap 说明）。为什么要碰它：
   皮肤列表的可选性同样吃这一段，只打准入位是不够的 —— 2.6 的真机反馈里，
   皮肤关掉后准入位已经按快照还原、皮肤列表却照样能选，就是因为当时清的其实是
   另一段位图。
   和 bgm 对称：只整段刷自己那一侧，关掉时完全不动。 */
static void skin_bits_cmd(int do_patch) {
    unsigned long base = libpad_base();
    if (!base) { logline("skinbits: no libpad"); return; }
    unsigned long svd = (unsigned long) *(void **) (base + 0x10de320);
    if (!svd) { logline("skinbits: savedata null"); return; }
    unsigned char *sb = skin_bitmap(svd);
    if (!addr_mapped((unsigned long) sb)) { logline("skinbits: bitmap unmapped"); return; }
    unsigned int nz = 0;
    for (int i = 0; i < BGM_BITMAP_LEN; i++) if (sb[i]) nz++;
    logline("bits: skin bitmap nonzero bytes=%u/%#x", nz, BGM_BITMAP_LEN);
    /* 只碰自己这一侧；关掉时完全不动（2.6.2 起没有“还原”这一步）。 */
    if (!do_patch || !g_bits_mode || !g_skin_enabled) return;
    memset(sb, 0xff, BGM_BITMAP_LEN);
    logline("bits: patched skin bitmap -> 0xff len=%#x", BGM_BITMAP_LEN);
}

static void bgm_bits_cmd(int do_patch) {
    unsigned long base = libpad_base();
    if (!base) { logline("bits: no libpad"); return; }
    unsigned long svd = (unsigned long) *(void **) (base + 0x10de320);
    if (!svd) { logline("bits: savedata null"); return; }
    unsigned char *pos = (unsigned char *) (svd + BGM_BITMAP_POS);
    unsigned char *neg = (unsigned char *) (svd + BGM_BITMAP_NEG);
    unsigned char *bb = bgm_bitmap(svd);
    char h1[2 * 32 + 1], h2[2 * 32 + 1];
    for (int i = 0; i < 32; i++) {
        snprintf(h1 + i * 2, 3, "%02x", pos[i]);
        snprintf(h2 + i * 2, 3, "%02x", neg[i]);
    }
    logline("bits: pos[0:32]=%s", h1);
    logline("bits: neg[0:32]=%s", h2);
    unsigned int nz = 0;
    for (int i = 0; i < BGM_BITMAP_LEN; i++) if (bb[i]) nz++;
    logline("bits: bgm bitmap nonzero bytes=%u/%#x", nz, BGM_BITMAP_LEN);
    /* 只碰自己这一侧；关掉时完全不动。 */
    if (do_patch && g_bits_mode && g_bgm_enabled) {
        memset(bb, 0xff, BGM_BITMAP_LEN);
        logline("bits: patched bgm bitmap -> 0xff len=%#x", BGM_BITMAP_LEN);
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
    /* 两个都关：完全停手。不再读快照、不再“还原”——
       游戏下次启动会从服务器拿到当前账号的真实清单。 */
    if (!g_skin_enabled && !g_bgm_enabled) return;
    unsigned long base = libpad_base();
    if (!base) return;
    unsigned long svd = (unsigned long) *(void **) (base + 0x10de320);
    if (!svd) return;
    unsigned int count = *(unsigned int *) (svd + 0xc773c4);
    unsigned long arr = *(unsigned long *) (svd + 0xc773c8);
    unsigned int changed = 0;

    /* 只给「开着」的那一半条目置准入位；关着的那一半原样不动。 */
    if (arr && count && count <= 4096) {
        for (unsigned int i = 0; i < count; i++) {
            unsigned char *e = (unsigned char *) (arr + (unsigned long) i * 0x34);
            unsigned short *f = (unsigned short *) (e + 0x0e);
            unsigned int id = *(unsigned short *) e;
            int want = (id < 10000u) ? g_skin_enabled : g_bgm_enabled;
            if (want && !(*f & 1)) { *f |= 1; changed++; }
        }
    }
    /* 只刷「开着」的那一段位图；另一段保持服务器下发的原样。 */
    if (g_bgm_enabled && (g_bits_mode == 1 || (g_bits_mode == 2 && g_bits_dirty))) {
        unsigned char *bb = bgm_bitmap(svd);
        for (int i = 0; i < BGM_BITMAP_LEN; i++) {
            if (bb[i] != 0xff) { bb[i] = 0xff; changed++; }
        }
    }
    if (g_skin_enabled && (g_bits_mode == 1 || (g_bits_mode == 2 && g_bits_dirty))) {
        unsigned char *sb = skin_bitmap(svd);
        for (int i = 0; i < BGM_BITMAP_LEN; i++) {
            if (sb[i] != 0xff) { sb[i] = 0xff; changed++; }
        }
    }
    if (g_bits_mode == 2) g_bits_dirty = 0;
    g_reapply_count++;
    if (changed) {
        logline("reapply: patched %u fields (tick %u, skin=%d bgm=%d)",
                changed, g_reapply_count, g_skin_enabled, g_bgm_enabled);
        g_new_dirty = 1;
    }
}

/* New 角标实验已停用：它依赖「打补丁前的原样位图」，而 2.6.2 起不再保存
   任何原样清单。skin.cfg 里的 new 仍可解析，但不再生效。 */
static int new_apply(void) { return 0; }

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
        snprintf(out, sizeof(out), "%s/pad_%02d_%s.bin", base_dir(), idx, perms);
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
    FILE *f = fopen(cmd_path(), "r");
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

/* ==== 存档偏移监视器：只读、只打日志，绝不写入 ====
   目的：在完全不碰 .text 的前提下，回答「某个字段到底在什么时候被谁改」。
   每 50ms 采样一次，值变化就记一行（带相对启动的毫秒数），便于和游戏行为对齐。
   皮肤 = SKIN_BASE + i*0x60 + 0x00（u16），记录起点 = DECK_REC_BASE + i*0x60（+0x1c 是皮肤）。 */

static unsigned long watch_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long) ts.tv_sec * 1000UL + (unsigned long) (ts.tv_nsec / 1000000L);
}

#define WATCH_MAX 32   /* 16 队 × 2（皮肤槽 + 记录首 4 字节） */
struct watch_ent { unsigned long off; int width; int active; unsigned long last; };
static struct watch_ent g_watch[WATCH_MAX];



static unsigned long g_watch_ms0;

static void watch_add(unsigned long off, int width) {
    for (int i = 0; i < WATCH_MAX; i++) {
        if (g_watch[i].active && g_watch[i].off == off && g_watch[i].width == width) return;
    }
    for (int i = 0; i < WATCH_MAX; i++) {
        if (!g_watch[i].active) {
            g_watch[i].off = off;
            g_watch[i].width = width;
            g_watch[i].active = 1;
            g_watch[i].last = (width == 2)
                              ? *(unsigned short *) (savedata_ptr() + off)
                              : *(unsigned int *) (savedata_ptr() + off);
            logline("watch: +%#lx w%d start=%#lx", off, width, g_watch[i].last);
            return;
        }
    }
    logline("watch: FULL, +%#lx 未加入监视", off);
}

static void watch_sample(void) {
    static int any;
    if (!any) {
        for (int i = 0; i < WATCH_MAX; i++) if (g_watch[i].active) { any = 1; break; }
        if (!any) return;
    }
    /* 已变化的条目单独打一行，带「距上次变化的间隔」，便于判「单次写」还是「连写」 */
    unsigned long svd = savedata_ptr();
    if (!svd || !savedata_ready()) return;
    unsigned long now = watch_now_ms();
    for (int i = 0; i < WATCH_MAX; i++) {
        if (!g_watch[i].active) continue;
        unsigned long v = (g_watch[i].width == 2)
                          ? *(unsigned short *) (svd + g_watch[i].off)
                          : *(unsigned int *) (svd + g_watch[i].off);
        if (v != g_watch[i].last) {
            logline("watch: +%#lx w%d %#lx -> %#lx (t=%lums)",
                    g_watch[i].off, g_watch[i].width, g_watch[i].last, v,
                    now - g_watch_ms0);
            g_watch[i].last = v;
        }
    }
}

/* 快速采样线程：5ms 一次，只读。与 50ms 主循环解耦，避免把「连写两次」合并成一次。
   回推路径可能是「先清零 +0x1c，再回填」，50ms 采样大概率只看到最终值。 */
static volatile int g_fast_watch_on;

static void *fast_watch_thread(void *arg) {
    (void) arg;
    while (g_fast_watch_on) {
        watch_sample();
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 5L * 1000L * 1000L;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void fast_watch_start(void) {
    if (g_fast_watch_on) return;
    g_fast_watch_on = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, fast_watch_thread, NULL) == 0) {
        pthread_detach(th);
        logline("watch: 5ms 采样线程已启动");
    } else {
        g_fast_watch_on = 0;
        logline("watch: 采样线程启动失败");
    }
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
static int g_qd_trace = 1;    /* 只读抓包，默认开 */
static int g_hold_enable = 1;   /* skin.cfg 里 `hold 0` 可关掉回推恢复，做对照实验 */

/* ==== 体力触发（已废弃：改由网络层 SAVE_DECKS 触发，见 fast-save 段） ====
   保留只是为了有个可回退的实现；只有 skin.cfg 里写 `trigger sta` 才会用到。 */
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
static int g_trigger;   /* 0 = 网络层 SAVE_DECKS（默认）；1 = 旧的体力触发 */

/* ==== 网络层快速存档（fast-save）====
   原理：客户端的队伍皮肤/队名/牌子只经一条请求上服务器 ——

     `save_decks&pid=%d&sid=%s&curdeck=%d&decksbs={ <16 字符>*N },<cnt>,<payload> &bchg=1`

   其中每一段是 `b[x]`（单个 96 字节队伍记录，base64 化的 24 个 u32）
   或 `s[xx]`（一整套 32 条记录）。服务器对这段做校验：选了没拥有的皮肤就回
   「エラー番号:1」并回滚。所以只要**在请求拼好、还没发出去之前**把 payload 里的
   皮肤字段换掉，服务器看到的永远合法，本地存档可以随便显示。

   实现是**函数指针替换**，不写任何 .text：
     `_padApiExec(char const* action, char const* param, unsigned char* data, uint)
        -> requestExec(action, "action=<action>&r=%d&m=%d" + param, data, len)`
   把 requestExec 的指针（保存/恢复时直接改指针本身）
   换成 `qd_exec`：进去时解 qd、改 payload、重新拼；出来时恢复原 qd 并写回原缓冲区。
   这样连"服务器回什么、本地怎么解析"都不用管。 */

#define QD_BUF_CAP 0x2000                 /* requestExec 的报文缓冲上限（与构造代码里的 0x2000 一致） */
#define QD_TRAILER 22UL                   /* ",%d,%d%s" / "action=%s&r=%d&m=%d" 预留长度 */
#define DECK_STRIDE 0x60U
#define DECK_COUNT 32                     /* 存档里预分配的记录槽数（_parseDeckAll 写死 cmp #0x20） */



#define MAX_DECK_OFF 0x52f3b7UL           /* u8：当前队伍上限 MaxDeckSets，6..27，随等级变 */
#define DECK_LIMIT_LO 6
#define DECK_LIMIT_HI 32
#define DECK_REC_BASE 0x52f3bcUL          /* _parseDeckAll 的 32 条队伍记录起点 */
/* 一条 0x60 记录的布局（实测 + _parseDeckAll 反汇编）：
     +0x00..0x17  6 个队员的卡牌 ID（u32 x6），会被回推覆盖
     +0x1c        皮肤（u16），回推只清零、从不回填 —— 这就是我们要保的字段
     +0x5f        牌子位（u8），回推会写（带 eor 混淆）
   SKIN_BASE = DECK_REC_BASE + 0x1c，所以以 SKIN_BASE 为基时皮肤偏移是 0。 */
#define REC_SKIN_OFF 0x1cU                /* 以下都相对一条 0x60 记录的起点 DECK_REC_BASE */
#define REC_BADGE_OFF 0x5fU
#define REC_BGM1_OFF 0x18U
#define REC_BGM2_OFF 0x1aU
#define SKIN_BADGE_MASK 0xf0

static unsigned char *volatile g_qd_buf;
static volatile int g_qd_restore;

/* ===== 小型 base64（PAD 的变体表） =====
   `_API_REQ_SAVE_DECKS` 的编码循环在 0x999d1c..0x999d48：

       38 00 16 97   strb w23, [x20], #1        ; 先写分隔符 ',' (0x2c)
       8b 08 03 09   add  x9, x24, x8           ; x24 = libpad base
       38 79 69 23   ldrb w3, [x9, x25]         ; w25 = 0x28cdd  <- 表下标
       91 00 05 1a   add  x26, x8, #1
       f1 00 ff 5f   cmp  x26, #0x3f            ; 0..62 —— 恰好 63 项
       54 ff fe a1   b.ne 0x999d1c

   所以表是 libpad 里一张 **63 字节**的常量数组 `x24+0x28cdd`（下标 0..62）。
   `#` 在表外另有一个比较分支专管"填充/终止"，所以表内不重复 `#`。
   本表按可见 ASCII 升序排布（0x21..0x5f 去掉 `'` 与 `"`… 恰好 60 个），
   剩下 3 个只可能是不可见字符或 `$`：靠 qdump 抓真实 payload 校准即可判定。 */
#define B64_PAD '#'
static const char B64A[] =
        "!\"#$%&()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_";
static signed char g_b64d[256];

__attribute__((constructor))
static void b64_init(void) {
    memset(g_b64d, -1, sizeof(g_b64d));
    for (int i = 0; i < (int) (sizeof(B64A) - 1); i++) g_b64d[(unsigned char) B64A[i]] = (signed char) i;
    g_b64d[(unsigned char) B64_PAD] = -3;      /* 填充与表中已有字符的区分在解码端处理 */
}

static int b64_of(unsigned char c) {
    if (c == (unsigned char) B64_PAD) return -1;      /* 填充 */
    int v = g_b64d[c];
    return (v < 0) ? -2 : v;      /* -2 = 非法字符 */
}

static unsigned long b64_dec(const char *s, unsigned long n, unsigned char *out, unsigned long cap) {
    unsigned long w = 0, o = 0;
    for (unsigned long i = 0; i < n; i++) {
        int v = b64_of((unsigned char) s[i]);
        if (v < 0) break;
        w = ((w << 6) | (unsigned long) v) & 0xffffffUL;
        if ((i & 3) == 3) {
            if (o + 3 > cap) break;
            out[o++] = (unsigned char) (w >> 16);
            out[o++] = (unsigned char) (w >> 8);
            out[o++] = (unsigned char) w;
        }
    }
    return o;
}

static int b64_enc(const unsigned char *in, unsigned long n, char *out, unsigned long cap) {
    unsigned long o = 0, i = 0;
    while (i + 3 <= n) {
        unsigned long w = ((unsigned long) in[i] << 16) | ((unsigned long) in[i + 1] << 8) | in[i + 2];
        if (o + 4 >= cap) return -1;
        out[o++] = B64A[((w >> 18) & 63) % (sizeof(B64A) - 1)];
        out[o++] = B64A[((w >> 12) & 63) % (sizeof(B64A) - 1)];
        out[o++] = B64A[((w >> 6) & 63) % (sizeof(B64A) - 1)];
        out[o++] = B64A[(w & 63) % (sizeof(B64A) - 1)];
        i += 3;
    }
    unsigned long r = n - i;
    if (r) {
        unsigned long w = (unsigned long) in[i] << 16;
        if (r == 2) w |= (unsigned long) in[i + 1] << 8;
        if (o + 4 >= cap) return -1;
        out[o++] = B64A[((w >> 18) & 63) % (sizeof(B64A) - 1)];
        out[o++] = B64A[((w >> 12) & 63) % (sizeof(B64A) - 1)];
        if (r == 2) out[o++] = B64A[((w >> 6) & 63) % (sizeof(B64A) - 1)];
        out[o++] = '#';
    }
    out[o] = 0;
    return (int) o;
}

/* ===== 本地皮肤值 g_local =====
   前提（用户确认）：皮肤/BGM 完全是本地状态，服务端不保存，回推的必然是默认值。

   更新规则（用户定的）：
     写入值 != 默认值  ->  接受，g_local 随之更新（用户在设定界面随便换）
     写入值 == 默认值  ->  忽略，立刻把存档改回 g_local
   因为默认值只可能来自服务器回推，而用户唯一失去的能力就是"用默认皮肤"——
   关掉模块默认皮肤自然就回来了。 */
/* 多用户：savedata 指针在整个生命周期内不变（load/deleteSaveData/_loadPlayerData
   都只读 *(libpad+0x10de320)，从不重新赋值），切用户是把另一份数据灌进同一块
   内存。所以槽位键不能用指针。

   键选 /data/user/0/jp.gungho.pad/files/data048.bin 的内容哈希：
     - 这是加密身份凭据（84 字节纯随机），换文件即换账号；
     - 内容长期稳定（mtime 停在 2026-02-27，登录刷新不碰它），可以放心做键；
     - 是普通文件，进程启动即可读，不必等 savedata 就绪。
   用哈希而不是原始字节，是为了不在内存/日志里长时间摊开凭据。 */
#define USER_CRED_PATH "/data/user/0/jp.gungho.pad/files/data048.bin"
#define USER_SLOTS 4

struct user_slot {
    unsigned long long cred_hash;
    int valid;
    unsigned long last_used;         /* 秒级时间戳，槽位耗尽时按 LRU 复用 */
    unsigned char local[DECK_COUNT][DECK_STRIDE];
    int have;
};

static struct user_slot g_users[USER_SLOTS];
static int g_cur_slot = -1;          /* 当前凭据对应的槽；-1 = 尚未识别 */
static unsigned long g_restored;
static unsigned long g_user_switches;

/* FNV-1a 64 位，对 data048.bin 全文做摘要。读失败返回 0。 */
static unsigned long long cred_hash(void) {
    FILE *f = fopen(USER_CRED_PATH, "rb");
    if (!f) return 0;
    unsigned char buf[256];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) return 0;
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned long long) buf[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

static void user_load(struct user_slot *U);

/* 取当前用户的槽位。
 *
 * 2.6.2 修了两件事：
 *  1) 新凭据先观察 2 秒再切槽。data048.bin 在登录/切号过程中可能被短暂
 *     改写，读到一个中间态哈希就切槽，日志里会出现同一个 cred 反复
 *     “account switched”，并且把真正的槽清掉。观察期返回 NULL，调用方
 *     这一轮不碰任何账号数据。
 *  2) 槽位耗尽时按 LRU 复用，而不是固定 memcpy 到 slot 0；如果哈希其实
 *     命中了一个已有槽位（切回旧账号），直接复用它、不 memset，避免把
 *     那个账号的本地皮肤值清空。 */
static struct user_slot *cur_user(void) {
    /* 每轮都 fopen 太浪费：缓存 1 秒。 */
    static unsigned long long cached_h;
    static unsigned long cached_at;
    static unsigned long long pending_h;
    static unsigned long pending_at;
    unsigned long now = (unsigned long) time(NULL);
    unsigned long long h;
    if (cached_h && now - cached_at < 1) {
        h = cached_h;
    } else {
        h = cred_hash();
        cached_h = h;
        cached_at = now;
        if (!h) return NULL;
    }
    if (g_cur_slot >= 0 && g_users[g_cur_slot].valid && g_users[g_cur_slot].cred_hash == h) {
        pending_h = 0;
        g_users[g_cur_slot].last_used = now;
        return &g_users[g_cur_slot];
    }
    /* 新哈希先观察，连续稳定 2 秒才认。观察期返回 NULL，宁可这轮不盖，
       也不能把 A 账号的 local 写进 B 账号的存档。 */
    if (h != pending_h) { pending_h = h; pending_at = now; return NULL; }
    if (now - pending_at < 2) return NULL;
    pending_h = 0;

    int pick = -1, vac = -1, lru = 0;
    for (int i = 0; i < USER_SLOTS; i++) {
        if (g_users[i].valid && g_users[i].cred_hash == h) { pick = i; break; }
        if (!g_users[i].valid && vac < 0) vac = i;
        if (g_users[i].last_used < g_users[lru].last_used) lru = i;
    }
    if (pick >= 0) {
        /* 切回一个仍然有效的旧槽：保留它的 local，别清空。 */
        g_cur_slot = pick;
        g_users[pick].last_used = now;
        g_user_switches++;
        g_badge_once = 0;
        g_quiet_until = 0;
        logline("fast: switched back to user slot %d cred=%016llx", pick, h);
        user_load(&g_users[pick]);
        return &g_users[pick];
    }
    pick = (vac >= 0) ? vac : lru;
    memset(&g_users[pick], 0, sizeof(g_users[pick]));
    g_users[pick].cred_hash = h;
    g_users[pick].valid = 1;
    g_users[pick].last_used = now;
    int reused = (g_cur_slot == pick);
    g_cur_slot = pick;
    if (!reused) {
        g_user_switches++;
        /* 换账号 = 换了一整份存档。下面这些是「每个账号只做一次」的状态，
           必须跟着重置，否则新账号会继承上一个账号的「已经做过了」，
           表现就是「切号后徽章/皮肤没被盖回」：
             - g_badge_once：徽章一次性盖回本地牌子；
             - g_quiet_until：登录后 60 秒「牌子只认本地」的窗口。 */
        g_badge_once = 0;
        g_quiet_until = 0;
        logline("fast: account switched -> reset per-user probe state (slot %d)", pick);
    }
    user_load(&g_users[pick]);
    logline("fast: user slot %d cred=%016llx", pick, h);
    return &g_users[pick];
}

/* ===== 槽位持久化 =====
   皮肤值只活在内存（savedata+0x52f3bc+k*0x60+0x00），每次登录都被回推清零，
   存档里没有可回落的持久副本。所以探针自己落盘，按凭据哈希分文件：
       "<base>/skins_<cred>.bin"
   格式：magic "PADH" + ver(u32) + lock(u32) + DECK_COUNT 条 {u8 skin, u8 badge} */
#define SKIN_STORE_MAGIC 0x48444150U     /* "PADH" */
#define SKIN_STORE_VER 3U

static void user_store_path(const struct user_slot *U, char *out, size_t cap);
static void user_save(const struct user_slot *U);
static void user_load(struct user_slot *U);

static void user_store_path(const struct user_slot *U, char *out, size_t cap) {
    snprintf(out, cap, "%s/skins_%016llx.bin", base_dir(), U->cred_hash);
}

static void user_save(const struct user_slot *U) {
    if (!U || !U->valid) return;
    char path[128];
    user_store_path(U, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) { logline("store: open %s failed", path); return; }
    unsigned int hdr[3] = { SKIN_STORE_MAGIC, SKIN_STORE_VER, (unsigned int) U->have };
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(U->local, sizeof(U->local), 1, f);
    fclose(f);
    logline("store: saved slot %d (%s)", g_cur_slot, path);
}

static void user_load(struct user_slot *U) {
    if (!U) return;
    char path[128];
    user_store_path(U, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) { logline("store: no file for cred=%016llx", U->cred_hash); return; }
    unsigned int hdr[3] = { 0, 0, 0 };
    if (fread(hdr, sizeof(hdr), 1, f) != 1 || hdr[0] != SKIN_STORE_MAGIC) {
        fclose(f);
        logline("store: bad header in %s", path);
        return;
    }
    if (hdr[1] == 1U) {
        unsigned char old[DECK_COUNT * 2];
        if (fread(old, sizeof(old), 1, f) != 1) { fclose(f); return; }
        for (int i = 0; i < DECK_COUNT; i++) {
            U->local[i][REC_SKIN_OFF] = old[i * 2 + 0];
            U->local[i][REC_BADGE_OFF] = old[i * 2 + 1];
        }
        U->have = (int) hdr[2];
        fclose(f);
        logline("store: migrated v1 slot %d have=%d", g_cur_slot, U->have);
        if (U->have) user_save(U);
        return;
    }
    if (hdr[1] == 2U) {
        /* v2 用 SKIN_BASE 相对偏移，皮肤落在记录首字节；搬到 +0x1c */
        if (fread(U->local, sizeof(U->local), 1, f) != 1) { fclose(f); return; }
        for (int i = 0; i < DECK_COUNT; i++) {
            if (U->local[i][0] != 0 && U->local[i][REC_SKIN_OFF] == 0) {
                U->local[i][REC_SKIN_OFF] = U->local[i][0];
                U->local[i][0] = 0;
            }
        }
        U->have = (int) hdr[2];
        fclose(f);
        logline("store: migrated v2 slot %d have=%d", g_cur_slot, U->have);
        if (U->have) user_save(U);
        return;
    }
    if (hdr[1] != SKIN_STORE_VER) {
        fclose(f);
        logline("store: unknown ver %u in %s", hdr[1], path);
        return;
    }
    if (fread(U->local, sizeof(U->local), 1, f) != 1) {
        fclose(f);
        logline("store: short read in %s", path);
        return;
    }
    fclose(f);
    U->have = (int) hdr[2];
    logline("store: loaded slot %d have=%d", g_cur_slot, U->have);
}

/* 有效队伍数：存档里只保证 [0, n) 有效，其余是预分配的空槽。
   来源 cSAVEDATA::invalidateMaxDeckSets @0xa4c06c：
       add x9, x22, #0x52f, lsl 12 ; add x9, x9, #0x3b0 ; ldrb w11, [x9, #7]
   读不到或越界时退回上限，宁可多扫也不漏掉用户的队伍。 */
static int deck_limit(unsigned long svd) {
    if (!addr_mapped(svd + MAX_DECK_OFF)) return DECK_LIMIT_HI;
    unsigned char n = *(volatile unsigned char *) (svd + MAX_DECK_OFF);
    if (n < DECK_LIMIT_LO || n > DECK_COUNT) return DECK_LIMIT_HI;
    return n;
}

/* 皮肤字节的低位是皮肤 ID，0x40/0x80 是标志位（拥有/标记），判默认时剔掉 */
static int is_default_skin(unsigned char v) {
    return (v & 0x7f) == 0;
}

/* 每轮轮询：先用存档同步 g_local，再把被回推清零的项恢复回去。
   顺序很重要 —— 先吸收用户的合法改动，再做恢复，避免把用户刚选的值当回推覆盖掉。 */
static volatile int g_got_trace;   /* 打开 body/rec 日志（gottrace） */
/* g_badge_once / g_quiet_until 的定义在文件头部（换账号时要 reset，所以提前声明） */
static void skin_sync(void) {
    unsigned long svd = savedata_ptr();
    if (!svd || !armed() || !savedata_ready()) return;
    if (!g_hold_enable) return;
    if (!addr_mapped(svd + DECK_REC_BASE) || !addr_mapped(svd + DECK_REC_BASE + DECK_COUNT * DECK_STRIDE)) return;
    struct user_slot *U = cur_user();
    if (!U) return;
    unsigned char *p = (unsigned char *) (svd + DECK_REC_BASE);
    int n = deck_limit(svd);
    int dirty = 0;

    /* 登录解析会用服务器的值（默认牌子）覆盖存档，所以这段时间内牌子只认本地 */
    if (!g_quiet_until) g_quiet_until = (long) time(NULL) + 60;
    int quiet = (long) time(NULL) < g_quiet_until;
    if (!g_badge_once) {
        int fixed = 0;
        if (U->have && g_badge_enabled) {
            for (int i = 0; i < n; i++) {
                unsigned char *cur = p + i * DECK_STRIDE;
                unsigned char lv = U->local[i][REC_BADGE_OFF];
                if (lv != 0 && cur[REC_BADGE_OFF] != lv) {
                    cur[REC_BADGE_OFF] = lv;
                    fixed++;
                }
            }
        }
        g_badge_once = 1;
        if (fixed) logline("fast: badge restored once (%d decks)", fixed);
    }
    for (int i = 0; i < n; i++) {
        unsigned char *cur = p + i * DECK_STRIDE;
        unsigned char *loc = U->local[i];
        unsigned char v = cur[REC_SKIN_OFF];

        /* 皮肤：只有开关打开时才「盖回」。吸收分支始终执行 —— 那只是把游戏里
           用户自己的选择记进本地缓存，不改游戏数据，关掉功能时也该保持同步。 */
        if (is_default_skin(v)) {
            /* 回推的默认皮肤：这是个「服务器不知道用户真实选择」的信号，跳过不写 */
            if (g_skin_enabled && U->have && v != loc[REC_SKIN_OFF]) {
                cur[REC_SKIN_OFF] = loc[REC_SKIN_OFF];
                g_restored++;
            }
        } else if (!U->have || loc[REC_SKIN_OFF] != v) {
            /* 用户自己选了一个真皮肤：吸收进当前用户的记录 */
            loc[REC_SKIN_OFF] = v;
            if (!U->have) {
                U->have = 1;
                logline("fast: local locked (slot %d)", g_cur_slot);
            }
            dirty = 1;
        }

        /* 牌子：存档里是 real ^ ((i*15+0x61)&0xff)，real 0 = 没戴牌子 */
        {
            unsigned char key = (unsigned char) ((i * 15 + 0x61) & 0xff);
            unsigned char real = (unsigned char) (cur[REC_BADGE_OFF] ^ key);
            unsigned char lreal = (unsigned char) (loc[REC_BADGE_OFF] ^ key);
            if (g_got_trace && i < 2 && cur[REC_BADGE_OFF] != loc[REC_BADGE_OFF])
                logline("bsync: i=%d cur=%#x loc=%#x real=%u lreal=%u quiet=%d",
                        i, cur[REC_BADGE_OFF], loc[REC_BADGE_OFF], real, lreal, quiet);
            if (g_badge_enabled && (real == 0 || real == 1 || quiet)) {
                /* 0=没戴，1=服务器默认牌子：都是「服务器不知道用户真实选择」的信号 */
                if (U->have && lreal != 0) {
                    cur[REC_BADGE_OFF] = (unsigned char) (lreal ^ key);
                    g_restored++;
                }
            } else if (real != 0 && real != 1 && !quiet && (!U->have || lreal != real)) {
                loc[REC_BADGE_OFF] = cur[REC_BADGE_OFF];
                if (!U->have) {
                    U->have = 1;
                    logline("fast: local locked (slot %d)", g_cur_slot);
                }
                dirty = 1;
            }
        }

        /* BGM 两个 u16：+0x18 道中、+0x1a ボス，全 0 = 默认 */
        unsigned short b1 = *(unsigned short *) (cur + REC_BGM1_OFF);
        unsigned short b2 = *(unsigned short *) (cur + REC_BGM2_OFF);
        unsigned short l1 = *(unsigned short *) (loc + REC_BGM1_OFF);
        unsigned short l2 = *(unsigned short *) (loc + REC_BGM2_OFF);
        if (i == 0) {
            static unsigned long dbg;
            if ((dbg++ % 600) == 0)
                logline("sync0: skin=%u loc=%u bd=%u/%u bgm=%u/%u lbgm=%u/%u",
                        v, loc[REC_SKIN_OFF], cur[REC_BADGE_OFF], loc[REC_BADGE_OFF], b1, b2, l1, l2);
        }
        if (g_bgm_enabled && b1 == 0 && b2 == 0) {
            if (U->have && (b1 != l1 || b2 != l2)) {
                *(unsigned short *) (cur + REC_BGM1_OFF) = l1;
                *(unsigned short *) (cur + REC_BGM2_OFF) = l2;
                g_restored++;
            }
        } else if (b1 != 0 && b2 != 0 && (!U->have || b1 != l1 || b2 != l2)) {
            *(unsigned short *) (loc + REC_BGM1_OFF) = b1;
            *(unsigned short *) (loc + REC_BGM2_OFF) = b2;
            if (!U->have) {
                U->have = 1;
                logline("fast: local locked (slot %d)", g_cur_slot);
            }
            dirty = 1;
        }
    }
    if (dirty) user_save(U);
}

/* ===== 改写一次 decksbs payload ===== */
static int qd_rewrite(char *lo, char *hi) {
    static char tmp[QD_BUF_CAP];
    int out = 0, changed = 0, segs = 0;
    struct user_slot *U = cur_user();
    if (!U || !U->have) return 0;
    char *b = memmem(lo, (size_t) (hi - lo), "&decksbs={", 10);
    if (!b) return 0;
    b += 10;
    char *e = memchr(b, '}', (size_t) (hi - b));
    if (!e) return 0;

    /* 1) 前缀 <token>,<cnt>, */
    char *comma = memchr(b, ',', (size_t) (e - b));
    if (!comma) return 0;
    if (comma - b > 8) return 0;
    char tok[10];
    memcpy(tok, b, (size_t) (comma - b));
    tok[comma - b] = 0;
    char *comma2 = memchr(comma, ',', (size_t) (e - comma));
    if (!comma2) return 0;
    int cnt = 0;
    {
        char cb[8];
        size_t n = (size_t) (comma2 - comma - 1);
        if (n == 0 || n > 6) return 0;
        memcpy(cb, comma + 1, n);
        cb[n] = 0;
        cnt = atoi(cb);
    }
    int is_set = (tok[0] == 's');
    int base = is_set ? 0 : atoi(tok + 1);
    if (base < 0 || base >= DECK_COUNT) return 0;

    /* 2) 先数一段有多少个 payload 段 */
    static unsigned char raw[QD_BUF_CAP];
    static char enc[QD_BUF_CAP];
    char *p = comma2 + 1;
    while (p < e) {
        char *c = memchr(p, ',', (size_t) (e - p));
        if (!c) break;
        segs++;
        p = c + 1;
    }
    if (segs == 0) return 0;

    /* 3) 前缀照抄，然后逐段按 g_local 重建 */
    memcpy(tmp, lo, (size_t) (comma2 + 1 - lo));
    out = (int) (comma2 + 1 - lo);
    p = comma2 + 1;
    for (int s = 0; s < segs; s++) {
        char *c = memchr(p, ',', (size_t) (e - p));
        unsigned long rawlen = b64_dec(p, (unsigned long) (c - p), raw, sizeof(raw));
        if (is_set) {
            unsigned long svd2 = savedata_ptr();
            int nl = svd2 ? deck_limit(svd2) : DECK_COUNT;
            for (unsigned k = 0; k < (unsigned) nl; k++) {
                if ((k + 1) * DECK_STRIDE > rawlen) break;
                /* 只改皮肤(+0x1c)和牌子(+0x5f)，卡牌 ID 一个字节都不碰。
                   每一项都要看自己的开关：关掉的功能不能继续改写上传内容，
                   否则「关了开关」在服务器侧仍然是改过的值。 */
                if (g_skin_enabled) raw[k * DECK_STRIDE + REC_SKIN_OFF] = U->local[k][REC_SKIN_OFF];
                if (g_badge_enabled) raw[k * DECK_STRIDE + REC_BADGE_OFF] = U->local[k][REC_BADGE_OFF];
            }
            changed = 1;
        } else {
            int idx = base + s;
            if (idx < DECK_COUNT && rawlen >= DECK_STRIDE) {
                if (g_skin_enabled) raw[REC_SKIN_OFF] = U->local[idx][REC_SKIN_OFF];
                if (g_badge_enabled) raw[REC_BADGE_OFF] = U->local[idx][REC_BADGE_OFF];
                changed = 1;
            }
        }
        int n = b64_enc(raw, rawlen, enc, sizeof(enc));
        if (n < 0 || out + n >= QD_BUF_CAP - 2) return 0;
        memcpy(tmp + out, enc, (size_t) n);
        out += n;
        tmp[out++] = ',';
        p = c + 1;
    }
    tmp[out - 1] = 0;   /* 去掉最后那个逗号 */
    out--;

    /* 4) } 后面的尾巴原样搬过去（构造代码在 } 之外还有 "&bchg=1" 之类） */
    size_t tl = (size_t) (hi - e - 1);
    if (tl) {
        if (out + (int) tl >= QD_BUF_CAP - QD_TRAILER) return 0;
        memcpy(tmp + out, e + 1, tl);
        out += (int) tl;
    }
    (void) cnt;
    memcpy(g_qd_buf, tmp, (size_t) out + 1);
    return changed;
}

/* 垫片入口（见 qd_hook）：先执行被覆盖的原指令，再跳回原函数 +4。
   还负责把 qd_rewrite 写进去的临时缓冲换回来。 */
static int (*g_tramp_fn)(const char *, const char *, unsigned char *, unsigned int);
static unsigned int g_fast_on;

/* T1 的替换入口：签名与 requestExec 完全一致 */
__attribute__((noinline))
static int qd_exec(const char *action, const char *param, unsigned char *data, unsigned int len) {
    unsigned char *saved = g_qd_buf;
    int rewrote = 0;
    /* 只读记录：qdtrace 开着时不改写任何字节，只把 action 和 param 抄进日志。
       用来确认 SAVE_DECKS 请求长什么样、皮肤字节落在哪。 */
    if (g_qd_trace && action && param) {
        char head[192];
        size_t n = strlen(param);
        if (n > sizeof(head) - 1) n = sizeof(head) - 1;
        memcpy(head, param, n);
        head[n] = 0;
        logline("qdtrace: action=%s len=%u param0=%s", action, len, head);
    }
    if (!g_fast_on || param == NULL || !g_tramp_fn) {
        return g_tramp_fn ? g_tramp_fn(action, param, data, len) : -1;
    }
    if (param && g_cur_slot >= 0 && g_users[g_cur_slot].have) {
        char *lo = (char *) param;
        char *semi = strstr(lo, ";");
        if (semi) {
            size_t hl = (size_t) (semi - lo);
            /* action=<a>&r=%d&m=%d 预留 22 字符 */
            if (hl + QD_TRAILER <= QD_BUF_CAP) {
                char *hi = memchr(semi + 1, ';', strlen(semi + 1));
                if (hi && (unsigned long) (hi - lo) + 1 + QD_TRAILER <= QD_BUF_CAP)
                    rewrote = qd_rewrite(lo, hi);
            }
        }
    }
    int r = g_tramp_fn(action, param, data, len);
    if (rewrote) {
        g_qd_buf = saved;
        g_qd_restore = 1;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    return r;
}

/* ===== 符号解析（纯数据操作，不碰 .text 一个字节） =====
   走的是运行库的 link_map：libpad 的动态段里有 DT_HASH / DT_STRTAB / DT_SYMTAB，
   从中按名字查到 requestExec 的真实地址。查到的地址只是一块数据，
   真正要 hook 它还得改 .text（铁律 1 禁止），所以这里只把它当作
   「证据 + 兜底」：qd_trace 允许在不 hook 的前提下把每一次上传的报文抓出来。 */

#define EXEC_SYM "_ZN8cAPPHTTP12_requestExecEPKcS1_Phj"

/* «动态段名» 表：u32 个数、偏移表、名字串 */
static unsigned long qd_sym_tab;      /* .dynsym 数组 */
static unsigned long qd_str_tab;      /* .dynstr */
static int qd_sym_count = -1;
static unsigned long qd_lib_base;

struct qd_sym {
    uint32_t name, info, other;
    uint16_t shndx;
    uint64_t value, size;
};

static int qd_count_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void) size; (void) data;
    if (qd_sym_count > 0) return 0;
    if (!info->dlpi_name || !strstr(info->dlpi_name, "libpad.so")) return 0;
    int found = 0;
    for (int i = 0; i < info->dlpi_phnum && !found; i++) {
        if (info->dlpi_phdr[i].p_type != PT_DYNAMIC) continue;
        found = 1;
        unsigned long base = info->dlpi_addr;
        Elf64_Dyn *d = (Elf64_Dyn *) (base + info->dlpi_phdr[i].p_vaddr);
        unsigned long nchain = 0, str = 0, sym = 0, gnu = 0;
        for (; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_HASH) nchain = *(unsigned int *) (base + d->d_un.d_ptr + 4);
            if (d->d_tag == DT_STRTAB) str = base + d->d_un.d_ptr;
            if (d->d_tag == DT_SYMTAB) sym = base + d->d_un.d_ptr;
            if (d->d_tag == DT_GNU_HASH) gnu = base + d->d_un.d_ptr;
        }
        /* 现代 Android 的 .so 往往只有 DT_GNU_HASH，没有 DT_HASH。
           没有 DT_HASH 时从 GNU_HASH 的 nbuckets/symoffset 推一个上界：
           symcount 无法直接得到，用「最大 bucket/chain 下标 + symoffset」兜底。 */
        if (!nchain && gnu) {
            unsigned int nbuckets = *(unsigned int *) gnu;
            unsigned int symoffset = *(unsigned int *) (gnu + 4);
            unsigned int bloom_size = *(unsigned int *) (gnu + 8);
            unsigned int *buckets = (unsigned int *) (gnu + 16 + bloom_size * 8);
            unsigned int maxb = 0;
            for (unsigned int i = 0; i < nbuckets && nbuckets < 0x100000; i++)
                if (buckets[i] > maxb) maxb = buckets[i];
            /* 走一遍 chain：hash % nbuckets 的桶起，沿 chain 直到 LSB=1 */
            unsigned int *chain = buckets + nbuckets;
            unsigned int last = maxb;
            if (last >= symoffset) {
                while (!(chain[last - symoffset] & 1u) && last < 0x200000) last++;
                last++;
            }
            nchain = last > symoffset ? last : symoffset;
            logline("fast: gnu_hash nbuckets=%u symoffset=%u -> symcount=%lu",
                    nbuckets, symoffset, nchain);
        }
        if (!nchain || !str || !sym) break;
        qd_lib_base = base;
        qd_sym_tab = sym;
        qd_str_tab = str;
        qd_sym_count = (int) nchain;
        logline("fast: dynsym %d entries @%#lx", qd_sym_count, sym);
    }
    return found;
}

/* 在 .dynsym 里按名字找函数；返回绝对地址（st_value 是不含 base 的 vaddr） */
static unsigned long qd_lookup(const char *name) {
    if (qd_sym_count <= 0) return 0;
    struct qd_sym *s = (struct qd_sym *) qd_sym_tab;
    for (int i = 1; i < qd_sym_count; i++) {
        if (s[i].value == 0 || s[i].name == 0) continue;
        if (strcmp((const char *) (qd_str_tab + s[i].name), name) == 0)
            return (s[i].value & 0x00ffffffffffffffUL) + qd_lib_base;
    }
    return 0;
}

/* ===== 安装：把 requestExec 的函数入口整体替换成 qd_exec =====
   做法是**在函数最前面写一条 b 指令**（4 字节），不是常见的那种
   "备份 8~16 字节 + 相对跳板" 的 inline hook —— 只改 4 字节，落点在函数
   自己的第一个指令（stp x29,x30）。qd_exec 的 C 签名与 requestExec 完全相同，
   所以参数寄存器不用搬；qd_exec 内部保存 x29/x30 到自己的栈帧里，
   用 g_exec 直接 call 原函数（原函数已改过的头 4 字节在 g_exec 里被
   临时还原？不需要：b 指令跳走以后原函数后面的代码是完整的，只有第一条
   stp 丢了 —— 而 qd_exec 自己已经做了 stp x29,x30。为了避免这种耦合，
   g_exec 指向的是**补丁前的入口副本**：一个 4 条指令的小垫片，
   先执行被覆盖的 stp，再 b 回 原函数+4。 */
#define STUB_MAGIC 0xd503201f

static unsigned long g_exec_entry;      /* 补丁后的跳转目标 = libpad 内原函数地址 */
static unsigned int g_exec_orig_word;   /* 被覆盖的原指令 */
static unsigned int *g_tramp;           /* 垫片：orig_word + b orig+4 */

static void *qd_alloc_tramp(void) {
    void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (m == MAP_FAILED) ? NULL : m;
}

/* 填 b 指令 */
static unsigned int arm_b(unsigned long from, unsigned long to) {
    long d = (long) (to - from);
    return 0x14000000u | (unsigned int) ((d >> 2) & 0x03ffffff);
}

/* on=1 安装，on=0 还原 */
static void qd_hook(int on) {
    unsigned long lib = libpad_base();
    if (!lib) { logline("fast: no libpad"); return; }
    /* 静态偏移：设备上的 libpad.so 与 devdump 的 libpad.runtime.so 同为 30708200 字节，
       llvm-nm 给出 _ZN8cAPPHTTP12_requestExecEPKcS1_Phj @ 0x9d0b70。
       动态符号表走 GNU_HASH 时拿不到可靠 symcount，索性直接用偏移，不打表。 */
    unsigned long target = lib + 0x9d0b70UL;
    if (!addr_mapped(target)) {
        logline("fast: requestExec @%#lx not mapped (lib=%#lx)", target, lib);
        return;
    }
    if (on) {
        if (g_exec_entry) { logline("fast: already hooked"); return; }
        g_tramp = qd_alloc_tramp();
        if (!g_tramp) { logline("fast: mmap tramp failed"); return; }
        g_exec_orig_word = *(unsigned int *) target;
        g_tramp[0] = g_exec_orig_word;                       /* 被覆盖的那条 stp */
        g_tramp[1] = arm_b((unsigned long) g_tramp + 4, target + 4);
        __builtin___clear_cache((char *) g_tramp, (char *) g_tramp + 8);
        g_exec_entry = target;
        g_tramp_fn = (int (*)(const char *, const char *, unsigned char *, unsigned int)) g_tramp;
        unsigned int patch = arm_b(target, (unsigned long) qd_exec);
        if (patch_words(target, &patch, 1) != 0) { logline("fast: patch failed"); g_tramp = NULL; return; }
        logline("fast: hooked requestExec @%#lx tramp=%p", target, (void *) g_tramp);
    } else {
        if (!g_exec_entry) return;
        if (patch_words(g_exec_entry, &g_exec_orig_word, 1) == 0)
            logline("fast: unhooked");
        g_exec_entry = 0;
        g_tramp_fn = NULL;
        if (g_tramp) { munmap(g_tramp, 0x1000); g_tramp = NULL; }
    }
}

/* ============ GOT 钩子：只改 .got.plt 里的 8 字节指针，绝不碰 .text ============
   上传链路：_initiateApi -> _API_REQ_SAVE_DECKS(0x9988c0) -> 逐字段拼 0x2000 文本
             -> _padApiExec(0x9d0afc) -> requestExec -> izHttpPost。
   这两处调用都走 PLT stub 读 .got.plt 槽；槽里是 R_AARCH64_RELATIVE 在加载时
   填好的函数地址，不会懒绑定重写。把槽换成我们的 C 函数，就能只读地看到
   真正的 param 和 body（不解析 base64、不改存档、不改代码页）。 */
#define MASK_TAG        0x00ffffffffffffffUL
#define GOT_SLOT_PADAPI 0x10f3948UL   /* -> cAPPHTTP::_padApiExec */
#define GOT_SLOT_REQSD  0x10f3c98UL   /* -> cAPPHTTP::_API_REQ_SAVE_DECKS */
#define GOT_SLOT_HASBADGE 0x10ec728UL /* -> sSAVEDATA::sPLAYER::sDLDATASAVE::hasBadge(int) */
#define GOT_SLOT_RES    0x10f3ca0UL   /* -> cAPPHTTP::_API_RES_SAVE_DECKS */
#define FN_PADAPI       0x9d0afcUL
#define FN_REQSD        0x9988c0UL
#define FN_HASBADGE     0xa36830UL
#define FN_RES          0x998db0UL
#define GOT_SCAN_LO     0x10da000UL
#define GOT_SCAN_HI     0x1110000UL
#define DECK_REC_OFF    0x52f3bcUL

static int (*g_got_padapi)(void *, const char *, unsigned char *, unsigned int);
static int (*g_got_reqsd)(void *, void *);
static int (*g_got_hasbadge)(void *, int);
static int (*g_got_res)(void *, void *);
static unsigned long g_got_slot_padapi, g_got_slot_reqsd, g_got_slot_hasbadge, g_got_slot_res;
static unsigned long g_got_old_padapi, g_got_old_reqsd, g_got_old_hasbadge, g_got_old_res;
static void *g_last_deck_arr;      /* 最后一次上传用的堆记录数组，给 RES 兜底 */
static volatile int g_got_on;      /* GOT 槽已改写成替身 */
static volatile int g_auto_got = 1;/* 启动后自动装钩子 */

static const unsigned char *mem_find(const unsigned char *h, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m == 0 || m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(h + i, needle, m) == 0) return h + i;
    return NULL;
}

static void hexdump_line(const char *tag, const unsigned char *p, int n) {
    char buf[3 * 32 + 1];
    int h = 0;
    if (n > 32) n = 32;
    for (int i = 0; i < n && h < (int) sizeof(buf) - 3; i++)
        h += snprintf(buf + h, sizeof(buf) - (size_t) h, "%02x", p[i]);
    buf[h] = 0;
    logline("%s: %s", tag, buf);
}

/* _padApiExec(this, param, body, len) 的替身：只记日志，原样放行。
   body 是 NUL 结尾的 C 串，len 常为 0，所以按串扫，不看 len。 */
#define GOT_BODY_LOG_MAX 3000
static int got_padapi(void *self, const char *param, unsigned char *body, unsigned int len) {
    if (g_got_trace && param) {
        size_t blen = 0;
        if (body) { while (blen < GOT_BODY_LOG_MAX && body[blen]) blen++; }
        int hit = strstr(param, "save_decks") || strstr(param, "upload_data");
        if (hit) {
            logline("got: padapi param=%s", param);
            if (body) {
                char *head = (char *) malloc(blen + 1);
                if (head) {
                    memcpy(head, body, blen); head[blen] = 0;
                    logline("got: padapi body[%u] %s", len, head);
                    free(head);
                }
                hexdump_line("got: padapi hex", body, 48);
            }
        }
    }
    return g_got_padapi ? g_got_padapi(self, param, body, len) : -1;
}

/* _API_REQ_SAVE_DECKS(this, q) 的替身。
   q->0x20 指向本次请求的卡组数组（stride 0x60，每次请求重建，可安全改写后还原）。
   实测字段（相对记录基址）：+0x28 u16 皮肤、+0x24 u16 BGM1、+0x26 u16 BGM2、
   +0x5f u8 牌子（存的是 xor 混淆值，get(i)=raw ^ ((i*15+0x61)&0xff)）。
   上传时把这些改成默认值，调原函数后再原样写回。 */
static volatile int g_def_on = 1;

static int got_reqsd(void *self, void *q) {
    if (!g_got_on || !q) return g_got_reqsd ? g_got_reqsd(self, q) : -1;

    unsigned long arr = *(unsigned long *) ((char *) q + 0x20) & MASK_TAG;
    unsigned long sv = savedata_ptr();
    int n = 0;
    if (sv) {
        int m = *(unsigned char *) (sv + 0x52f3b7);      /* maxDeckSets */
        int b = (int) *(short *) (sv + 0x52f3ac);        /* 买到的上限 */
        n = m > b ? m : b;
        if (n > 0x20) n = 0x20;
    }
    logline("got: req_save_decks q=%p arr=%#lx n=%d def=%d", q, arr, n, g_def_on);

    unsigned short sv_skin[32], sv_b1[32], sv_b2[32];
    unsigned char sv_bd[32];
    int patched = 0;

    if (arr && n > 0 && addr_mapped(arr) &&
        addr_mapped(arr + (unsigned long) n * 0x60 - 1)) {
        for (int i = 0; i < n; i++) {
            unsigned char *r = (unsigned char *) (arr + (unsigned long) i * 0x60);
            sv_skin[i] = *(unsigned short *) (r + 0x28);
            sv_b1[i]   = *(unsigned short *) (r + 0x24);
            sv_b2[i]   = *(unsigned short *) (r + 0x26);
            sv_bd[i]   = r[0x6b];
            if (g_got_trace && i < 3)
                logline("got: rec%d skin=%u bgm=%u/%u badge=%u raw6b=%#x", i,
                        sv_skin[i], sv_b1[i], sv_b2[i],
                        (unsigned) (sv_bd[i] ^ (unsigned char) ((i * 15 + 0x61) & 0xff)),
                        sv_bd[i]);
        }
        /* 上传前把「用户此刻选的牌子」记到本地：之后服务器回推什么都覆盖不了它 */
        {
            struct user_slot *U = cur_user();
            if (U) {
                int dty = 0;
                if (!U->have) { U->have = 1; logline("fast: local locked (slot %d)", g_cur_slot); dty = 1; }
                for (int i = 0; i < n; i++) {
                    unsigned char *r = (unsigned char *) (arr + (unsigned long) i * 0x60);
                    unsigned char *L = U->local[i];
                    /* 首次建记录时整条快照，避免只填牌子导致皮肤被"恢复"成 0 */
                    if (L[REC_BADGE_OFF] != r[0x6b]) { L[REC_BADGE_OFF] = r[0x6b]; dty = 1; }
                    if (L[REC_SKIN_OFF] != r[0x28]) { L[REC_SKIN_OFF] = r[0x28]; dty = 1; }
                    if (*(unsigned short *)(L + REC_BGM1_OFF) != *(unsigned short *)(r + 0x24)) {
                        *(unsigned short *)(L + REC_BGM1_OFF) = *(unsigned short *)(r + 0x24); dty = 1;
                    }
                    if (*(unsigned short *)(L + REC_BGM2_OFF) != *(unsigned short *)(r + 0x26)) {
                        *(unsigned short *)(L + REC_BGM2_OFF) = *(unsigned short *)(r + 0x26); dty = 1;
                    }
                }
                if (dty) user_save(U);
            }
        }

        /* 上传默认化：三段独立受各自开关控制。
           只有开启的那一项才需要在服务端「隐身」；关掉的功能本来就不改，
           再把它抹成默认值反而会破坏用户自己在游戏里的选择。 */
        int def_skin = g_skin_enabled;
        int def_bgm = g_bgm_enabled;
        int def_badge = g_badge_enabled;
        if (g_def_on && (def_skin || def_bgm || def_badge)) {
            for (int i = 0; i < n; i++) {
                unsigned char *r = (unsigned char *) (arr + (unsigned long) i * 0x60);
                if (def_skin) *(unsigned short *) (r + 0x28) = 0;  /* 皮肤 -> 默认 */
                if (def_bgm) {
                    *(unsigned short *) (r + 0x24) = 0;             /* BGM1 -> 默认 */
                    *(unsigned short *) (r + 0x26) = 0;             /* BGM2 -> 默认 */
                }
                if (def_badge) r[0x6b] = (unsigned char) ((i * 15 + 0x61) & 0xff); /* 牌子 -> 默认(0) */
                patched++;
            }
        }
    } else {
        logline("got: req_save_decks arr unmapped for n=%d", n);
    }

    int ret = g_got_reqsd ? g_got_reqsd(self, q) : -1;

    if (patched) {
        for (int i = 0; i < n; i++) {
            unsigned char *r = (unsigned char *) (arr + (unsigned long) i * 0x60);
            *(unsigned short *) (r + 0x28) = sv_skin[i];
            *(unsigned short *) (r + 0x24) = sv_b1[i];
            *(unsigned short *) (r + 0x26) = sv_b2[i];
            r[0x6b] = sv_bd[i];
        }
        logline("got: restored %d decks", patched);
    }
    return ret;
}

/* hasBadge(this, id) 的替身：id>0 一律当已拥有（0..255 之外照旧）。
   这样队伍牌子列表里未获得的项不再置灰。上传时仍会被 defon 改成默认值。 */
static int got_hasbadge(void *self, int id) {
    if (g_badge_enabled && id > 0 && id < 256) return 1;
    return g_got_hasbadge ? g_got_hasbadge(self, id) : 0;
}

/* _API_RES_SAVE_DECKS 替身：服务端回推会把牌子写回存档（0 会被它换成自己的值），
   调完原函数后把本地记录的牌子字节盖回去。这样牌子就完全只存在本地。 */
static int got_res(void *self, void *q) {
    unsigned long sv = savedata_ptr();
    struct user_slot *U = cur_user();
    int n = 0, ok = 0;
    unsigned char pre[32], loc2[32];
    if (g_got_on && sv && U && U->have && addr_mapped(sv + DECK_REC_BASE)) {
        n = deck_limit(sv);
        if (n > 32) n = 32;
        unsigned char *p = (unsigned char *) (sv + DECK_REC_BASE);
        ok = 1;
        for (int i = 0; i < n; i++) {
            pre[i] = p[i * DECK_STRIDE + REC_BADGE_OFF];
            loc2[i] = U->local[i][REC_BADGE_OFF];
        }
    }
    int ret = g_got_res ? g_got_res(self, q) : -1;
    /* 牌子开关关掉后不能再盖回本地值：本地那份可能是我们强开的未拥有牌子，
       继续还原等于「关了没关掉」。让服务端的值原样生效。 */
    if (ok && g_badge_enabled) {
        unsigned char *p = (unsigned char *) (sv + DECK_REC_BASE);
        int fixed = 0;
        for (int i = 0; i < n; i++) {
            if (loc2[i] == 0) continue;                 /* 本地没有有效牌子 */
            unsigned char now = p[i * DECK_STRIDE + REC_BADGE_OFF];
            if (now != pre[i]) {
                if (g_got_trace)
                    logline("res: deck%d badge %#x -> %#x, restore %#x", i, pre[i], now, loc2[i]);
                p[i * DECK_STRIDE + REC_BADGE_OFF] = loc2[i];
                fixed++;
            }
        }
        if (fixed) logline("res: badge restored %d decks", fixed);
    }
    return ret;
}

/* 在 GOT 区里扫 8 字节值 == fn（用于静态偏移失效时的兜底） */
static unsigned long got_find_slot(unsigned long lib, unsigned long fn) {
    unsigned long lo = lib + GOT_SCAN_LO, hi = lib + GOT_SCAN_HI;
    struct map_range *r = (struct map_range *) malloc(sizeof(struct map_range) * 8192);
    if (!r) return 0;
    int n = load_map_ranges(r, 8192);
    unsigned long hit = 0;
    for (int i = 0; i < n && !hit; i++) {
        unsigned long s = r[i].s > lo ? r[i].s : lo;
        unsigned long e = r[i].e < hi ? r[i].e : hi;
        if (s >= e) continue;
        for (unsigned long p = (s + 7) & ~7UL; p + 8 <= e; p += 8) {
            if ((*(volatile unsigned long *) p & MASK_TAG) == (fn & MASK_TAG)) { hit = p; break; }
        }
    }
    free(r);
    return hit;
}

/* 把 GOT 槽改成 val；槽在 RELRO 段，先临时放开写权限再收紧 */
static int got_poke(unsigned long slot, unsigned long val) {
    unsigned long page = slot & ~0xfffUL;
    if (mprotect((void *) page, 0x1000, PROT_READ | PROT_WRITE) != 0) {
        logline("got: mprotect rw %#lx failed %s", page, strerror(errno));
        return -1;
    }
    *(volatile unsigned long *) slot = val & MASK_TAG;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (mprotect((void *) page, 0x1000, PROT_READ) != 0)
        logline("got: mprotect r %#lx failed %s", page, strerror(errno));
    return 0;
}

static void got_set(int on) {
    unsigned long lib = libpad_base();
    if (!lib) { logline("got: no libpad"); return; }
    if (on == g_got_on) return;
    if (on) {
        unsigned long fnp = lib + FN_PADAPI, fns = lib + FN_REQSD, fnh = lib + FN_HASBADGE, fnr = lib + FN_RES;
        unsigned long sp = 0, ss = 0, sh = 0, sr = 0, c;
        c = lib + GOT_SLOT_PADAPI;
        if (addr_mapped(c) && (*(volatile unsigned long *) c & MASK_TAG) == (fnp & MASK_TAG)) sp = c;
        else sp = got_find_slot(lib, fnp);
        c = lib + GOT_SLOT_REQSD;
        if (addr_mapped(c) && (*(volatile unsigned long *) c & MASK_TAG) == (fns & MASK_TAG)) ss = c;
        else ss = got_find_slot(lib, fns);
        c = lib + GOT_SLOT_HASBADGE;
        if (addr_mapped(c) && (*(volatile unsigned long *) c & MASK_TAG) == (fnh & MASK_TAG)) sh = c;
        else sh = got_find_slot(lib, fnh);
        c = lib + GOT_SLOT_RES;
        if (addr_mapped(c) && (*(volatile unsigned long *) c & MASK_TAG) == (fnr & MASK_TAG)) sr = c;
        else sr = got_find_slot(lib, fnr);
        if (!sp || !ss || !sh || !sr) {
            static int nf = 0;
            if (nf++ < 3) logline("got: slot not found padapi=%#lx reqsd=%#lx", sp, ss);
            return;
        }
        g_got_old_padapi = *(volatile unsigned long *) sp;
        g_got_old_reqsd  = *(volatile unsigned long *) ss;
        g_got_old_hasbadge = *(volatile unsigned long *) sh;
        g_got_old_res    = *(volatile unsigned long *) sr;
        g_got_padapi = (int (*)(void *, const char *, unsigned char *, unsigned int)) (void *) fnp;
        g_got_reqsd  = (int (*)(void *, void *)) (void *) fns;
        g_got_hasbadge = (int (*)(void *, int)) (void *) fnh;
        g_got_res      = (int (*)(void *, void *)) (void *) fnr;
        g_got_slot_padapi = sp;
        g_got_slot_reqsd  = ss;
        g_got_slot_hasbadge = sh;
        g_got_slot_res   = sr;
        g_got_on = 1;
        if (got_poke(sp, (unsigned long) got_padapi) != 0) { g_got_on = 0; return; }
        if (got_poke(ss, (unsigned long) got_reqsd) != 0) {
            got_poke(sp, g_got_old_padapi);
            g_got_on = 0;
            return;
        }
        if (got_poke(sh, (unsigned long) got_hasbadge) != 0) {
            got_poke(sp, g_got_old_padapi);
            got_poke(ss, g_got_old_reqsd);
            g_got_on = 0;
            return;
        }
        if (got_poke(sr, (unsigned long) got_res) != 0) {
            got_poke(sp, g_got_old_padapi);
            got_poke(ss, g_got_old_reqsd);
            got_poke(sh, g_got_old_hasbadge);
            g_got_on = 0;
            return;
        }
        logline("got: ON lib=%#lx padapi_slot=%#lx reqsd_slot=%#lx", lib, sp, ss);
    } else {
        if (g_got_slot_padapi) got_poke(g_got_slot_padapi, g_got_old_padapi);
        if (g_got_slot_reqsd) got_poke(g_got_slot_reqsd, g_got_old_reqsd);
        if (g_got_slot_hasbadge) got_poke(g_got_slot_hasbadge, g_got_old_hasbadge);
        if (g_got_slot_res) got_poke(g_got_slot_res, g_got_old_res);
        g_got_on = 0;
        logline("got: OFF");
    }
}

static void qd_report_ex(int force) {
    unsigned long svd3 = savedata_ptr();
    int lh = (g_cur_slot >= 0) ? g_users[g_cur_slot].have : 0;
    logline("fast: on=%u slot=%d local=%d sw=%lu decks=%d restored=%lu pid=%d reqExec=%#lx", g_fast_on,
            g_cur_slot, lh, g_user_switches,
            svd3 ? deck_limit(svd3) : -1,
            g_restored, (int) getpid(),
            addr_mapped(libpad_base() + 0x9d0b70UL) ? libpad_base() + 0x9d0b70UL : 0UL);
    /* 32 队明细只在「和上次打印的不一样」时才打。
       原来每 5 秒无条件打 32 行，真机日志里 92% 都是这些内容完全相同的
       `fast: deckN skin=0`，把真正有用的行全冲掉了，也让文件涨得很快。
       force=1 用于手工 `qdr` 命令：用户主动要看，就别省。 */
    if (lh) {
        static unsigned char last_decks[DECK_COUNT];
        static int last_have, last_slot = -1;
        /* 换账号也要重新打一次：两个账号的队伍皮肤有可能完全一样，
           只比数组的话第二个账号就一行明细都看不到了。 */
        int changed = force || !last_have || last_slot != g_cur_slot;
        for (int i = 0; i < DECK_COUNT && !changed; i++) {
            if (g_users[g_cur_slot].local[i][REC_SKIN_OFF] != last_decks[i]) changed = 1;
        }
        for (int i = 0; i < DECK_COUNT; i++) {
            last_decks[i] = g_users[g_cur_slot].local[i][REC_SKIN_OFF];
        }
        last_have = 1;
        last_slot = g_cur_slot;
        if (changed) {
            for (int i = 0; i < DECK_COUNT; i++) {
                unsigned char v = last_decks[i];
                logline("fast: deck%d skin=%u%s", i + 1, v, (v & 0x40) ? " owner" : "");
            }
        }
    }
}

static void qd_report(void) { qd_report_ex(0); }

static void qd_set(int on) {
    /* 装 hook 只改 .text 的 4 个字节，不碰存档，不需要等 armed()。
       登录同步（回推→存档、存档→上传）发生在启动最初几秒，等 armed() 就只能看回放。 */
    if (on == (int) g_fast_on) return;
    g_fast_on = (unsigned int) on;
    qd_hook(on);
    logline("fast: %s", on ? "ON" : "OFF");
}

/* 原「时序差」实现，仅 `trigger sta` 回退时使用。
   注意：它把「目标皮肤」戳在存档里整整 15 秒，而游戏在每一帧渲染、每次进列表、
   每次打开皮肤选择画面时读的都是这个值 —— 于是画面上的设定会被重置显示成
   目标款、选不回去，正是本次要淘汰的方案。 */
static void skin_apply_sta(void) {
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

/* 50 ms 轮询主体 */
static void skin_apply(void) {
    if (g_trigger == 1) { skin_apply_sta(); return; }
    skin_sync();
}

/* 影子配置：skin.cfg 由 Java 侧写入（音量键切换），探针按文件内容重建 pin 列表。
   格式：每行一条 `pin <hexoff> <2|4> <val>`，其余行忽略。 */
#define SKIN_CFG_FMT "%s/skin.cfg"

/* skin.cfg 的格式版本。界面侧（CfgSync）每次写文件都会带上 `cfgver N` 行。
 *
 * 为什么必须有它：v2.1 时代探针读的是同一个路径，但那份文件里可能有手工/旧版
 * 留下的 `skin 1` / `bgm 1`，而旧版根本没有 badge 概念。升级到 v2.4 后如果
 * 直接按行解析，就会把「旧文件里的 1」当成「用户现在想要开」——症状就是
 * 「三个开关怎么拨都没用，皮肤和 BGM 恒解锁、徽章恒不解锁」。
 *
 * 所以：没有 cfgver 行（或版本过低）的文件，一律视为**遗留文件**，三个功能
 * 开关强制按 0 处理（调试用的 pin/fast/hold/bits 等仍然生效，方便排查）。
 * 这样默认状态永远是「全关」，与设计文档一致，也不会再有幽灵解锁。 */
#define CFG_VER_MIN 1
static time_t g_cfg_mtime;
static long g_cfg_size = -1;
static long g_cfg_nsec;

/* 最近一次生效的 cfgver（0 = 遗留文件 / 还没读到）。 */
static int g_cfg_ver_applied;
/* 上一次写状态文件时的「版本+三开关」签名，避免重复写盘。 */
static int g_status_sig = -1;

/* 把「探针此刻认为的开关」写成一个状态文件，供 Java 侧读出来回显到界面。
 *
 * 为什么需要它：用户拨开关没反应时，说不清是「设置没传进游戏」还是
 * 「探针不认这份文件」。有了这个文件，界面就能直接显示探针实际的取值，
 * 一眼定位断在哪一环。写入失败只可能是目录不可写，此时静默跳过即可 —— 
 * 诊断信息不值得影响主流程。 */
static void write_status_file(void) {
    char p[320], tmp[340];
    snprintf(p, sizeof(p), "%s/probe.status", base_dir());
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    /* 先写临时文件再 rename：Java 侧每 2 秒读一次这个文件，
       直接覆盖写会让它读到写了一半的内容（比如只有 pid 行没有 ts 行）。 */
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "pid %d\n", (int) getpid());
    fprintf(f, "ts %ld\n", (long) time(NULL));
    fprintf(f, "cfgver %d\n", g_cfg_ver_applied);
    fprintf(f, "skin %d\n", g_skin_enabled);
    fprintf(f, "bgm %d\n", g_bgm_enabled);
    fprintf(f, "badge %d\n", g_badge_enabled);
    fclose(f);
    if (rename(tmp, p) != 0) remove(tmp);
}

static void load_skin_cfg(void) {
    struct stat st;
    char cfgs[320];
    snprintf(cfgs, sizeof(cfgs), SKIN_CFG_FMT, base_dir());
    if (stat(cfgs, &st) != 0) return;
    /* 用「秒 + 纳秒 + 大小」判断变化。只比秒级 mtime 会漏掉同一秒内
       「0 -> 1 -> 0」这种来回改且最终大小不变的编辑，开关拨了却不生效。 */
    if (st.st_mtime == g_cfg_mtime && (long) st.st_size == g_cfg_size
        && (long) st.st_mtim.tv_nsec == g_cfg_nsec) return;
    g_cfg_mtime = st.st_mtime;
    g_cfg_size = (long) st.st_size;
    g_cfg_nsec = (long) st.st_mtim.tv_nsec;
    char cfgp[320];
    snprintf(cfgp, sizeof(cfgp), SKIN_CFG_FMT, base_dir());
    FILE *f = fopen(cfgp, "r");
    if (!f) return;
    for (int i = 0; i < PIN_MAX; i++) g_pins[i].active = 0;
    char line[160];
    int n = 0;
    /* 三个功能开关先攒在局部变量里，等整份文件读完、确认 cfgver 之后再生效。
       这样「文件里没有版本行」的遗留文件不会在解析途中就把开关打开。 */
    int cfg_ver = 0;
    int v_skin = 0, v_bgm = 0, v_badge = 0;
    int has_skin = 0, has_bgm = 0, has_badge = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long off = 0, val = 0;
        int w = 4;
        if (strncmp(line, "cfgver ", 7) == 0) {
            cfg_ver = atoi(line + 7);
            continue;
        }
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
        if (strncmp(line, "gotauto ", 8) == 0) {
            int v = atoi(line + 8) ? 1 : 0;
            if (v != g_auto_got) logline("cfg: gotauto=%d", v);
            g_auto_got = v;
            continue;
        }
        if (strncmp(line, "hold ", 5) == 0) {
            g_hold_enable = atoi(line + 5) ? 1 : 0;
            logline("cfg: hold=%d", g_hold_enable);
            continue;
        }
        if (strncmp(line, "mode ", 5) == 0) {
            g_skin_mode = (strncmp(line + 5, "target", 6) == 0) ? 1 : 0;
            logline("cfg: skin mode=%s", g_skin_mode ? "target" : "legal");
            continue;
        }
        if (strncmp(line, "trigger ", 8) == 0) {
            int t = (strncmp(line + 8, "sta", 3) == 0) ? 1 : 0;
            if (t != g_trigger) logline("cfg: trigger=%s", t ? "sta(legacy)" : "net");
            g_trigger = t;
            continue;
        }
        if (strncmp(line, "fast ", 5) == 0) {
            int on = atoi(line + 5) != 0;
            if (on != (int) g_fast_on) qd_set(on);
            continue;
        }
        /* 两段拥有位图的 skin/bgm 归属对调。默认 0（pos 归皮肤、neg 归 BGM），
           这是 2.5 真机日志推出来的。万一真机上反了，改这一行即可，不用重发版。 */
        if (strncmp(line, "bitmapswap ", 11) == 0) {
            int v = atoi(line + 11) ? 1 : 0;
            if (v != g_bitmap_swap) logline("cfg: bitmapswap=%d", v);
            g_bitmap_swap = v;
            continue;
        }
        /* 三个功能开关。缺省 = 0 = 关。只认 "0"/"1" 之外的写 0，避免手滑写成
           "skin on" 这种让 atoi 返回 0 却又被当成开启的情况。
           这里只记到局部变量，等 cfgver 校验通过后才提交（见函数末尾）。 */
        if (strncmp(line, "skin ", 5) == 0) {
            v_skin = atoi(line + 5) ? 1 : 0;
            has_skin = 1;
            continue;
        }
        if (strncmp(line, "bgm ", 4) == 0) {
            v_bgm = atoi(line + 4) ? 1 : 0;
            has_bgm = 1;
            continue;
        }
        if (strncmp(line, "badge ", 6) == 0) {
            v_badge = atoi(line + 6) ? 1 : 0;
            has_badge = 1;
            continue;
        }
        /* badge_ack 只给界面用，探针不解析，这里显式跳过免得误判成 pin */
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
    /* 版本校验：只有带合法 cfgver 的文件才允许改动三个功能开关。
       旧文件（v2.1/v2.2 遗留）没有这一行 → cfg_ver 为 0 → 全部按关处理。
       注意这里「按关处理」= 把 g_*_enabled 显式置 0，而不是「不动」——
       不动的话上一轮可能已经被旧值打开，关掉开关就永远关不掉了。 */
    if (cfg_ver >= CFG_VER_MIN) {
        if (has_skin) {
            if (v_skin != g_skin_enabled) logline("cfg: skin=%d", v_skin);
            g_skin_enabled = v_skin;
        }
        if (has_bgm) {
            if (v_bgm != g_bgm_enabled) logline("cfg: bgm=%d", v_bgm);
            g_bgm_enabled = v_bgm;
        }
        if (has_badge) {
            if (v_badge != g_badge_enabled) {
                logline("cfg: badge=%d", v_badge);
                /* 从关拨到开：重新允许「一次性盖回本地牌子」。
                   不复位的话，同一次游戏进程里关掉再打开就不会再盖回，
                   看到的现象是「徽章开关开了但牌子还是空的」。 */
                if (v_badge) g_badge_once = 0;
            }
            g_badge_enabled = v_badge;
        }
        if (!has_skin && !has_bgm && !has_badge)
            logline("cfg: ver=%d but no switch lines, keep defaults", cfg_ver);
    } else {
        /* 遗留文件：强制全关。只在值真的变过时打日志，避免每轮刷屏。 */
        if (g_skin_enabled || g_bgm_enabled || g_badge_enabled) {
            logline("cfg: legacy file (no cfgver), forcing all switches off "
                    "(was skin=%d bgm=%d badge=%d)",
                    g_skin_enabled, g_bgm_enabled, g_badge_enabled);
        }
        g_skin_enabled = 0;
        g_bgm_enabled = 0;
        g_badge_enabled = 0;
    }
    /* 一行把「这次重读到底采纳了什么」写清楚。排查时只要 grep 这一行，
       就能分清是文件没变、版本不对、还是开关值没写进去。 */
    logline("cfg: ver=%d switches skin=%d bgm=%d badge=%d, %d pins",
            (cfg_ver >= CFG_VER_MIN) ? cfg_ver : 0,
            g_skin_enabled, g_bgm_enabled, g_badge_enabled, n);
    /* 版本 + 三个开关的最终取值落一个状态文件，给界面回显用。
       用四元组做变化判断：只比 cfgver 会漏掉「版本没变但开关被拨动」的情况。 */
    {
        int ver = (cfg_ver >= CFG_VER_MIN) ? cfg_ver : 0;
        int sig = (ver << 3) | (g_skin_enabled << 2) | (g_bgm_enabled << 1) | g_badge_enabled;
        if (sig != g_status_sig) {
            g_status_sig = sig;
            g_cfg_ver_applied = ver;
            write_status_file();
        }
    }
}

static void *worker(void *arg) {
    (void) arg;
    char last[64] = {0};
    g_boot = time(NULL);
    logline("worker started, watching %s", cmd_path());

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
        /* 先读一遍 skin.cfg：后面的自动解锁要靠它决定打不打位图/准入位。
           原来是放在自动解锁之后，导致首轮读到的是默认全关，一次性的解锁被空转掉。 */
        load_skin_cfg();
        /* 启动后自动解锁一次：等待 savedata 真正加载完成（count 合理）再动手。
           两个开关都关时**不置位** unlocked，这样用户在设置界面里中途打开，
           下一轮就能补上解锁，不用重启游戏。 */
        {
            static int unlocked = 0;
            if (!unlocked) {
                if (armed() && savedata_ready()) {
                    if (g_skin_enabled || g_bgm_enabled) {
                        /* 只按开着的开关打自己那一侧。
                           2.6.2 起打补丁前不再保存任何「原样清单」，关掉时也不还原；
                           游戏重启后会从服务器拿到当前账号的真实数据。 */
                        if (g_skin_enabled) skin_bits_cmd(1);
                        if (g_bgm_enabled) bgm_bits_cmd(1);
                        /* 准入位按 id 分流（皮肤条目 / BGM 条目），只要有一个开着
                           就调一次；savedata_cmd 内部看 id 决定置哪一半。 */
                        savedata_cmd(1);
                        logline("auto-unlock done (skin=%d bgm=%d)",
                                g_skin_enabled, g_bgm_enabled);
                        unlocked = 1;
                    } else {
                        /* 两个都关：完全不动存档。等用户打开开关后再补。
                           关掉开关后需要重启游戏才恢复，设置界面有提示。 */
                        static unsigned skiptries = 0;
                        if ((skiptries++ % 100) == 0)
                            logline("auto-unlock waiting: skin/bgm both off");
                    }
                }
            }
        }
        /* 启动后自动装 GOT 钩子：否则重启游戏后第一次 save_decks 会带着真实皮肤上传 */
        if (!g_got_on && g_auto_got) {
            static unsigned gtries = 0;
            if ((gtries++ % 20) == 0) got_set(1);
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
                skin_bits_cmd(0);
            } else if (strcmp(cmd, "unlockbits") == 0) {
                bgm_bits_cmd(1);
            } else if (strcmp(cmd, "unlockskinbits") == 0) {
                skin_bits_cmd(1);
            } else if (strcmp(cmd, "unlockbgm") == 0) {
                savedata_cmd(1);
            } else if (strcmp(cmd, "dumppad") == 0) {
                dump_libpad();
            } else if (strncmp(cmd, "r ", 2) == 0) {
                unsigned long off = 0, len = 8;
                if (sscanf(cmd + 2, "%lx %lx", &off, &len) >= 1) mem_read_cmd(off, len);
            } else if (strncmp(cmd, "lr ", 3) == 0) {
                /* libpad 相对读：用来看 .got.plt 槽里现在装的是谁 */
                unsigned long off = 0, len = 8;
                if (sscanf(cmd + 3, "%lx %lx", &off, &len) >= 1) {
                    unsigned long lib = libpad_base();
                    if (!lib) logline("lr: no libpad");
                    else {
                        if (len > 64) len = 64;
                        unsigned long a = lib + off;
                        if (!addr_mapped(a) || !addr_mapped(a + len - 1)) logline("lr: off=%#lx unmapped", off);
                        else {
                            char b[200]; int n = 0;
                            for (unsigned long i = 0; i < len; i++)
                                n += snprintf(b + n, sizeof(b) - (size_t) n, "%02x", ((unsigned char *) a)[i]);
                            logline("lr: lib=%#lx off=%#lx [%lu] %s", lib, off, len, b);
                        }
                    }
                }
            } else if (strncmp(cmd, "w16 ", 4) == 0) {
                unsigned long off = 0, val = 0;
                if (sscanf(cmd + 4, "%lx %lx", &off, &val) == 2) mem_write_cmd(2, off, val);
            } else if (strncmp(cmd, "w32 ", 4) == 0) {
                unsigned long off = 0, val = 0;
                if (sscanf(cmd + 4, "%lx %lx", &off, &val) == 2) mem_write_cmd(4, off, val);
            } else if (strncmp(cmd, "find ", 5) == 0) {
                unsigned long val = 0; int size = 2;
                if (sscanf(cmd + 5, "%lx %d", &val, &size) >= 1) find_cmd(val, size);
            } else if (strcmp(cmd, "dumpdec") == 0) {
                /* 从 libpad base + 0x28cdd 读那张 6-bit 表的原始 63 字节，
                   打印可打印字符与十六进制，供校准 B64A。 */
                unsigned long lib = libpad_base();
                if (!lib) logline("dumpdec: no libpad");
                else {
                    const unsigned char *t = (const unsigned char *) (lib + 0x28cdd);
                    char hex[64 * 3 + 1], asc[64];
                    int h = 0;
                    for (int i = 0; i < 63; i++) {
                        h += snprintf(hex + h, sizeof(hex) - (size_t) h, "%02x", t[i]);
                        asc[i] = (t[i] >= 0x20 && t[i] < 0x7f) ? (char) t[i] : '.';
                    }
                    hex[h] = 0; asc[63] = 0;
                    logline("dumpdec: %s", hex);
                    logline("dumpdec: %s", asc);
                }
            } else if (strcmp(cmd, "deckinfo") == 0) {
                unsigned long sv = savedata_ptr();
                if (!sv) { logline("deckinfo: no savedata"); }
                else {
                    unsigned long long h = cred_hash();
                    logline("deckinfo: cred=%016llx slot=%d have=%d switches=%lu",
                            h, g_cur_slot,
                            (g_cur_slot >= 0) ? g_users[g_cur_slot].have : -1, g_user_switches);
                    int n = deck_limit(sv);
                    logline("deckinfo: maxDeckSets=%d (raw=%u)", n,
                            addr_mapped(sv + MAX_DECK_OFF) ? *(volatile unsigned char *) (sv + MAX_DECK_OFF) : 0xff);
                    if (addr_mapped(sv + DECK_REC_BASE)) {
                        unsigned char *q = (unsigned char *) (sv + DECK_REC_BASE);
                        for (int i = 0; i < DECK_COUNT; i++) {
                            unsigned char sk = q[i * DECK_STRIDE + REC_SKIN_OFF];
                            unsigned char bd = q[i * DECK_STRIDE + REC_BADGE_OFF];
                            unsigned char ls = (g_cur_slot >= 0) ? g_users[g_cur_slot].local[i][REC_SKIN_OFF] : 0;
                            logline("deckinfo: %2d skin=%3u badge=%3u local=%3u%s",
                                    i + 1, sk, bd, ls, (i + 1 > n) ? " (beyond limit)" : "");
                        }
                    } else {
                        logline("deckinfo: SKIN_BASE unmapped");
                    }
                }
            } else if (strcmp(cmd, "svdpath") == 0) {
                /* 校验 SKIN_BASE 与 _parseDeckAll 记录区的关系：两者相差多少字节、
                   偏移处各是什么值，用来定死「0x52f3d8 是 0x52f3bc+0x1c」这个推断。 */
                unsigned long sv = savedata_ptr();
                if (!sv) { logline("svdpath: no savedata"); }
                else if (!addr_mapped(sv + SKIN_BASE)) { logline("svdpath: unmapped"); }
                else {
                    logline("svdpath: 0x52f3bc(base) = %0*llx", 16, (unsigned long long) sv);
                    logline("svdpath: 0x52f3d8 - 0x52f3bc = 0x%lx", (unsigned long) (SKIN_BASE - DECK_REC_BASE));
                    unsigned char *q = (unsigned char *) (sv + DECK_REC_BASE);
                    char buf[3 * 16 + 1];
                    int hh = 0;
                    for (int i = 0; i < 16; i++) hh += snprintf(buf + hh, sizeof(buf) - (size_t) hh, "%02x", q[i]);
                    logline("svdpath: rec0[+0x00..0x0f] = %s", buf);
                    hh = 0;
                    for (int i = 0x1c; i < 0x1c + 16; i++) hh += snprintf(buf + hh, sizeof(buf) - (size_t) hh, "%02x", q[i]);
                    logline("svdpath: rec0[+0x1c..0x2b] = %s", buf);
                }
            } else if (strcmp(cmd, "qdr") == 0) {
                qd_report_ex(1);
            } else if (strncmp(cmd, "mark", 4) == 0) {
                logline("MARK %s t=%lums", cmd + 4, watch_now_ms() - g_watch_ms0);
            } else if (strcmp(cmd, "watchall") == 0) {
                g_watch_ms0 = watch_now_ms();
                for (int i = 0; i < DECK_COUNT; i++)
                    watch_add(SKIN_BASE + (unsigned long) i * SKIN_STRIDE, 2);
                logline("watch: armed %d decks", DECK_COUNT);
                fast_watch_start();

            } else if (strcmp(cmd, "fast") == 0) {
                qd_set(1);
                qd_report();
            } else if (strcmp(cmd, "fastoff") == 0) {
                qd_set(0);
            } else if (strncmp(cmd, "gottrace", 8) == 0) {
                g_got_trace = 1;
                got_set(1);
            } else if (strncmp(cmd, "gotoff", 6) == 0) {
                g_got_trace = 0;
                got_set(0);
            } else if (strncmp(cmd, "badgeon", 7) == 0) {
                g_badge_enabled = 1;
                logline("badge: unlock=%d", g_badge_enabled);
            } else if (strncmp(cmd, "badgeoff", 8) == 0) {
                g_badge_enabled = 0;
                logline("badge: unlock=%d", g_badge_enabled);
            } else if (strncmp(cmd, "gotauto", 7) == 0) {
                g_auto_got = atoi(cmd + 7) ? 1 : 0;
                logline("got: auto=%d", g_auto_got);
            } else if (strcmp(cmd, "defon") == 0) {
                g_def_on = 1;
                logline("def: ON (upload skin/bgm/badge -> default)");
            } else if (strcmp(cmd, "defoff") == 0) {
                g_def_on = 0;
                logline("def: OFF (upload original values)");
            } else if (strncmp(cmd, "bfind ", 6) == 0) {
                unsigned long val = 0, off = 0, len = 0x1000;
                int k = sscanf(cmd + 6, "%lx %lx %lx", &val, &off, &len);
                if (k >= 1) bfind_cmd(val, off, len);
                else logline("bfind: usage bfind <u32> [off] [len]");
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
        /* 每轮先读开关，再做事。原来 load_skin_cfg() 排在 quiet_reapply() 之后，
           于是拨动开关后要等下一轮（50ms）才生效，且「关掉」那一轮还会按旧值
           再打一次补丁 —— 手感上就是「关了没关掉」。 */
        load_skin_cfg();
        /* 50ms 一轮：把「服务器覆盖 -> 我们改回」的窗口压到最小 */
        if (g_auto) quiet_reapply();
        watch_sample();
        if (g_new_mode == 5) {
            static time_t last5 = 0;
            time_t now5 = time(NULL);
            if (now5 - last5 >= 2) { last5 = now5; g_new_dirty = 1; }
        }
        if (g_new_dirty && new_apply()) g_new_dirty = 0;
        /* 心跳：每 10 秒把状态文件的时间戳刷一次。界面靠它判断探针是否还活着
           （游戏进程被杀/换进程后旧状态文件会留在原地，光看内容分不出新旧）。 */
        {
            static long last_status = 0;
            long now = (long) time(NULL);
            if (now - last_status >= 10) {
                last_status = now;
                write_status_file();
            }
        }
        pin_apply();
        skin_apply();
        if (g_trigger == 1) sta_watch();
        {
            /* 每 5 秒打一行状态，方便用户「一眼看出」当前探针认为每队该是什么皮肤 */
            static unsigned long last_rep = 0;
            if (g_cur_slot >= 0 && g_users[g_cur_slot].have && now_ms() - last_rep >= 5000UL) {
                last_rep = now_ms();
                qd_report();
            }
        }
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 50L * 1000L * 1000L;
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* 只用一个目录：游戏私有目录。外置目录只作为「一次性回迁来源」。 */
#define BASE_DIR_EXTERNAL "/storage/emulated/0/Android/data/jp.gungho.pad/files/padhelper"

static void copy_missing(const char *from, const char *to) {
    DIR *d = opendir(from);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "skins_", 6) != 0 && strcmp(e->d_name, "skin.cfg") != 0) continue;
        char s[320], t[320];
        snprintf(s, sizeof(s), "%s/%s", from, e->d_name);
        snprintf(t, sizeof(t), "%s/%s", to, e->d_name);
        if (access(t, F_OK) == 0) continue;
        FILE *fi = fopen(s, "rb");
        if (!fi) continue;
        FILE *fo = fopen(t, "wb");
        if (fo) {
            char b[4096];
            size_t n;
            while ((n = fread(b, 1, sizeof(b), fi)) > 0) fwrite(b, 1, n, fo);
            fclose(fo);
            logline("migrate: %s -> %s", s, t);
        }
        fclose(fi);
    }
    closedir(d);
}

static void pick_base(void) {
    mkdir(BASE_DIR_INTERNAL, 0770);
    snprintf(g_base, sizeof(g_base), "%s", BASE_DIR_INTERNAL);
}

/* 之前外置目录里的本地存档/skin.cfg 回迁到私有目录 */
static void migrate_files(void) {
    copy_missing(BASE_DIR_EXTERNAL, base_dir());
    copy_missing("/storage/emulated/0/padhelper", base_dir());
}

__attribute__((constructor))
static void padhelper_native_init(void) {
    pick_base();
    /* 打开时先量一下已有文件多大，否则新进程的计数器从 0 开始，
       一份已经 50 MB 的旧日志要再写满 1 MB 才会触发轮转。 */
    {
        struct stat st;
        if (stat(log_path(), &st) == 0 && st.st_size > 0) g_log_bytes = (long) st.st_size;
    }
    g_log = fopen(log_path(), "a");
    if (g_log && g_log_bytes > PROBE_LOG_MAX) log_rotate();
    logline("base dir: %s", base_dir());
    migrate_files();
    logline("native probe loaded: pid=%d uid=%d", (int) getpid(), (int) getuid());

    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL) == 0) {
        pthread_detach(th);
    } else {
        logline("failed to start worker");
    }
}
