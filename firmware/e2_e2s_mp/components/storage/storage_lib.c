/* storage_lib.c — 书目扫描、开/关书、删书与 text_source_t 的读路径。
 *
 * DESIGN.md §6.1(只碰 /sdcard/books 与 /sdcard/reader 子树)、§8
 * (storage_read_at 用互斥量包住 fseek/fread)、§6.6(SD 丢失规则)。
 *
 * 书目缓存:64 本 × storage_book_t,静态占用 ≈17 KB(§3.2 预算)。
 * id = FNV-1a32(绝对路径) 的 8 位十六进制,进度/书签都以它为文件名。
 * 读路径返回 0 表示成功(text_source_t 约定),短读只可能发生在文件尾,
 * 由调用方用 src.size 截断请求。
 *
 * 文件名编码(S8 的权衡,记在这里备查;sdkconfig 不归本组件管):
 * CONFIG_FATFS_CODEPAGE=437 + CONFIG_FATFS_API_ENCODING_ANSI_OEM=y。
 *   - 设备自己写的名字(上传名)是 UTF-8 字节流,按 OEM 单字节经
 *     ff_oem2uni 存进 LFN(UTF-16);CP437 的 128 个扩展码位互不相同,
 *     读回来 ff_uni2oem 逐字节可逆,所以本机读 /sdcard/books 的 UTF-8
 *     书名列目录能原样往返(PC 上看这些名字是 CP437 乱码,属预期)。
 *   - PC 用 UTF-16 LFN 建的中文书名,读目录时 uni→OEM(CP437) 转换失败,
 *     put_utf 返回 0,FatFs 整条 LFN 作废、回退到 8.3 短名(非 ASCII 字符
 *     在 SFN 里被 ff_uni2oem 换成 '_',形如 "_____~1.TXT")。书仍能打开
 *     (短名是合法别名),只是显示名丢了,不再是中文。
 *   - 换 CP936 能让 PC 的中文书名往返,但设备写入的 UTF-8 字节会被当成
 *     GBK 双字节解释,大部分会被丢弃/替换,上传名反而坏掉;而且 UTF-8
 *     中文名在两处(UPL 头、目录项)的字节一致性也被破坏。
 *   结论:保持 437,上传/改名一律走 BLE;PC 直拷建议用 ASCII 名。
 */
#include "storage_internal.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

#include "rc_common.h"

static const char *TAG = ST_TAG;

#define ST_LIB_MAX 64           /* DESIGN §3.2:64 本 × (64+192+16) */

static storage_book_t s_books[ST_LIB_MAX];
static int s_book_count;

static FILE *s_file;            /* 当前打开的书,仅 storage_lib 使用 */
static uint64_t s_file_size;
static storage_book_t s_current;
static bool s_has_current;

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

static bool has_txt_suffix(const char *name)
{
    size_t n = strlen(name);
    if (n < 5) {
        return false;                   /* 至少 "a.txt" */
    }
    const char *e = name + n - 4;
    if (e[0] != '.') {
        return false;
    }
    /* FAT 本身大小写不敏感,Windows 拷来的 .TXT 也应当能读 */
    return (e[1] == 't' || e[1] == 'T') && (e[2] == 'x' || e[2] == 'X') &&
           (e[3] == 't' || e[3] == 'T');
}

/* 逐个路径分量检查 "..",不误伤 "a..b.txt" 这样的合法文件名 */
static bool has_dotdot_component(const char *p)
{
    while (*p) {
        while (*p == '/') {
            p++;
        }
        const char *s = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t l = (size_t)(p - s);
        if (l == 2 && s[0] == '.' && s[1] == '.') {
            return true;
        }
    }
    return false;
}

static bool book_path_ok(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    if (has_dotdot_component(path)) {
        return false;                   /* 禁止路径穿越 */
    }
    static const char prefix[] = READER_DIR_BOOKS "/";
    size_t pl = sizeof(prefix) - 1;
    if (strncmp(path, prefix, pl) != 0) {
        return false;                   /* 只允许 books 目录内的书 */
    }
    if (path[pl] == '\0') {
        return false;
    }
    return strchr(path + pl, '\\') == NULL;
}

static int cmp_book_name(const void *a, const void *b)
{
    const storage_book_t *ba = (const storage_book_t *)a;
    const storage_book_t *bb = (const storage_book_t *)b;
    return strcmp(ba->name, bb->name);
}

/* 任何读写错误:关 FILE*、置 s_sd_ok=false(§6.6)。 */
static void lib_io_fail(const char *why)
{
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    s_file_size = 0;
    s_has_current = false;
    st_sd_fail(why);
}

/* ------------------------------------------------------------------ */
/* 目录扫描                                                           */
/* ------------------------------------------------------------------ */

int st_lib_scan(void)
{
    s_book_count = 0;

    DIR *d = opendir(READER_DIR_BOOKS);
    if (!d) {
        int e = errno;
        if (e == ENOENT || e == ENOTDIR) {
            ESP_LOGI(TAG, "no book directory yet (%s)", READER_DIR_BOOKS);
            return ST_OK;               /* 没有目录 = 没有书,不是卡故障 */
        }
        st_sd_fail("opendir(books) failed");
        return ST_ERR_IO;
    }

    int n = 0;
    bool truncated = false;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(d);
        if (!de) {
            if (errno != 0) {
                ESP_LOGW(TAG, "readdir(%s) failed (errno=%d)", READER_DIR_BOOKS, errno);
                closedir(d);
                s_book_count = 0;
                st_sd_fail("readdir(books) failed");
                return ST_ERR_IO;
            }
            break;
        }
        const char *name = de->d_name;
        if (!name[0] || name[0] == '.') {
            continue;                   /* 跳过 "." / ".." / 隐藏文件 */
        }
        if (!has_txt_suffix(name)) {
            continue;
        }
        size_t nl = strlen(name);
        if (nl > READER_LIB_NAME_MAX) {
            ESP_LOGW(TAG, "skip %s: name %u B > %d", name, (unsigned)nl,
                     READER_LIB_NAME_MAX);
            continue;
        }

        char path[ST_PATH_MAX];
        int w = snprintf(path, sizeof(path), "%s/%s", READER_DIR_BOOKS, name);
        if (w <= 0 || (size_t)w >= sizeof(path)) {
            ESP_LOGW(TAG, "skip %s: path too long", name);
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            ESP_LOGW(TAG, "skip %s: stat errno=%d", name, errno);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        if (st.st_size < 0) {
            /* VFS 的 off_t 是 32 位;>2 GB 的书无法用 fseek 定位 */
            ESP_LOGW(TAG, "skip %s: beyond 32-bit file offset", name);
            continue;
        }
        if (n >= ST_LIB_MAX) {
            truncated = true;
            break;
        }

        storage_book_t *b = &s_books[n++];
        memset(b, 0, sizeof(*b));
        memcpy(b->name, name, nl + 1);
        memcpy(b->path, path, (size_t)w + 1);
        b->id = rc_fnv1a32(b->path);
        b->size = (uint64_t)st.st_size;
    }
    closedir(d);

    if (n > 1) {
        qsort(s_books, (size_t)n, sizeof(s_books[0]), cmp_book_name);
    }
    s_book_count = n;
    if (truncated) {
        ESP_LOGW(TAG, "book list truncated to %d entries", ST_LIB_MAX);
    }
    ESP_LOGI(TAG, "book scan: %d book(s)", s_book_count);
    return ST_OK;
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                           */
/* ------------------------------------------------------------------ */

int storage_list(storage_book_t *out, int max)
{
    if (!out || max <= 0) {
        return ST_ERR_BAD_PARAM;
    }
    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }
    int n = (s_book_count < max) ? s_book_count : max;
    if (n > 0) {
        memcpy(out, s_books, (size_t)n * sizeof(s_books[0]));
    }
    st_unlock();
    return n;
}

int storage_open_book(const storage_book_t *b)
{
    if (!b) {
        return ST_ERR_BAD_PARAM;
    }
    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }

    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    s_file_size = 0;
    s_has_current = false;

    if (!book_path_ok(b->path)) {
        rc = ST_ERR_BAD_PARAM;
        goto out;
    }

    FILE *f = fopen(b->path, "rb");
    if (!f) {
        int e = errno;
        if (e == ENOENT || e == ENOTDIR) {
            rc = ST_ERR_NOT_FOUND;      /* 书被删了不是卡故障 */
        } else {
            st_sd_fail("fopen(book) failed");
            rc = ST_ERR_IO;
        }
        goto out;
    }

    struct stat st;
    if (stat(b->path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        ESP_LOGW(TAG, "book vanished after open: %s", b->path);
        rc = ST_ERR_NOT_FOUND;
        goto out;
    }
    if (st.st_size < 0) {
        fclose(f);
        rc = ST_ERR_SIZE;
        goto out;
    }

    s_file = f;
    s_file_size = (uint64_t)st.st_size;
    s_current = *b;
    /* id 以绝对路径为准,避免调用方带入过期/伪造的 id */
    s_current.id = rc_fnv1a32(s_current.path);
    s_current.size = s_file_size;
    s_has_current = true;
    ESP_LOGI(TAG, "open book %s (id=%08x, %llu B)", s_current.path, (unsigned)s_current.id,
             (unsigned long long)s_current.size);
    rc = ST_OK;

out:
    st_unlock();
    return rc;
}

st_err_t storage_close_book(void)
{
    /* 关句柄不能依赖 s_sd_ok:读失败后卡已被标记失效,但书还开着。
     *
     * 这里只等 ST_LOCK_MS(2 s):上传 END 会在持锁状态下拷贝整本书
     * (≤32 MB,数十秒)。原来的 ST_WAIT_FOREVER 会让 MENU/回书架这类
     * 调用方一直卡在 fclose 上,整个 UI 冻结。
     * 超时返回 ST_ERR_BUSY 且句柄保持打开 —— 调用方稍后重试即可;
     * 下一次 close_book/open_book/storage_remount 都会再关一次,
     * 不会泄漏 FILE*。 */
    int rc = st_lock(ST_LOCK_MS);
    if (rc != ST_OK) {
        /* ST_ERR_BUSY:END 持锁;ST_ERR_NO_SD:互斥量还没建,不可能有句柄 */
        return (st_err_t)rc;
    }
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    s_file_size = 0;
    s_has_current = false;
    st_unlock();
    return ST_OK;
}

/* 删除一本书及其边车(§6.1 命名:<id>.pos / <id>.bmk0 / <id>.bmk1)。
 * 边车以"传入路径的 FNV-1a32"为 id,与 storage_list 填进 path 的字节序列
 * 必须完全一致,所以调用方要原样回传 storage_book_t.path。 */
st_err_t storage_delete_book(const char *path)
{
    if (!book_path_ok(path)) {
        st_sd_set_error("delete: path outside " READER_DIR_BOOKS);
        return ST_ERR_BAD_PARAM;
    }

    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return (st_err_t)rc;            /* NO_SD / BUSY(可重试) */
    }

    /* 正在读的书被删:先关句柄。FatFs 允许 unlink 打开的文件,但目录项与
     * 簇已经被回收,继续 fread 会读到后来者写的数据,所以不能留着。 */
    if (s_has_current && strcmp(s_current.path, path) == 0) {
        if (s_file) {
            fclose(s_file);
            s_file = NULL;
        }
        s_file_size = 0;
        s_has_current = false;
        ESP_LOGW(TAG, "delete: closing the open book %s", path);
    }

    bool found = true;
    errno = 0;
    if (unlink(path) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            found = false;              /* 书本来就不在:边车仍要清 */
        } else {
            ESP_LOGE(TAG, "unlink(%s) failed (errno=%d)", path, errno);
            st_sd_set_error("unlink(book) failed");
            st_unlock();
            return ST_ERR_IO;
        }
    }

    /* 边车缺失不算错(没读过/没加过书签的书本来就没有) */
    uint32_t id = rc_fnv1a32(path);
    static const char *const suffixes[] = { ".pos", ".bmk0", ".bmk1" };
    bool side_err = false;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char side[ST_PATH_MAX];
        int w = snprintf(side, sizeof(side), "%s/%08" PRIx32 "%s", READER_DIR_STATE, id,
                         suffixes[i]);
        if (w <= 0 || (size_t)w >= sizeof(side)) {
            side_err = true;
            continue;
        }
        errno = 0;
        if (unlink(side) != 0 && errno != ENOENT) {
            ESP_LOGW(TAG, "unlink(%s) failed (errno=%d)", side, errno);
            side_err = true;
        }
    }

    /* 书目缓存同步:调用方就算直接 storage_list 也不该再看到这本。
     * books/<name>.part 故意不动:它属于可能还在进行的内存上传会话,
     * END 发布时会用 "wb" 重写。 */
    if (st_lib_scan() != ST_OK) {
        side_err = true;                /* 扫描失败已置 s_sd_ok=false */
    }

    st_unlock();
    if (!found) {
        return ST_ERR_NOT_FOUND;
    }
    return side_err ? ST_ERR_IO : ST_OK;
}

/* text_source_t 的读实现(§8)。语义:
 *   - 返回 ST_OK(0) 表示成功。text_source_t 没有"读了多少"的出参,
 *     所以文件尾的短读也返回 0:调用方必须先用 src.size 把请求截到
 *     文件内(text 侧全部读取点都已按 b->size 截断)。
 *   - off == size:合法,读 0 字节,返回 0(buf 内容不动)。
 *   - off > size:ST_ERR_BAD_PARAM(调用方算错了偏移);fail-fast 比静默
 *     返回 0 更容易在验收里发现分页溢出。
 *   - 取锁超时(上传 END 正在拷贝)返回 ST_ERR_BUSY,可重试。 */
int storage_read_at(void *ctx, uint64_t off, void *buf, size_t len)
{
    (void)ctx;                          /* text_source_t 的 ctx 未使用 */

    if (!buf && len > 0) {
        return ST_ERR_BAD_PARAM;
    }
    if (len == 0) {
        return ST_OK;
    }

    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }
    if (!s_file) {
        rc = ST_ERR_NOT_FOUND;
        goto out;
    }
    if (off > s_file_size) {
        rc = ST_ERR_BAD_PARAM;
        goto out;
    }
    if (off == s_file_size) {
        rc = ST_OK;                     /* 文件尾:读 0 字节 */
        goto out;
    }

    size_t want = len;
    if ((uint64_t)want > s_file_size - off) {
        want = (size_t)(s_file_size - off);   /* 尾部短读,不算错误 */
    }
    if (off > 0x7FFFFFFFull) {                /* 32 位 VFS 偏移保护 */
        rc = ST_ERR_BAD_PARAM;
        goto out;
    }
    if (fseek(s_file, (long)off, SEEK_SET) != 0) {
        lib_io_fail("fseek(book) failed");
        rc = ST_ERR_IO;
        goto out;
    }
    size_t got = fread(buf, 1, want, s_file);
    if (got != want) {
        lib_io_fail("fread(book) failed");
        rc = ST_ERR_IO;
        goto out;
    }
    rc = ST_OK;

out:
    st_unlock();
    return rc;
}

const storage_book_t *storage_current_book(void)
{
    return s_has_current ? &s_current : NULL;
}
