/*
 * Quran PS5 — مصحف المدينة مع الفهرس والتلاوة
 *
 * - صفحات المصحف (604) تُنزَّل من الإنترنت وتُحفظ في الكاش
 * - صفحتان متجاورتان على الشاشة العريضة (الفردية يمين)
 * - الفهرس: السور والأجزاء (△)
 * - التلاوة آية بآية من everyayah.com (□)، مع تقليب الصفحات تلقائيًا
 * - اختيار القارئ (○)
 *
 * Linux: SDL2 + libcurl. PS5: نفس الكود مع QURAN_PS5 (native title).
 */
#include <SDL2/SDL.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#include "quran_data.h"
#include "quran_layout.h"

#define PAGE_COUNT 604
#define PAGE_URL "https://raw.githubusercontent.com/zeyadetman/quran-pages-images/master/quran-images/%d.jpg"
#define AYAH_URL "https://everyayah.com/data/%s/%03d%03d.mp3"
#define TEX_SLOTS 8
#define PREFETCH_RADIUS 3

#ifdef QURAN_PS5
#define DEFAULT_CACHE "/data/QuranPS5"
#define UI_DIR "/app0/assets/ui"
#define CA_BUNDLE "/app0/assets/ca-bundle.crt"
#define WIN_W 3840
#define WIN_H 2160
#define WIN_FLAGS SDL_WINDOW_SHOWN
#define REN_FLAGS SDL_RENDERER_SOFTWARE
#define GETENV(n) ((const char *)0)
#else
#define DEFAULT_CACHE "./cache"
#define UI_DIR "./assets/ui"
#define WIN_W 1280
#define WIN_H 720
#define WIN_FLAGS (SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI)
#define REN_FLAGS (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
#define GETENV(n) getenv(n)
#endif

enum { ST_NONE = 0, ST_DOWNLOADING, ST_READY, ST_FAILED };

/* ---------------- log ---------------- */
static FILE *g_log;
static SDL_mutex *g_log_lock;
#define LOG(...) do { if (g_log) { if (g_log_lock) SDL_LockMutex(g_log_lock); \
    fprintf(g_log, "[%7u] ", (unsigned)SDL_GetTicks()); fprintf(g_log, __VA_ARGS__); \
    fputc('\n', g_log); fflush(g_log); if (g_log_lock) SDL_UnlockMutex(g_log_lock); } } while (0)

static char g_cache_dir[512] = DEFAULT_CACHE;
static SDL_atomic_t g_state[PAGE_COUNT + 1];
static SDL_atomic_t g_want_page;
static SDL_atomic_t g_quit;
static SDL_sem *g_wake;

/* ---------------- files ---------------- */
static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && st.st_size > 0; }

static void mkdir_p(const char *dir) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    mkdir(tmp, 0777);
}

static void page_path(int n, char *out, size_t sz) { snprintf(out, sz, "%s/%03d.jpg", g_cache_dir, n); }

static int read_int_setting(const char *name, int def) {
    char p[600]; snprintf(p, sizeof p, "%s/%s.txt", g_cache_dir, name);
    FILE *f = fopen(p, "r"); int v = def;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = def; fclose(f); }
    return v;
}
static void write_int_setting(const char *name, int v) {
    char p[600]; snprintf(p, sizeof p, "%s/%s.txt", g_cache_dir, name);
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d\n", v); fclose(f); }
}

/* ---------------- quran helpers ---------------- */
static int ayah_key(int s, int a) { return s * 1000 + a; }
static int page_of(int s, int a) {           /* last page whose first ayah <= (s, a) */
    int k = ayah_key(s, a), lo = 0, hi = PAGE_COUNT - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (ayah_key(PAGE_FIRST[mid][0], PAGE_FIRST[mid][1]) <= k) lo = mid; else hi = mid - 1;
    }
    return lo + 1;
}
static int sura_of_page(int p) {             /* the sura being read at the top of page p */
    int s = 1;
    for (int i = 0; i < SURA_COUNT; i++) if (SURA_PAGE[i] <= p) s = i + 1;
    return PAGE_FIRST[p - 1][0] < s ? s : PAGE_FIRST[p - 1][0];
}
static int juz_of_page(int p) { int j = 0; for (int i = 0; i < JUZ_COUNT; i++) if (JUZ_PAGE[i] <= p) j = i; return j; }

/* ---------------- http ---------------- */
typedef struct { unsigned char *data; size_t len, cap; } Buf;

static size_t on_data(void *ptr, size_t sz, size_t nm, void *ud) {
    Buf *b = (Buf *)ud; size_t n = sz * nm;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256 * 1024;
        while (nc < b->len + n) nc *= 2;
        unsigned char *nd = (unsigned char *)realloc(b->data, nc);
        if (!nd) return 0;
        b->data = nd; b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, n); b->len += n;
    return n;
}

static int http_get(CURL *c, const char *url, Buf *b) {
    char errbuf[CURL_ERROR_SIZE]; errbuf[0] = 0;
    b->len = 0;
    curl_easy_reset(c);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "QuranPS5/1.0");
#ifdef CA_BUNDLE
    curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE);
#endif
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) { LOG("GET %s failed: %s %s", url, curl_easy_strerror(rc), errbuf); return 0; }
    return 1;
}

static int save_file(const char *path, const unsigned char *d, size_t n) {
    char tmp[700]; snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return 0;
    int ok = fwrite(d, 1, n, f) == n; fclose(f);
    return ok && rename(tmp, path) == 0;
}

/* ---------------- page downloader ---------------- */
static int downloader(void *ud) {
    (void)ud;
    CURL *c = curl_easy_init();
    Buf b = {0};
    while (!SDL_AtomicGet(&g_quit)) {
        int center = SDL_AtomicGet(&g_want_page), target = 0;
        for (int d = 0; d <= PREFETCH_RADIUS * 2 && !target; d++) {     /* 0, +1, -1, +2, -2 ... */
            int off = (d + 1) / 2, n = center + ((d & 1) ? off : -off);
            if (n >= 1 && n <= PAGE_COUNT && SDL_AtomicGet(&g_state[n]) == ST_NONE) target = n;
        }
        if (!target) { SDL_SemWaitTimeout(g_wake, 500); continue; }
        SDL_AtomicSet(&g_state[target], ST_DOWNLOADING);
        char url[256], path[600];
        snprintf(url, sizeof url, PAGE_URL, target);
        page_path(target, path, sizeof path);
        int ok = http_get(c, url, &b) && b.len > 1024 && b.data[0] == 0xFF && b.data[1] == 0xD8 &&
                 save_file(path, b.data, b.len);
        SDL_AtomicSet(&g_state[target], ok ? ST_READY : ST_FAILED);
    }
    free(b.data);
    curl_easy_cleanup(c);
    return 0;
}

/* ---------------- recitation ---------------- */
enum { AU_IDLE = 0, AU_LOADING, AU_PLAYING, AU_ERROR };
static SDL_AudioDeviceID g_dev;
static SDL_AudioSpec g_have;
static SDL_atomic_t g_play_gen;      /* bumped on every (re)start */
static SDL_atomic_t g_play_start;    /* ayah_key to start from */
static SDL_atomic_t g_playing;       /* 1 while the reciter thread should feed audio */
static SDL_atomic_t g_audible;       /* ayah_key currently heard */
static SDL_atomic_t g_au_status;
static SDL_atomic_t g_reciter;
static SDL_sem *g_audio_wake;

typedef struct { int key; Uint32 bytes; } Clip;

static int load_ayah_mp3(CURL *c, int rec, int s, int a, Buf *b) {
    char dir[700], path[760], url[300];
    snprintf(dir, sizeof dir, "%s/audio/%s", g_cache_dir, RECITER_FOLDER[rec]);
    snprintf(path, sizeof path, "%s/%03d%03d.mp3", dir, s, a);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        b->len = 0;
        if (n > 0 && (size_t)n <= b->cap) { b->len = fread(b->data, 1, (size_t)n, f); }
        else if (n > 0) { free(b->data); b->data = (unsigned char *)malloc((size_t)n); b->cap = b->data ? (size_t)n : 0;
                          if (b->data) b->len = fread(b->data, 1, (size_t)n, f); }
        fclose(f);
        if (b->len > 512) return 1;
    }
    snprintf(url, sizeof url, AYAH_URL, RECITER_FOLDER[rec], s, a);
    const char *base = GETENV("QURAN_AUDIO_URL");            /* tests: file:///.../%s/%03d%03d.mp3 */
    if (base) snprintf(url, sizeof url, base, RECITER_FOLDER[rec], s, a);
    if (!http_get(c, url, b) || b->len < 512) return 0;
    mkdir_p(dir);
    save_file(path, b->data, b->len);
    return 1;
}

/* decode an MP3 into the device format; returns malloc'd PCM */
static Uint8 *decode_to_device(const Buf *b, Uint32 *out_len) {
    drmp3_config cfg; drmp3_uint64 frames = 0;
    drmp3_int16 *pcm = drmp3_open_memory_and_read_pcm_frames_s16(b->data, b->len, &cfg, &frames, NULL);
    if (!pcm || !frames) { if (pcm) drmp3_free(pcm, NULL); return NULL; }
    SDL_AudioStream *st = SDL_NewAudioStream(AUDIO_S16SYS, (Uint8)cfg.channels, (int)cfg.sampleRate,
                                             g_have.format, g_have.channels, g_have.freq);
    if (!st) { drmp3_free(pcm, NULL); return NULL; }
    SDL_AudioStreamPut(st, pcm, (int)(frames * cfg.channels * sizeof(drmp3_int16)));
    SDL_AudioStreamFlush(st);
    drmp3_free(pcm, NULL);
    int avail = SDL_AudioStreamAvailable(st);
    Uint8 *out = (Uint8 *)malloc(avail > 0 ? (size_t)avail : 1);
    int got = out ? SDL_AudioStreamGet(st, out, avail) : 0;
    SDL_FreeAudioStream(st);
    if (got <= 0) { free(out); return NULL; }
    *out_len = (Uint32)got;
    return out;
}

static int reciter_thread(void *ud) {
    (void)ud;
    CURL *c = curl_easy_init();
    Buf b = {0};
    Clip ring[8]; int ring_n = 0; Uint32 ring_bytes = 0;
    int gen_seen = -1, cur_s = 1, cur_a = 1, basmala = 0, at_end = 0, rec_used = -1;
    while (!SDL_AtomicGet(&g_quit)) {
        if (!SDL_AtomicGet(&g_playing)) { SDL_SemWaitTimeout(g_audio_wake, 200); gen_seen = -1; continue; }
        int gen = SDL_AtomicGet(&g_play_gen), rec = SDL_AtomicGet(&g_reciter);
        if (gen != gen_seen || rec != rec_used) {
            SDL_ClearQueuedAudio(g_dev);
            int k = SDL_AtomicGet(&g_play_start);
            cur_s = k / 1000; cur_a = k % 1000;
            basmala = (cur_a == 1 && cur_s != 1 && cur_s != 9);
            ring_n = 0; ring_bytes = 0; at_end = 0;
            gen_seen = gen; rec_used = rec;
            SDL_AtomicSet(&g_audible, k);
            SDL_AtomicSet(&g_au_status, AU_LOADING);
            SDL_PauseAudioDevice(g_dev, 0);
        }
        /* which queued clip is being heard */
        Uint32 q = SDL_GetQueuedAudioSize(g_dev);
        Uint32 consumed = ring_bytes > q ? ring_bytes - q : 0;
        while (ring_n > 0 && ring[0].bytes <= consumed) {
            consumed -= ring[0].bytes; ring_bytes -= ring[0].bytes;
            memmove(ring, ring + 1, sizeof(Clip) * (size_t)(--ring_n));
        }
        if (ring_n > 0) { SDL_AtomicSet(&g_audible, ring[0].key); SDL_AtomicSet(&g_au_status, AU_PLAYING); }
        if (at_end && ring_n == 0 && q == 0) { SDL_AtomicSet(&g_playing, 0); SDL_AtomicSet(&g_au_status, AU_IDLE); continue; }

        if (at_end || ring_n >= 2) { SDL_Delay(30); continue; }    /* one clip playing + one ready */

        int s = basmala ? 1 : cur_s, a = basmala ? 1 : cur_a;
        if (!load_ayah_mp3(c, rec, s, a, &b)) {
            LOG("recitation: could not load %s %03d%03d", RECITER_FOLDER[rec], s, a);
            if (ring_n == 0) { SDL_AtomicSet(&g_au_status, AU_ERROR); SDL_AtomicSet(&g_playing, 0); }
            else SDL_Delay(1000);
            continue;
        }
        Uint32 n = 0;
        Uint8 *pcm = decode_to_device(&b, &n);
        if (SDL_AtomicGet(&g_play_gen) != gen_seen || SDL_AtomicGet(&g_reciter) != rec_used) { free(pcm); continue; }
        if (!pcm) {
            LOG("recitation: bad mp3 %03d%03d (%u bytes)", s, a, (unsigned)b.len);
        } else {
            SDL_QueueAudio(g_dev, pcm, n);
            free(pcm);
            LOG("recite %03d%03d (%u ms)", s, a, (unsigned)(n * 1000ull / ((Uint64)g_have.freq * g_have.channels * 2)));
            ring[ring_n].key = ayah_key(cur_s, basmala ? 1 : cur_a);
            ring[ring_n].bytes = n; ring_n++; ring_bytes += n;
        }
        if (basmala) { basmala = 0; continue; }
        if (++cur_a > SURA_AYAHS[cur_s - 1]) {
            if (cur_s == SURA_COUNT) at_end = 1;
            else { cur_s++; cur_a = 1; basmala = (cur_s != 9); }
        }
    }
    free(b.data);
    curl_easy_cleanup(c);
    return 0;
}

/* ---------------- page textures ---------------- */
typedef struct { int page; SDL_Texture *tex; int w, h; Uint32 used; } Slot;
static Slot g_slots[TEX_SLOTS];

static Slot *get_texture(SDL_Renderer *r, int n) {
    if (n < 1 || n > PAGE_COUNT) return NULL;
    for (int i = 0; i < TEX_SLOTS; i++)
        if (g_slots[i].tex && g_slots[i].page == n) { g_slots[i].used = SDL_GetTicks(); return &g_slots[i]; }
    if (SDL_AtomicGet(&g_state[n]) != ST_READY) return NULL;
    char path[600]; page_path(n, path, sizeof path);
    int w, h, ch;
    unsigned char *px = stbi_load(path, &w, &h, &ch, 4);
    if (!px) { remove(path); SDL_AtomicSet(&g_state[n], ST_NONE); SDL_SemPost(g_wake); return NULL; }
    int lru = 0;
    for (int i = 1; i < TEX_SLOTS; i++)
        if (!g_slots[i].tex || (g_slots[lru].tex && g_slots[i].used < g_slots[lru].used)) lru = i;
    Slot *s = &g_slots[lru];
    if (s->tex) SDL_DestroyTexture(s->tex);
    s->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    SDL_SetTextureScaleMode(s->tex, SDL_ScaleModeBest);
    SDL_UpdateTexture(s->tex, NULL, px, w * 4);
    stbi_image_free(px);
    s->page = n; s->w = w; s->h = h; s->used = SDL_GetTicks();
    return s;
}

/* ---------------- UI images ---------------- */
typedef struct { SDL_Texture *t; SDL_Rect src; int w, h; } Img;
enum { IM_SURA0 = 0, IM_DIGIT0 = IM_SURA0 + SURA_COUNT, IM_RECITER0 = IM_DIGIT0 + 10,
       IM_INDEX = IM_RECITER0 + RECITER_COUNT, IM_SURAS, IM_JUZ_TAB, IM_JUZ, IM_PAGE, IM_RECITER, IM_PLAYING,
       IM_PAUSED, IM_LOADING, IM_AUDIO_ERR, IM_AYAH, IM_HINT_MAIN, IM_HINT_INDEX, IM_HINT_RECITER, IM_COUNT };
static Img g_img[IM_COUNT];
static SDL_Texture *g_atlas;
static float g_u = 1.0f;          /* 1 unit = 1 px at 1080p */

/* All labels go into ONE atlas texture. On the console, allocations under the mapping threshold come
   from a ~16 MiB libc heap; 146 separate label textures filled most of it and starved OpenSSL. */
static void load_ui(SDL_Renderer *r) {
    static const char *words[] = { "w_index", "w_suras", "w_juz_tab", "w_juz", "w_page", "w_reciter", "w_playing",
        "w_paused", "w_loading", "w_audio_err", "w_ayah", "hint_main", "hint_index", "hint_reciter" };
    static unsigned char *px[IM_COUNT];
    char name[64], path[600];
    const int AW = 2048, PAD = 3;
    int x = PAD, y = PAD, shelf = 0;
    for (int id = 0; id < IM_COUNT; id++) {
        if (id < IM_DIGIT0) snprintf(name, sizeof name, "sura_%03d", id - IM_SURA0 + 1);
        else if (id < IM_RECITER0) snprintf(name, sizeof name, "digit_%d", id - IM_DIGIT0);
        else if (id < IM_INDEX) snprintf(name, sizeof name, "reciter_%d", id - IM_RECITER0);
        else snprintf(name, sizeof name, "%s", words[id - IM_INDEX]);
        snprintf(path, sizeof path, "%s/%s.png", UI_DIR, name);
        int w, h, ch;
        px[id] = stbi_load(path, &w, &h, &ch, 4);
        if (!px[id]) { LOG("missing ui image %s", path); continue; }
        if (w > AW - 2 * PAD) w = AW - 2 * PAD;
        if (x + w + PAD > AW) { x = PAD; y += shelf + PAD; shelf = 0; }
        g_img[id].src = (SDL_Rect){ x, y, w, h };
        g_img[id].w = w; g_img[id].h = h;
        x += w + PAD; if (h > shelf) shelf = h;
    }
    int AH = y + shelf + PAD;
    Uint32 *atlas = (Uint32 *)calloc((size_t)AW * AH, 4);      /* one big block: mapped, not heap */
    for (int id = 0; id < IM_COUNT; id++) {
        if (!px[id]) continue;
        SDL_Rect s = g_img[id].src;
        int stride = 0, ch;
        (void)ch;
        /* original width may be larger than the clipped one */
        stride = s.w;
        for (int row = 0; row < s.h; row++)
            memcpy(atlas + (size_t)(s.y + row) * AW + s.x, px[id] + (size_t)row * stride * 4, (size_t)s.w * 4);
        stbi_image_free(px[id]); px[id] = NULL;
    }
    g_atlas = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, AW, AH);
    if (g_atlas) {
        SDL_SetTextureBlendMode(g_atlas, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_atlas, SDL_ScaleModeBest);
        SDL_UpdateTexture(g_atlas, NULL, atlas, AW * 4);
        for (int id = 0; id < IM_COUNT; id++) if (g_img[id].w) g_img[id].t = g_atlas;
    } else LOG("atlas texture failed: %s", SDL_GetError());
    free(atlas);
    LOG("ui atlas %dx%d", AW, AH);
}

/* draw an image so that its 64-px font renders at `px` (in 1080p units); anchor: -1 left, 0 centre, +1 right */
static int draw_img(SDL_Renderer *r, int id, int x, int cy, float px, int anchor, SDL_Color col) {
    Img *im = &g_img[id];
    if (!im->t) return 0;
    float sc = px * g_u / 64.0f;
    int w = (int)(im->w * sc), h = (int)(im->h * sc);
    SDL_Rect d = { anchor < 0 ? x : anchor > 0 ? x - w : x - w / 2, cy - h / 2, w, h };
    SDL_SetTextureColorMod(im->t, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(im->t, col.a);
    SDL_RenderCopy(r, im->t, &im->src, &d);
    return w;
}

static int number_width(int v, float px) {
    char s[16]; snprintf(s, sizeof s, "%d", v);
    int w = 0;
    for (char *p = s; *p; p++) w += (int)(g_img[IM_DIGIT0 + (*p - '0')].w * px * g_u / 64.0f * 0.92f);
    return w;
}
/* Arabic-Indic digits, written left to right */
static int draw_number(SDL_Renderer *r, int v, int x, int cy, float px, int anchor, SDL_Color col) {
    char s[16]; snprintf(s, sizeof s, "%d", v);
    int total = number_width(v, px);
    int cx = anchor < 0 ? x : anchor > 0 ? x - total : x - total / 2;
    for (char *p = s; *p; p++) {
        int id = IM_DIGIT0 + (*p - '0');
        draw_img(r, id, cx, cy, px, -1, col);
        cx += (int)(g_img[id].w * px * g_u / 64.0f * 0.92f);
    }
    return total;
}

static const SDL_Color WHITE = { 255, 255, 255, 255 }, GOLD = { 226, 192, 112, 255 }, DIM = { 190, 205, 195, 255 };

static void fill(SDL_Renderer *r, SDL_Rect rc, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    SDL_SetRenderDrawColor(r, R, G, B, A); SDL_RenderFillRect(r, &rc);
}
static void frame(SDL_Renderer *r, SDL_Rect rc, int t, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect s[4] = { { rc.x, rc.y, rc.w, t }, { rc.x, rc.y + rc.h - t, rc.w, t },
                      { rc.x, rc.y, t, rc.h }, { rc.x + rc.w - t, rc.y, t, rc.h } };
    SDL_RenderFillRects(r, s, 4);
}

/* ---------------- page drawing ---------------- */
typedef struct { int page; SDL_Rect dst; float sc; int crop_top; } Placed;
/* full-screen layout: trim the thin white strip above/below the page frame (in 1403-px image rows) */
#define CROP_TOP 30
#define CROP_BOTTOM 1372
#define PAPER_R 249
#define PAPER_G 244
#define PAPER_B 217
static Placed g_placed[2];          /* where the visible pages were drawn (for the ayah highlight) */

static void draw_page(SDL_Renderer *r, int n, SDL_Rect area, int align, int slot) {
    Slot *s = get_texture(r, n);
    g_placed[slot].page = 0;
    if (s) {
        int ct = s->h * CROP_TOP / 1403, cb = s->h * CROP_BOTTOM / 1403;
        SDL_Rect src = { 0, ct, s->w, cb - ct };
        float sc = SDL_min((float)area.w / src.w, (float)area.h / src.h);
        SDL_Rect dst = { 0, 0, (int)(src.w * sc + 0.5f), (int)(src.h * sc + 0.5f) };
        dst.x = area.x + (align < 0 ? 0 : align > 0 ? area.w - dst.w : (area.w - dst.w) / 2);
        dst.y = area.y + (area.h - dst.h) / 2;
        SDL_RenderCopy(r, s->tex, &src, &dst);
        g_placed[slot].page = n; g_placed[slot].dst = dst; g_placed[slot].sc = sc; g_placed[slot].crop_top = ct;
        return;
    }
    const float aspect = 974.0f / (CROP_BOTTOM - CROP_TOP);
    int pw = (int)(area.h * aspect), ph = area.h;
    if (pw > area.w) { pw = area.w; ph = (int)(area.w / aspect); }
    SDL_Rect pr = { area.x + (align < 0 ? 0 : align > 0 ? area.w - pw : (area.w - pw) / 2), area.y + (area.h - ph) / 2, pw, ph };
    int st = SDL_AtomicGet(&g_state[n]), u = (int)(g_u * 4);
    if (st == ST_FAILED) {
        SDL_Rect x = { pr.x + pw / 2 - 8 * u, pr.y + ph / 2 - 8 * u, 16 * u, 16 * u };
        fill(r, x, 190, 60, 50, 255);
    } else {
        Uint32 t = SDL_GetTicks() / 120;
        for (int i = 0; i < 8; i++) {
            SDL_Rect d = { pr.x + pw / 2 - 18 * u + i * 5 * u, pr.y + ph / 2 - u, 3 * u, 3 * u };
            fill(r, d, 40, 110, 70, i == (int)(t % 8) ? 255 : 90);
        }
    }
}

/* highlight the ayah being recited on the visible pages */
static void draw_highlight(SDL_Renderer *r, int key) {
    int sura = key / 1000, ayah = key % 1000;
    for (int i = 0; i < 2; i++) {
        Placed *pl = &g_placed[i];
        if (!pl->page) continue;
        const short *f = PAGE_FRAME[pl->page - 1];
        if (!f[0]) continue;
        float xl = f[1], xr = f[2], y0 = f[3] / 10.0f, d = f[4] / 100.0f;
        for (int k = PAGE_SEG_OFF[pl->page - 1]; k < PAGE_SEG_OFF[pl->page]; k++) {
            const unsigned short *sg = PAGE_SEGS[k];
            if (sg[0] != sura || sg[1] != ayah) continue;
            float x0 = xl + sg[3] / 290.0f * (xr - xl), x1 = xl + sg[4] / 290.0f * (xr - xl);
            float cy = y0 + sg[2] * d;
            SDL_Rect rc = { pl->dst.x + (int)(x0 * pl->sc), pl->dst.y + (int)((cy - d / 2 + 2 - pl->crop_top) * pl->sc),
                            (int)((x1 - x0) * pl->sc), (int)((d - 4) * pl->sc) };
            SDL_SetRenderDrawColor(r, 235, 190, 60, 70);
            SDL_RenderFillRect(r, &rc);
            SDL_Rect under = { rc.x, rc.y + rc.h - (int)(3 * g_u), rc.w, (int)(3 * g_u) + 1 };   /* gold underline */
            SDL_SetRenderDrawColor(r, 200, 150, 40, 200);
            SDL_RenderFillRect(r, &under);
        }
    }
}

/* ---------------- UI state ---------------- */
enum { MODE_VIEW = 0, MODE_INDEX, MODE_RECITER };
typedef struct {
    int mode, tab, sel[2], top[2], rsel;
    Uint32 toast_until; int toast_kind;     /* 0 none, 1 playing, 2 paused, 3 error, 4 reciter */
    Uint32 hint_until;
} UI;

static void draw_toast(SDL_Renderer *r, UI *ui, int ww, int wh) {
    Uint32 now = SDL_GetTicks();
    int st = SDL_AtomicGet(&g_au_status), playing = SDL_AtomicGet(&g_playing);
    int kind = now < ui->toast_until ? ui->toast_kind : 0;
    if (!kind && playing && st == AU_LOADING) kind = 5;
    if (!kind) return;
    int H = (int)(70 * g_u), cy = wh - (int)(70 * g_u);
    int rec = SDL_AtomicGet(&g_reciter);
    int w1 = 0, w2 = 0, gap = (int)(24 * g_u);
    int a = IM_PLAYING, b = IM_RECITER0 + rec;
    if (kind == 2) { a = IM_PAUSED; b = -1; }
    if (kind == 3) { a = IM_AUDIO_ERR; b = -1; }
    if (kind == 4) { a = IM_RECITER; }
    if (kind == 5) { a = IM_LOADING; b = -1; }
    float px = 34;
    w1 = (int)(g_img[a].w * px * g_u / 64.0f);
    if (b >= 0) w2 = (int)(g_img[b].w * px * g_u / 64.0f);
    int W = w1 + (b >= 0 ? gap + w2 : 0) + (int)(60 * g_u);
    SDL_Rect box = { ww / 2 - W / 2, cy - H / 2, W, H };
    fill(r, box, 12, 52, 38, 235);
    frame(r, box, (int)(2 * g_u) + 1, GOLD);
    int x = box.x + box.w - (int)(30 * g_u);                      /* right to left */
    draw_img(r, a, x, cy, px, +1, kind == 3 ? (SDL_Color){ 255, 170, 160, 255 } : GOLD);
    if (b >= 0) draw_img(r, b, x - w1 - gap, cy, px, +1, WHITE);
}

static void draw_index(SDL_Renderer *r, UI *ui, int ww, int wh) {
    fill(r, (SDL_Rect){ 0, 0, ww, wh }, 0, 0, 0, 150);
    int W = (int)(780 * g_u), H = (int)(960 * g_u);
    SDL_Rect p = { ww / 2 - W / 2, wh / 2 - H / 2, W, H };
    fill(r, p, 14, 62, 45, 250);
    frame(r, p, (int)(3 * g_u) + 1, GOLD);
    /* tabs: السور (right), الأجزاء (left) */
    int ty = p.y + (int)(60 * g_u);
    SDL_Rect tr = { p.x + W / 2, p.y + (int)(18 * g_u), W / 2 - (int)(18 * g_u), (int)(84 * g_u) };
    SDL_Rect tl = { p.x + (int)(18 * g_u), tr.y, tr.w, tr.h };
    fill(r, ui->tab == 0 ? tr : tl, 226, 192, 112, 60);
    frame(r, ui->tab == 0 ? tr : tl, (int)(2 * g_u) + 1, GOLD);
    draw_img(r, IM_SURAS, tr.x + tr.w / 2, ty, 40, 0, ui->tab == 0 ? GOLD : DIM);
    draw_img(r, IM_JUZ_TAB, tl.x + tl.w / 2, ty, 40, 0, ui->tab == 1 ? GOLD : DIM);

    int count = ui->tab == 0 ? SURA_COUNT : JUZ_COUNT;
    int rowh = (int)(66 * g_u), y0 = p.y + (int)(122 * g_u);
    int rows = (p.y + H - (int)(80 * g_u) - y0) / rowh;
    int *sel = &ui->sel[ui->tab], *top = &ui->top[ui->tab];
    if (*sel < *top) *top = *sel;
    if (*sel >= *top + rows) *top = *sel - rows + 1;
    int xr = p.x + W - (int)(36 * g_u), xl = p.x + (int)(36 * g_u);
    for (int i = 0; i < rows && *top + i < count; i++) {
        int idx = *top + i, cy = y0 + i * rowh + rowh / 2;
        SDL_Rect row = { p.x + (int)(14 * g_u), y0 + i * rowh + 2, W - (int)(28 * g_u), rowh - 4 };
        if (idx == *sel) { fill(r, row, 226, 192, 112, 70); frame(r, row, (int)(2 * g_u) + 1, GOLD); }
        else if (i & 1) fill(r, row, 255, 255, 255, 10);
        SDL_Color c = idx == *sel ? WHITE : (SDL_Color){ 225, 232, 226, 255 };
        int pg;
        if (ui->tab == 0) {
            int nw = draw_number(r, idx + 1, xr, cy, 32, +1, GOLD);
            draw_img(r, IM_SURA0 + idx, xr - nw - (int)(22 * g_u), cy, 36, +1, c);
            pg = SURA_PAGE[idx];
        } else {
            int w = draw_img(r, IM_JUZ, xr, cy, 36, +1, c);
            draw_number(r, idx + 1, xr - w - (int)(14 * g_u), cy, 34, +1, GOLD);
            pg = JUZ_PAGE[idx];
        }
        int nw = draw_number(r, pg, xl, cy, 30, -1, DIM);
        draw_img(r, IM_PAGE, xl + nw + (int)(12 * g_u), cy, 26, -1, DIM);
    }
    /* scroll bar */
    if (count > rows) {
        int bh = (rows * rowh) * rows / count, by = y0 + (rows * rowh - bh) * (*top) / (count - rows);
        fill(r, (SDL_Rect){ p.x + (int)(4 * g_u), by, (int)(5 * g_u), bh }, 226, 192, 112, 160);
    }
    draw_img(r, IM_HINT_INDEX, p.x + W / 2, p.y + H - (int)(40 * g_u), 24, 0, DIM);
}

static void draw_reciters(SDL_Renderer *r, UI *ui, int ww, int wh) {
    fill(r, (SDL_Rect){ 0, 0, ww, wh }, 0, 0, 0, 150);
    int rowh = (int)(76 * g_u);
    int W = (int)(720 * g_u), H = (int)(150 * g_u) + rowh * RECITER_COUNT + (int)(70 * g_u);
    SDL_Rect p = { ww / 2 - W / 2, wh / 2 - H / 2, W, H };
    fill(r, p, 14, 62, 45, 250);
    frame(r, p, (int)(3 * g_u) + 1, GOLD);
    draw_img(r, IM_RECITER, p.x + W / 2, p.y + (int)(64 * g_u), 44, 0, GOLD);
    int y0 = p.y + (int)(120 * g_u), cur = SDL_AtomicGet(&g_reciter);
    for (int i = 0; i < RECITER_COUNT; i++) {
        int cy = y0 + i * rowh + rowh / 2;
        SDL_Rect row = { p.x + (int)(16 * g_u), y0 + i * rowh + 3, W - (int)(32 * g_u), rowh - 6 };
        if (i == ui->rsel) { fill(r, row, 226, 192, 112, 70); frame(r, row, (int)(2 * g_u) + 1, GOLD); }
        draw_img(r, IM_RECITER0 + i, p.x + W - (int)(70 * g_u), cy, 38, +1, i == ui->rsel ? WHITE : DIM);
        if (i == cur) {                                         /* current reciter marker */
            int d = (int)(14 * g_u);
            fill(r, (SDL_Rect){ p.x + W - (int)(46 * g_u), cy - d / 2, d, d }, 226, 192, 112, 255);
        }
    }
    draw_img(r, IM_HINT_RECITER, p.x + W / 2, p.y + H - (int)(42 * g_u), 24, 0, DIM);
}

/* ---------------- input ---------------- */
enum { K_NONE = 0, K_LEFT, K_RIGHT, K_UP, K_DOWN, K_L1, K_R1, K_CROSS, K_CIRCLE, K_SQUARE, K_TRIANGLE, K_OPTIONS, K_TOUCH, K_QUIT };

static int map_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT; case SDLK_UP: return K_UP; case SDLK_DOWN: return K_DOWN;
    case SDLK_PAGEDOWN: return K_L1; case SDLK_PAGEUP: return K_R1;
    case SDLK_RETURN: case SDLK_x: return K_CROSS; case SDLK_BACKSPACE: case SDLK_c: return K_CIRCLE;
    case SDLK_SPACE: case SDLK_s: return K_SQUARE; case SDLK_TAB: case SDLK_t: return K_TRIANGLE;
    case SDLK_o: return K_OPTIONS; case SDLK_h: return K_TOUCH; case SDLK_ESCAPE: return K_QUIT;
    }
    return K_NONE;
}
static int map_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_LEFT; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_RIGHT;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_UP; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_DOWN;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return K_L1; case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return K_R1;
    case SDL_CONTROLLER_BUTTON_A: return K_CROSS; case SDL_CONTROLLER_BUTTON_B: return K_CIRCLE;
    case SDL_CONTROLLER_BUTTON_X: return K_SQUARE; case SDL_CONTROLLER_BUTTON_Y: return K_TRIANGLE;
    case SDL_CONTROLLER_BUTTON_START: return K_OPTIONS;
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return K_TOUCH;
    }
    return K_NONE;
}

/* ---------------- main ---------------- */
int main(int argc, char **argv) {
    const char *env = GETENV("QURAN_CACHE");
    if (env && *env) snprintf(g_cache_dir, sizeof g_cache_dir, "%s", env);
    mkdir_p(g_cache_dir);
    { char lp[600]; snprintf(lp, sizeof lp, "%s/quran.log", g_cache_dir); g_log = fopen(lp, "w"); }
    LOG("start");

    int page = (argc > 1) ? atoi(argv[1]) : read_int_setting("last_page", 1);
    if (page < 1 || page > PAGE_COUNT) page = 1;
    int rec = read_int_setting("reciter", 0);
    if (rec < 0 || rec >= RECITER_COUNT) rec = 0;
    SDL_AtomicSet(&g_reciter, rec);

    for (int n = 1; n <= PAGE_COUNT; n++) {
        char p[600]; page_path(n, p, sizeof p);
        SDL_AtomicSet(&g_state[n], file_exists(p) ? ST_READY : ST_NONE);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        LOG("SDL_Init failed: %s", SDL_GetError());
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return 1;
    }
    g_log_lock = SDL_CreateMutex();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    SDL_Window *win = SDL_CreateWindow("القرآن الكريم", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, WIN_FLAGS);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, REN_FLAGS);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { LOG("renderer failed: %s", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    load_ui(ren);

    SDL_AudioSpec want; SDL_zero(want);
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 2048;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &g_have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (!g_dev) LOG("audio device failed: %s", SDL_GetError());
    else LOG("audio %d Hz, %d ch", g_have.freq, g_have.channels);

    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }

    g_wake = SDL_CreateSemaphore(0);
    g_audio_wake = SDL_CreateSemaphore(0);
    SDL_AtomicSet(&g_want_page, page);
    SDL_Thread *th = SDL_CreateThread(downloader, "pages", NULL);
    SDL_Thread *ath = g_dev ? SDL_CreateThread(reciter_thread, "reciter", NULL) : NULL;


    UI ui; memset(&ui, 0, sizeof ui);
    ui.hint_until = SDL_GetTicks() + 7000;
    SDL_Texture *bg = NULL; int bg_w = 0, bg_h = 0; Uint32 bg_key = 0xFFFFFFFFu, last_key = 0xFFFFFFFFu;
    int running = 1, last_saved = page, opt_was_down = 0;

    /* tests: QURAN_TEST="t d d x" = sequence of keys, QURAN_SHOT=file.bmp */
    const char *test = GETENV("QURAN_TEST"), *shot = GETENV("QURAN_SHOT");
    Uint32 test_next = SDL_GetTicks() + 400;

    while (running) {
        int ww, wh;
        SDL_GetRendererOutputSize(ren, &ww, &wh);
        g_u = wh / 1080.0f;
        int spread = (float)ww / wh >= 1.2f, step = spread ? 2 : 1;
        if (spread && !(page & 1)) page--;
        int user_page = 0;                              /* page changed by the user (not by recitation) */

        int keys[16], nk = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int k = K_NONE;
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN) k = map_key(e.key.keysym.sym);
            else if (e.type == SDL_CONTROLLERBUTTONDOWN) k = map_button(e.cbutton.button);
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            if (k && nk < 16) keys[nk++] = k;
        }
        /* Options can arrive as raw joystick button 4 on the console */
        int opt_down = pad && SDL_JoystickGetButton(SDL_GameControllerGetJoystick(pad), 4);
        if (opt_down && !opt_was_down && nk < 16) keys[nk++] = K_OPTIONS;
        opt_was_down = opt_down;
        if (test && *test && SDL_GetTicks() >= test_next) {
            static const char *map = " lrudLRxcstoh";
            while (*test == ' ') test++;
            const char *f = *test ? strchr(map, *test) : NULL;
            if (f && nk < 16) keys[nk++] = (int)(f - map);
            if (*test) test++;
            test_next = SDL_GetTicks() + 500;
        }

        for (int i = 0; i < nk; i++) {
            int k = keys[i];
            if (k == K_QUIT) { running = 0; break; }
            if (ui.mode == MODE_INDEX) {
                int count = ui.tab == 0 ? SURA_COUNT : JUZ_COUNT, *sel = &ui.sel[ui.tab];
                if (k == K_UP) *sel = (*sel + count - 1) % count;
                else if (k == K_DOWN) *sel = (*sel + 1) % count;
                else if (k == K_R1) *sel = SDL_max(*sel - 10, 0);
                else if (k == K_L1) *sel = SDL_min(*sel + 10, count - 1);
                else if (k == K_LEFT || k == K_RIGHT) ui.tab ^= 1;
                else if (k == K_CIRCLE || k == K_TRIANGLE) ui.mode = MODE_VIEW;
                else if (k == K_CROSS) {
                    page = ui.tab == 0 ? SURA_PAGE[*sel] : JUZ_PAGE[*sel];
                    ui.mode = MODE_VIEW; user_page = 1;
                }
            } else if (ui.mode == MODE_RECITER) {
                if (k == K_UP) ui.rsel = (ui.rsel + RECITER_COUNT - 1) % RECITER_COUNT;
                else if (k == K_DOWN) ui.rsel = (ui.rsel + 1) % RECITER_COUNT;
                else if (k == K_CIRCLE) ui.mode = MODE_VIEW;
                else if (k == K_CROSS) {
                    SDL_AtomicSet(&g_reciter, ui.rsel);
                    write_int_setting("reciter", ui.rsel);
                    ui.mode = MODE_VIEW;
                    ui.toast_kind = 4; ui.toast_until = SDL_GetTicks() + 3000;
                    if (SDL_AtomicGet(&g_playing)) {         /* continue from the ayah being heard */
                        SDL_AtomicSet(&g_play_start, SDL_AtomicGet(&g_audible));
                        SDL_AtomicAdd(&g_play_gen, 1);
                    }
                }
            } else {
                if (k == K_LEFT || k == K_L1) { page += step; user_page = 1; }
                else if (k == K_RIGHT || k == K_R1) { page -= step; user_page = 1; }
                else if (k == K_DOWN) { page += 20; user_page = 1; }
                else if (k == K_UP) { page -= 20; user_page = 1; }
                else if (k == K_TRIANGLE) {
                    ui.mode = MODE_INDEX;
                    ui.sel[0] = sura_of_page(page) - 1;
                    ui.sel[1] = juz_of_page(page);
                } else if (k == K_CIRCLE) { ui.mode = MODE_RECITER; ui.rsel = SDL_AtomicGet(&g_reciter); }
                else if (k == K_OPTIONS || k == K_TOUCH) ui.hint_until = SDL_GetTicks() + 7000;
                else if (k == K_CROSS) {
                    for (int n = 1; n <= PAGE_COUNT; n++)
                        if (SDL_AtomicGet(&g_state[n]) == ST_FAILED) SDL_AtomicSet(&g_state[n], ST_NONE);
                    SDL_SemPost(g_wake);
                } else if (k == K_SQUARE && g_dev) {
                    if (SDL_AtomicGet(&g_playing)) {
                        int paused = SDL_GetAudioDeviceStatus(g_dev) == SDL_AUDIO_PAUSED;
                        SDL_PauseAudioDevice(g_dev, !paused);
                        ui.toast_kind = paused ? 1 : 2; ui.toast_until = SDL_GetTicks() + (paused ? 3000 : 600000);
                    } else {                                    /* start from the first ayah on screen */
                        SDL_AtomicSet(&g_play_start, ayah_key(PAGE_FIRST[page - 1][0], PAGE_FIRST[page - 1][1]));
                        SDL_AtomicAdd(&g_play_gen, 1);
                        SDL_AtomicSet(&g_playing, 1);
                        SDL_SemPost(g_audio_wake);
                        ui.toast_kind = 1; ui.toast_until = SDL_GetTicks() + 3000;
                    }
                }
            }
        }
        if (page < 1) page = 1;
        if (page > PAGE_COUNT) page = PAGE_COUNT;
        if (spread && !(page & 1)) page--;

        int playing = SDL_AtomicGet(&g_playing);
        if (playing && user_page) {                     /* the user moved: recite from the new page */
            SDL_AtomicSet(&g_play_start, ayah_key(PAGE_FIRST[page - 1][0], PAGE_FIRST[page - 1][1]));
            SDL_AtomicAdd(&g_play_gen, 1);
            SDL_PauseAudioDevice(g_dev, 0);
        } else if (playing && SDL_GetAudioDeviceStatus(g_dev) == SDL_AUDIO_PLAYING) {   /* follow the recitation */
            int k = SDL_AtomicGet(&g_audible), ap = page_of(k / 1000, k % 1000);
            if (ap != page && !(spread && ap == page + 1)) {
                page = spread ? (ap & 1 ? ap : ap - 1) : ap;
                LOG("follow recitation -> page %d", page);
            }
        }
        if (SDL_AtomicGet(&g_au_status) == AU_ERROR && ui.toast_kind != 3) {
            ui.toast_kind = 3; ui.toast_until = SDL_GetTicks() + 5000;
            SDL_AtomicSet(&g_au_status, AU_IDLE);
        }
        if (SDL_AtomicGet(&g_want_page) != page) { SDL_AtomicSet(&g_want_page, page); SDL_SemPost(g_wake); }

        /* ---- render (only when something changed) ---- */
        Slot *t1 = get_texture(ren, page);
        Slot *t2 = (spread && page < PAGE_COUNT) ? get_texture(ren, page + 1) : NULL;
        int pages_ready = t1 && (!spread || page == PAGE_COUNT || t2);
        Uint32 now = SDL_GetTicks();
        Uint32 bkey = (Uint32)page * 2654435761u ^ (Uint32)(ww * 31 + wh) ^ (Uint32)(spread << 30)
                    ^ (t1 ? 0x1000u : 0) ^ (t2 ? 0x2000u : 0)
                    ^ (Uint32)(SDL_AtomicGet(&g_state[page]) << 16)
                    ^ (Uint32)((spread && page < PAGE_COUNT ? SDL_AtomicGet(&g_state[page + 1]) : 0) << 18)
                    ^ (pages_ready ? 0 : (now / 120) << 20);
        int toast_on = now < ui.toast_until || (playing && SDL_AtomicGet(&g_au_status) == AU_LOADING);
        Uint32 key = bkey ^ (Uint32)(ui.mode * 7919 + ui.tab * 104729 + ui.sel[0] * 31 + ui.sel[1] * 1031 + ui.rsel * 65537)
                   ^ (Uint32)(toast_on ? 0x40000000u ^ (Uint32)ui.toast_kind * 3u ^ (Uint32)SDL_AtomicGet(&g_reciter) * 977u : 0)
                   ^ (now < ui.hint_until ? 0x20000000u : 0) ^ (Uint32)(SDL_AtomicGet(&g_au_status) * 7777)
                   ^ (playing ? (Uint32)SDL_AtomicGet(&g_audible) * 2246822519u : 0);
        int shot_ready = shot && pages_ready && (!test || !*test) && now > test_next + 1500;
        if (key == last_key && !shot_ready) { SDL_Delay(16); continue; }
        last_key = key;

        if (!bg || bg_w != ww || bg_h != wh) {
            if (bg) SDL_DestroyTexture(bg);
            bg = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, ww, wh);
            bg_w = ww; bg_h = wh; bg_key = 0xFFFFFFFFu;
        }
        if (bkey != bg_key) {                           /* redraw the pages only when they change */
            if (bg) SDL_SetRenderTarget(ren, bg);
            /* the page paper colour fills the whole screen, so the pages have no dark borders */
            SDL_SetRenderDrawColor(ren, PAPER_R, PAPER_G, PAPER_B, 255);
            SDL_RenderClear(ren);
            if (spread) {
                draw_page(ren, page, (SDL_Rect){ ww / 2, 0, ww - ww / 2, wh }, -1, 0);
                g_placed[1].page = 0;
                if (page + 1 <= PAGE_COUNT) draw_page(ren, page + 1, (SDL_Rect){ 0, 0, ww / 2, wh }, +1, 1);
                int sw = (int)(2 * g_u) + 1;                       /* spine */
                fill(ren, (SDL_Rect){ ww / 2 - sw / 2, 0, sw, wh }, 196, 186, 156, 255);
                fill(ren, (SDL_Rect){ ww / 2 - sw / 2 - (int)(6 * g_u), 0, (int)(6 * g_u), wh }, 120, 100, 60, 18);
                fill(ren, (SDL_Rect){ ww / 2 + sw - sw / 2, 0, (int)(6 * g_u), wh }, 120, 100, 60, 18);
            } else { draw_page(ren, page, (SDL_Rect){ 0, 0, ww, wh }, 0, 0); g_placed[1].page = 0; }
            if (bg) SDL_SetRenderTarget(ren, NULL);
            bg_key = bkey;
        }
        if (bg) SDL_RenderCopy(ren, bg, NULL, NULL);
        if (playing) draw_highlight(ren, SDL_AtomicGet(&g_audible));
        if (ui.mode == MODE_INDEX) draw_index(ren, &ui, ww, wh);
        else if (ui.mode == MODE_RECITER) draw_reciters(ren, &ui, ww, wh);
        else if (now < ui.hint_until) {
            int H = (int)(64 * g_u), W = (int)(g_img[IM_HINT_MAIN].w * 28 * g_u / 64.0f) + (int)(56 * g_u);
            SDL_Rect box = { ww / 2 - W / 2, wh - (int)(40 * g_u) - H, W, H };
            fill(ren, box, 12, 52, 38, 225);
            frame(ren, box, (int)(2 * g_u) + 1, GOLD);
            draw_img(ren, IM_HINT_MAIN, ww / 2, box.y + H / 2, 28, 0, WHITE);
        }
        if (ui.mode == MODE_VIEW && !(now < ui.hint_until)) draw_toast(ren, &ui, ww, wh);

        if (shot_ready) {
            SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ww, wh, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
            SDL_SaveBMP(sf, shot);
            SDL_FreeSurface(sf);
            running = 0;
        }
        SDL_RenderPresent(ren);
        if (page != last_saved) { write_int_setting("last_page", page); last_saved = page; }
    }

    write_int_setting("last_page", page);
    SDL_AtomicSet(&g_quit, 1);
    SDL_SemPost(g_wake); SDL_SemPost(g_audio_wake);
    SDL_WaitThread(th, NULL);
    if (ath) SDL_WaitThread(ath, NULL);

    if (g_dev) SDL_CloseAudioDevice(g_dev);
    for (int i = 0; i < TEX_SLOTS; i++) if (g_slots[i].tex) SDL_DestroyTexture(g_slots[i].tex);
    if (bg) SDL_DestroyTexture(bg);
    if (g_atlas) SDL_DestroyTexture(g_atlas);
    if (pad) SDL_GameControllerClose(pad);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    curl_global_cleanup();
    SDL_Quit();
    LOG("exit");
    return 0;
}
