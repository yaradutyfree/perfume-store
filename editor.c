/*
 * Perfume Store Product Editor
 * Build: make
 * Requires: SDL2, SDL2_ttf, SDL2_image
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

/* ── Paths ────────────────────────────────────────────────────────── */
#define HTML_PATH "/home/xmm/Projects/perfume-store/index.html"
#define IMG_DIR   "/home/xmm/Projects/perfume-store/images"
#define GIT_DIR   "/home/xmm/Projects/perfume-store"
#define FONT_PATH "/usr/share/fonts/truetype/lato/Lato-Regular.ttf"

/* ── Window layout ────────────────────────────────────────────────── */
#define WIN_W    900
#define WIN_H    660
#define LIST_W   230   /* left panel */
#define BTN_BAR  52    /* bottom button bar */
#define STAT_H   26    /* status bar */
#define PAD      14
#define TF_H     34

/* ── Limits ───────────────────────────────────────────────────────── */
#define MAX_PRODS 20
#define BUF_MAX   (1 << 20)  /* 1 MB */

/* ── Colors ───────────────────────────────────────────────────────── */
static const SDL_Color
    C_BG    = {14,  14,  14,  255},
    C_PANEL = {20,  20,  20,  255},
    C_CARD  = {26,  26,  26,  255},
    C_BDR   = {44,  44,  44,  255},
    C_GOLD  = {201, 168, 76,  255},
    C_LGOLD = {230, 200, 120, 255},
    C_TEXT  = {240, 234, 214, 255},
    C_MUTE  = {110, 110, 110, 255},
    C_SEL   = {38,  30,  8,   255},
    C_HOV   = {26,  22,  6,   255},
    C_GRN   = {37,  175, 85,  255},
    C_RED   = {165, 50,  50,  255},
    C_PURP  = {75,  100, 200, 255},
    C_DARK2 = {14,  14,  14,  255};

/* ── Data ─────────────────────────────────────────────────────────── */
typedef struct {
    int  id;
    char name[64];
    char desc[128];
    char price[16];   /* numeric string: "45" or "45.50" */
    char size[16];    /* "50ml" */
    char badge[32];   /* "New" / "Best Seller" / "" */
    char icon[16];    /* emoji fallback */
    char image[256];  /* "images/foo.jpg" or "" */
} Prod;

/* ── Text field ───────────────────────────────────────────────────── */
typedef struct {
    char     buf[256];
    int      len, cursor, active;
    SDL_Rect rect;
    char     label[32];
    char     hint[64];
} TF;

/* ── Globals ──────────────────────────────────────────────────────── */
static SDL_Window   *gwin  = NULL;
static SDL_Renderer *gren  = NULL;
static TTF_Font     *gfnt  = NULL;   /* 14px */
static TTF_Font     *gfsm  = NULL;   /* 11px */
static TTF_Font     *gflg  = NULL;   /* 18px */

static Prod  prods[MAX_PRODS];
static int   np    = 0;
static int   sel   = 0;
static int   nxtid = 100;

enum { FN=0, FD, FP, FS, FB, NFIELDS };
static TF   tfs[NFIELDS];
static int  atf   = -1;

static SDL_Texture *imgtex = NULL;
static int          imgw, imgh;

/* file-path modal */
static int modal = 0;
static TF  mtf;
static SDL_Rect mrect;

static char   stmsg[256] = "Ready";
static Uint32 sttime     = 0;

static SDL_Rect btn_add, btn_del, btn_save, btn_push, btn_browse;
static int hov_item = -1;
static int dirty    = 0;

/* ═══════════════════════════════════════════════════════════════════
   Drawing helpers
   ═══════════════════════════════════════════════════════════════════ */
static void scol(SDL_Color c) {
    SDL_SetRenderDrawColor(gren, c.r, c.g, c.b, c.a);
}

static void frect(SDL_Rect r, SDL_Color c) { scol(c); SDL_RenderFillRect(gren, &r); }
static void drect(SDL_Rect r, SDL_Color c) { scol(c); SDL_RenderDrawRect(gren, &r); }

static void hline(int x1, int x2, int y, SDL_Color c) {
    scol(c);
    SDL_RenderDrawLine(gren, x1, y, x2, y);
}

static SDL_Texture *make_tex(const char *txt, TTF_Font *f, SDL_Color c, int *ow, int *oh) {
    if (!txt || !txt[0]) return NULL;
    SDL_Surface *s = TTF_RenderUTF8_Blended(f, txt, c);
    if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(gren, s);
    if (ow) *ow = s->w;
    if (oh) *oh = s->h;
    SDL_FreeSurface(s);
    return t;
}

static void dtext(const char *txt, int x, int y, TTF_Font *f, SDL_Color c) {
    int w, h;
    SDL_Texture *t = make_tex(txt, f, c, &w, &h);
    if (!t) return;
    SDL_Rect d = {x, y, w, h};
    SDL_RenderCopy(gren, t, NULL, &d);
    SDL_DestroyTexture(t);
}

static void dtext_clip(const char *txt, SDL_Rect clip, TTF_Font *f, SDL_Color c) {
    int w, h;
    SDL_Texture *t = make_tex(txt, f, c, &w, &h);
    if (!t) return;
    SDL_Rect src = {0, 0, w, h};
    SDL_Rect dst = {clip.x, clip.y + (clip.h - h) / 2, w, h};
    if (dst.x + dst.w > clip.x + clip.w) {
        int overflow = (dst.x + dst.w) - (clip.x + clip.w);
        src.w -= overflow;
        dst.w  = src.w;
    }
    SDL_RenderSetClipRect(gren, &clip);
    SDL_RenderCopy(gren, t, &src, &dst);
    SDL_RenderSetClipRect(gren, NULL);
    SDL_DestroyTexture(t);
}

static void dtext_center(const char *txt, SDL_Rect r, TTF_Font *f, SDL_Color c) {
    int w, h;
    SDL_Texture *t = make_tex(txt, f, c, &w, &h);
    if (!t) return;
    SDL_Rect d = {r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
    SDL_RenderCopy(gren, t, NULL, &d);
    SDL_DestroyTexture(t);
}

static void set_status(const char *msg) {
    strncpy(stmsg, msg, sizeof(stmsg) - 1);
    sttime = SDL_GetTicks();
}

/* ═══════════════════════════════════════════════════════════════════
   HTML parsing
   ═══════════════════════════════════════════════════════════════════ */
static int extract_field(const char *blk, const char *key, char *out, int outsz) {
    const char *p = strstr(blk, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < outsz - 1) out[i++] = *p++;
        out[i] = '\0';
        return 1;
    }
    /* unquoted (numbers, null) */
    if (strncmp(p, "null", 4) == 0) { out[0] = '\0'; return 1; }
    int i = 0;
    while (*p && *p != ',' && *p != '\n' && *p != '\r' && i < outsz - 1)
        out[i++] = *p++;
    out[i] = '\0';
    /* trim whitespace */
    while (i > 0 && isspace((unsigned char)out[i-1])) out[--i] = '\0';
    return 1;
}

static void load_html(void) {
    FILE *f = fopen(HTML_PATH, "r");
    if (!f) { set_status("ERROR: Cannot open index.html"); return; }
    char *buf = malloc(BUF_MAX);
    if (!buf) { fclose(f); return; }
    int sz = (int)fread(buf, 1, BUF_MAX - 1, f);
    buf[sz] = '\0';
    fclose(f);

    np    = 0;
    nxtid = 1;

    const char *marker = "const products = [";
    char *arr = strstr(buf, marker);
    if (!arr) { set_status("ERROR: products array not found"); free(buf); return; }
    arr += strlen(marker);

    /* find end of array */
    char *arr_end = strstr(arr, "\n  ];");
    if (!arr_end) arr_end = strstr(arr, "\n  ]");
    if (!arr_end) { free(buf); return; }

    char *p = arr;
    while (p < arr_end && np < MAX_PRODS) {
        char *ob = strchr(p, '{');
        if (!ob || ob >= arr_end) break;

        /* find matching } */
        int depth = 1;
        char *cb = ob + 1;
        while (*cb && depth) {
            if (*cb == '{') depth++;
            else if (*cb == '}') depth--;
            cb++;
        }

        int blen = (int)(cb - ob);
        char *blk = malloc(blen + 1);
        memcpy(blk, ob, blen);
        blk[blen] = '\0';

        Prod *pr = &prods[np];
        memset(pr, 0, sizeof(Prod));

        char tmp[32];
        if (extract_field(blk, "id",    tmp,      sizeof(tmp)))     pr->id = atoi(tmp);
        extract_field(blk, "name",  pr->name,  sizeof(pr->name));
        extract_field(blk, "desc",  pr->desc,  sizeof(pr->desc));
        extract_field(blk, "price", pr->price, sizeof(pr->price));
        extract_field(blk, "size",  pr->size,  sizeof(pr->size));
        extract_field(blk, "badge", pr->badge, sizeof(pr->badge));
        extract_field(blk, "icon",  pr->icon,  sizeof(pr->icon));
        extract_field(blk, "image", pr->image, sizeof(pr->image));

        if (strcmp(pr->badge, "null") == 0) pr->badge[0] = '\0';
        if (strcmp(pr->image, "null") == 0) pr->image[0] = '\0';

        if (pr->id >= nxtid) nxtid = pr->id + 1;
        if (pr->name[0]) np++;

        free(blk);
        p = cb;
    }
    free(buf);

    char msg[64];
    snprintf(msg, sizeof(msg), "Loaded %d products from index.html", np);
    set_status(msg);
    dirty = 0;
}

/* ═══════════════════════════════════════════════════════════════════
   HTML saving
   ═══════════════════════════════════════════════════════════════════ */

/* fix render template to support image field */
static void fix_render_template(char *buf) {
    const char *old_tpl = "<span>${p.icon}</span>";
    const char *new_tpl =
        "${p.image ? `<img src=\"${p.image}\" alt=\"${p.name}\" />` : `<span>${p.icon}</span>`}";
    char *pos = strstr(buf, old_tpl);
    if (!pos) return;  /* already updated */

    int old_len = (int)strlen(old_tpl);
    int new_len = (int)strlen(new_tpl);
    int rest    = (int)strlen(pos + old_len);

    memmove(pos + new_len, pos + old_len, rest + 1);
    memcpy(pos, new_tpl, new_len);
}

static void save_html(void) {
    /* read full file */
    FILE *f = fopen(HTML_PATH, "r");
    if (!f) { set_status("ERROR: Cannot open index.html"); return; }
    char *buf = malloc(BUF_MAX * 2);
    if (!buf) { fclose(f); return; }
    int sz = (int)fread(buf, 1, BUF_MAX - 1, f);
    buf[sz] = '\0';
    fclose(f);

    /* update render template if needed */
    fix_render_template(buf);

    /* locate products array boundaries */
    const char *marker = "const products = [";
    char *arr_start = strstr(buf, marker);
    if (!arr_start) { set_status("ERROR: products marker not found"); free(buf); return; }
    arr_start += strlen(marker);

    char *arr_end = strstr(arr_start, "\n  ];");
    if (!arr_end) { set_status("ERROR: products end not found"); free(buf); return; }

    /* build new products JS */
    char *newjs = malloc(BUF_MAX);
    if (!newjs) { free(buf); return; }
    int pos = 0;
    pos += snprintf(newjs + pos, BUF_MAX - pos, "\n");

    for (int i = 0; i < np; i++) {
        Prod *pr = &prods[i];
        char badge_js[64], image_js[300];

        if (pr->badge[0])
            snprintf(badge_js, sizeof(badge_js), "\"%s\"", pr->badge);
        else
            strcpy(badge_js, "null");

        if (pr->image[0])
            snprintf(image_js, sizeof(image_js), "\"%s\"", pr->image);
        else
            strcpy(image_js, "null");

        pos += snprintf(newjs + pos, BUF_MAX - pos,
            "    {\n"
            "      id: %d,\n"
            "      name: \"%s\",\n"
            "      desc: \"%s\",\n"
            "      price: %s,\n"
            "      size: \"%s\",\n"
            "      icon: \"%s\",\n"
            "      badge: %s,\n"
            "      image: %s\n"
            "    }%s\n",
            pr->id,
            pr->name,
            pr->desc,
            pr->price[0] ? pr->price : "0",
            pr->size,
            pr->icon[0] ? pr->icon : "🌺",
            badge_js,
            image_js,
            i < np - 1 ? "," : ""
        );
    }
    newjs[pos] = '\0';

    /* splice: before + newjs + from arr_end onward */
    int before_len  = (int)(arr_start - buf);
    int after_start = (int)(arr_end   - buf);
    int after_len   = sz - after_start;
    int newjs_len   = pos;

    char *out = malloc(before_len + newjs_len + after_len + 4);
    if (!out) { free(buf); free(newjs); return; }
    memcpy(out, buf, before_len);
    memcpy(out + before_len, newjs, newjs_len);
    memcpy(out + before_len + newjs_len, arr_end, after_len);
    out[before_len + newjs_len + after_len] = '\0';

    /* write back */
    f = fopen(HTML_PATH, "w");
    if (!f) {
        set_status("ERROR: Cannot write index.html");
        free(out); free(buf); free(newjs);
        return;
    }
    fputs(out, f);
    fclose(f);

    free(out); free(buf); free(newjs);
    dirty = 0;
    set_status("Saved! Click 'Push to GitHub' to update the website.");
}

/* ═══════════════════════════════════════════════════════════════════
   Image helpers
   ═══════════════════════════════════════════════════════════════════ */
static void free_img(void) {
    if (imgtex) { SDL_DestroyTexture(imgtex); imgtex = NULL; }
}

static void load_preview(const char *path) {
    free_img();
    if (!path || !path[0]) return;

    /* try absolute path first, then relative to GIT_DIR */
    SDL_Surface *s = IMG_Load(path);
    if (!s) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", GIT_DIR, path);
        s = IMG_Load(full);
    }
    if (!s) return;

    imgw   = s->w;
    imgh   = s->h;
    imgtex = SDL_CreateTextureFromSurface(gren, s);
    SDL_FreeSurface(s);
}

/* copy src file to IMG_DIR/basename, set dest to relative path */
static int copy_image(const char *src, char *dest_rel, int dest_sz) {
    /* ensure images dir exists */
    mkdir(IMG_DIR, 0755);

    /* get basename */
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;

    /* build dest path */
    char dest_abs[512];
    snprintf(dest_abs, sizeof(dest_abs), "%s/%s", IMG_DIR, base);

    /* copy */
    FILE *fin  = fopen(src, "rb");
    if (!fin)  return 0;
    FILE *fout = fopen(dest_abs, "wb");
    if (!fout) { fclose(fin); return 0; }

    char cbuf[4096];
    size_t n;
    while ((n = fread(cbuf, 1, sizeof(cbuf), fin)) > 0)
        fwrite(cbuf, 1, n, fout);

    fclose(fin);
    fclose(fout);

    snprintf(dest_rel, dest_sz, "images/%s", base);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
   Product <-> form sync
   ═══════════════════════════════════════════════════════════════════ */
static void tf_set(TF *t, const char *s) {
    strncpy(t->buf, s ? s : "", sizeof(t->buf) - 1);
    t->len    = (int)strlen(t->buf);
    t->cursor = t->len;
}

static void fields_from_prod(int i) {
    if (i < 0 || i >= np) return;
    Prod *p = &prods[i];
    tf_set(&tfs[FN], p->name);
    tf_set(&tfs[FD], p->desc);
    tf_set(&tfs[FP], p->price);
    tf_set(&tfs[FS], p->size);
    tf_set(&tfs[FB], p->badge);
    load_preview(p->image);
}

static void fields_to_prod(int i) {
    if (i < 0 || i >= np) return;
    Prod *p = &prods[i];
    strncpy(p->name,  tfs[FN].buf, sizeof(p->name)  - 1);
    strncpy(p->desc,  tfs[FD].buf, sizeof(p->desc)  - 1);
    strncpy(p->price, tfs[FP].buf, sizeof(p->price) - 1);
    strncpy(p->size,  tfs[FS].buf, sizeof(p->size)  - 1);
    strncpy(p->badge, tfs[FB].buf, sizeof(p->badge) - 1);
    dirty = 1;
}

/* ═══════════════════════════════════════════════════════════════════
   Layout — compute all rects
   ═══════════════════════════════════════════════════════════════════ */
static void setup_layout(void) {
    int ry  = WIN_H - BTN_BAR - STAT_H;  /* right panel top */
    int rx  = LIST_W + 1;
    int rw  = WIN_W - rx;

    /* === left panel buttons === */
    int bw = LIST_W / 2 - PAD - 4;
    btn_add = (SDL_Rect){ PAD,            ry + (BTN_BAR - 34) / 2, bw, 34 };
    btn_del = (SDL_Rect){ PAD + bw + 8,  ry + (BTN_BAR - 34) / 2, bw, 34 };

    /* === right panel buttons === */
    int pw = 200;
    btn_save = (SDL_Rect){ WIN_W - PAD - pw*2 - 12, ry + (BTN_BAR-34)/2, pw, 34 };
    btn_push = (SDL_Rect){ WIN_W - PAD - pw,         ry + (BTN_BAR-34)/2, pw, 34 };

    /* === text fields === */
    int fx  = rx + PAD;
    int fw  = rw - PAD * 2;
    int fy  = 50;
    int gap = TF_H + 24;

    /* label row height = 14 + 6 gap + TF_H */
    tfs[FN].rect = (SDL_Rect){ fx, fy,          fw,       TF_H };  strncpy(tfs[FN].label, "Perfume Name",    31); strncpy(tfs[FN].hint, "e.g. Rose Oud",  63);
    tfs[FD].rect = (SDL_Rect){ fx, fy+gap,      fw,       TF_H };  strncpy(tfs[FD].label, "Description",     31); strncpy(tfs[FD].hint, "Short tagline",  63);
    tfs[FP].rect = (SDL_Rect){ fx, fy+gap*2,    fw/3,     TF_H };  strncpy(tfs[FP].label, "Price ($)",       31); strncpy(tfs[FP].hint, "45",             63);
    tfs[FS].rect = (SDL_Rect){ fx + fw/3 + 12,  fy+gap*2, fw/3-12, TF_H }; strncpy(tfs[FS].label, "Size",     31); strncpy(tfs[FS].hint, "50ml",           63);
    tfs[FB].rect = (SDL_Rect){ fx + fw*2/3 + 8, fy+gap*2, fw/3-8,  TF_H }; strncpy(tfs[FB].label, "Badge",   31); strncpy(tfs[FB].hint, "New / Best Seller",63);
    tfs[FN].rect.y += 10;  /* title row down a bit */

    /* image browse button */
    int img_y = fy + gap * 3;
    btn_browse = (SDL_Rect){ fx, img_y, 160, 34 };

    /* modal rect (centered) */
    mrect = (SDL_Rect){ WIN_W/2 - 280, WIN_H/2 - 80, 560, 160 };
    mtf.rect = (SDL_Rect){ mrect.x + PAD, mrect.y + 60, mrect.w - PAD*2, TF_H };
    strncpy(mtf.label, "Image file path", 31);
    strncpy(mtf.hint,  "/home/user/photo.jpg", 63);
}

/* ═══════════════════════════════════════════════════════════════════
   Text field rendering
   ═══════════════════════════════════════════════════════════════════ */
static void draw_tf(TF *t, int is_active) {
    SDL_Color bdr = is_active ? C_GOLD : C_BDR;

    frect(t->rect, C_CARD);
    drect(t->rect, bdr);

    SDL_Rect clip = { t->rect.x + 8, t->rect.y, t->rect.w - 16, t->rect.h };
    if (t->len > 0) {
        dtext_clip(t->buf, clip, gfnt, C_TEXT);
    } else {
        dtext_clip(t->hint, clip, gfnt, C_MUTE);
    }

    /* cursor */
    if (is_active) {
        Uint32 now = SDL_GetTicks();
        if ((now / 530) % 2 == 0) {
            /* measure text up to cursor */
            int cx = t->rect.x + 8;
            if (t->cursor > 0) {
                char tmp[256];
                int n = t->cursor < t->len ? t->cursor : t->len;
                memcpy(tmp, t->buf, n);
                tmp[n] = '\0';
                int tw, th;
                TTF_SizeUTF8(gfnt, tmp, &tw, &th);
                cx += tw;
            }
            int cy1 = t->rect.y + 5;
            int cy2 = t->rect.y + t->rect.h - 5;
            scol(C_GOLD);
            SDL_RenderDrawLine(gren, cx, cy1, cx, cy2);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
   Text field input
   ═══════════════════════════════════════════════════════════════════ */
static void tf_activate(TF *t, int idx) {
    if (atf >= 0 && atf < NFIELDS) tfs[atf].active = 0;
    if (idx >= 0 && idx < NFIELDS) {
        tfs[idx].active = 1;
        /* select-all: move cursor to end so user can type or backspace */
        tfs[idx].cursor = tfs[idx].len;
    }
    atf = idx;
    SDL_StartTextInput();
}

static void tf_select_all(TF *t) {
    /* Ctrl+A: clear buffer for quick replace */
    t->buf[0] = '\0';
    t->len    = 0;
    t->cursor = 0;
}

static void tf_activate_modal(void) {
    mtf.active = 1;
    modal      = 1;
    mtf.len    = mtf.cursor = 0;
    mtf.buf[0] = '\0';
    SDL_StartTextInput();
}

static void tf_deactivate(void) {
    if (atf >= 0 && atf < NFIELDS) tfs[atf].active = 0;
    mtf.active = 0;
    atf = -1;
    SDL_StopTextInput();
}

static void tf_insert(TF *t, const char *s) {
    int slen = (int)strlen(s);
    if (t->len + slen >= (int)sizeof(t->buf)) return;
    memmove(t->buf + t->cursor + slen, t->buf + t->cursor, t->len - t->cursor + 1);
    memcpy(t->buf + t->cursor, s, slen);
    t->len    += slen;
    t->cursor += slen;
    dirty = 1;
}

static void tf_backspace(TF *t) {
    if (t->cursor <= 0) return;
    memmove(t->buf + t->cursor - 1, t->buf + t->cursor, t->len - t->cursor + 1);
    t->len--;
    t->cursor--;
    dirty = 1;
}

/* ═══════════════════════════════════════════════════════════════════
   Rendering
   ═══════════════════════════════════════════════════════════════════ */
static void draw_button(SDL_Rect r, const char *label, SDL_Color bg, SDL_Color fg) {
    frect(r, bg);
    drect(r, C_BDR);
    dtext_center(label, r, gfnt, fg);
}

static void render(void) {
    scol(C_BG);
    SDL_RenderClear(gren);

    int bary  = WIN_H - BTN_BAR - STAT_H;
    int right = LIST_W + 1;

    /* ── LEFT PANEL ── */
    frect((SDL_Rect){0, 0, LIST_W, bary}, C_PANEL);

    /* header */
    frect((SDL_Rect){0, 0, LIST_W, 40}, C_CARD);
    dtext("Products", PAD, 12, gflg, C_GOLD);
    hline(0, LIST_W, 40, C_BDR);

    /* product list */
    int item_h = 46;
    for (int i = 0; i < np; i++) {
        SDL_Rect ir = {0, 40 + i * item_h, LIST_W, item_h};
        SDL_Color bg = (i == sel)      ? C_SEL :
                       (i == hov_item) ? C_HOV : C_PANEL;
        frect(ir, bg);
        hline(0, LIST_W, ir.y + item_h - 1, C_BDR);

        /* gold left accent for selected */
        if (i == sel) frect((SDL_Rect){0, ir.y, 3, item_h}, C_GOLD);

        SDL_Rect clip = {PAD + 4, ir.y + 8, LIST_W - PAD*2, 18};
        SDL_Color nc  = (i == sel) ? C_LGOLD : C_TEXT;
        dtext_clip(prods[i].name[0] ? prods[i].name : "(unnamed)", clip, gfnt, nc);

        /* price */
        char ptxt[24];
        snprintf(ptxt, sizeof(ptxt), "$%s / %s", prods[i].price, prods[i].size);
        SDL_Rect pc = {PAD + 4, ir.y + 28, LIST_W - PAD*2, 14};
        dtext_clip(ptxt, pc, gfsm, C_MUTE);
    }

    /* add/del buttons */
    frect((SDL_Rect){0, bary, LIST_W, BTN_BAR}, C_PANEL);
    hline(0, LIST_W, bary, C_BDR);
    draw_button(btn_add, "+ Add",   (SDL_Color){30,50,20,255}, C_GRN);
    draw_button(btn_del, "Delete",  (SDL_Color){40,15,15,255}, C_RED);

    /* ── DIVIDER ── */
    frect((SDL_Rect){LIST_W, 0, 1, WIN_H}, C_BDR);

    /* ── RIGHT PANEL header ── */
    frect((SDL_Rect){right, 0, WIN_W - right, 40}, C_CARD);
    hline(right, WIN_W, 40, C_BDR);

    if (np == 0) {
        SDL_Rect center = {right, 40, WIN_W - right, bary - 40};
        dtext_center("No products loaded — click + Add", center, gfnt, C_MUTE);
    } else {
        char htitle[80];
        snprintf(htitle, sizeof(htitle), "Editing: %s",
                 prods[sel].name[0] ? prods[sel].name : "(new)");
        dtext(htitle, right + PAD, 12, gflg, C_TEXT);

        /* labels above each field */
        for (int i = 0; i < NFIELDS; i++) {
            TF *t = &tfs[i];
            dtext(t->label, t->rect.x, t->rect.y - 18, gfsm, C_MUTE);
            draw_tf(t, atf == i);
        }

        /* browse button */
        dtext("Product Image", btn_browse.x, btn_browse.y - 18, gfsm, C_MUTE);
        draw_button(btn_browse, "Browse / Set Image", C_PURP, C_TEXT);

        /* image preview */
        if (imgtex) {
            /* compute preview rect (max 180x180) */
            int pw = 180, ph = 180;
            float ar = (float)imgw / imgh;
            if (ar > 1.0f) ph = (int)(pw / ar);
            else           pw = (int)(ph * ar);

            SDL_Rect pr = {
                btn_browse.x + btn_browse.w + 16,
                btn_browse.y,
                pw, ph
            };
            SDL_RenderCopy(gren, imgtex, NULL, &pr);
            drect(pr, C_BDR);

            /* show current image path */
            SDL_Rect pclip = {
                btn_browse.x,
                btn_browse.y + btn_browse.h + 6,
                WIN_W - btn_browse.x - PAD,
                14
            };
            dtext_clip(prods[sel].image, pclip, gfsm, C_MUTE);
        } else if (prods[sel].image[0]) {
            dtext(prods[sel].image, btn_browse.x, btn_browse.y + btn_browse.h + 6, gfsm, C_MUTE);
        } else {
            dtext("No image set — using emoji icon", btn_browse.x,
                  btn_browse.y + btn_browse.h + 6, gfsm, C_MUTE);
        }
    }

    /* ── BOTTOM BAR ── */
    frect((SDL_Rect){right, bary, WIN_W - right, BTN_BAR}, C_CARD);
    hline(right, WIN_W, bary, C_BDR);
    draw_button(btn_save, "Save & Update HTML", C_GRN, (SDL_Color){10,10,10,255});
    draw_button(btn_push, "Push to GitHub",     (SDL_Color){30,50,30,255}, C_GRN);

    /* dirty indicator */
    if (dirty) {
        dtext("*", btn_save.x - 14, btn_save.y + 9, gfnt, C_GOLD);
    }

    /* ── STATUS BAR ── */
    int sy = WIN_H - STAT_H;
    frect((SDL_Rect){0, sy, WIN_W, STAT_H}, C_DARK2);
    hline(0, WIN_W, sy, C_BDR);
    dtext(stmsg, PAD, sy + 6, gfsm, C_MUTE);

    /* ── MODAL ── */
    if (modal) {
        /* overlay */
        SDL_SetRenderDrawBlendMode(gren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gren, 0, 0, 0, 180);
        SDL_RenderFillRect(gren, NULL);
        SDL_SetRenderDrawBlendMode(gren, SDL_BLENDMODE_NONE);

        /* dialog box */
        frect(mrect, C_CARD);
        drect(mrect, C_GOLD);
        dtext("Enter full path to image file:", mrect.x + PAD, mrect.y + PAD, gfnt, C_TEXT);
        dtext("(e.g. /home/user/perfume.jpg)", mrect.x + PAD, mrect.y + PAD + 20, gfsm, C_MUTE);
        draw_tf(&mtf, 1);
        draw_button(
            (SDL_Rect){mrect.x + mrect.w - 100 - PAD, mrect.y + mrect.h - 44, 100, 32},
            "OK", C_GRN, (SDL_Color){10,10,10,255});
        draw_button(
            (SDL_Rect){mrect.x + PAD, mrect.y + mrect.h - 44, 80, 32},
            "Cancel", C_RED, C_TEXT);
    }

    SDL_RenderPresent(gren);
}

/* ═══════════════════════════════════════════════════════════════════
   Modal confirm
   ═══════════════════════════════════════════════════════════════════ */
static void modal_confirm(void) {
    if (!mtf.len) { modal = 0; SDL_StopTextInput(); return; }

    char dest_rel[256] = {0};
    if (!copy_image(mtf.buf, dest_rel, sizeof(dest_rel))) {
        set_status("ERROR: Could not copy image — check the path");
        modal = 0;
        SDL_StopTextInput();
        return;
    }

    strncpy(prods[sel].image, dest_rel, sizeof(prods[sel].image) - 1);
    load_preview(dest_rel);
    dirty  = 1;
    modal  = 0;
    SDL_StopTextInput();
    set_status("Image set! Remember to Save & Update HTML.");
}

/* ═══════════════════════════════════════════════════════════════════
   Git push
   ═══════════════════════════════════════════════════════════════════ */
static void git_push(void) {
    set_status("Pushing to GitHub...");
    render();

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "cd %s && git add -A && git commit -m 'Update products' && git push 2>&1",
        GIT_DIR);

    FILE *fp = popen(cmd, "r");
    if (!fp) { set_status("ERROR: popen failed"); return; }

    char line[256];
    char last[256] = {0};
    while (fgets(line, sizeof(line), fp))
        strncpy(last, line, sizeof(last) - 1);
    pclose(fp);

    /* trim newline */
    int ll = (int)strlen(last);
    while (ll > 0 && (last[ll-1] == '\n' || last[ll-1] == '\r')) last[--ll] = '\0';

    if (last[0]) set_status(last);
    else         set_status("Pushed successfully! Website updated.");
}

/* ═══════════════════════════════════════════════════════════════════
   Event handling
   ═══════════════════════════════════════════════════════════════════ */
static int pt_in(SDL_Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void handle_event(SDL_Event *e) {
    if (e->type == SDL_QUIT) { SDL_Quit(); exit(0); }

    /* ── MOUSE MOTION ── */
    if (e->type == SDL_MOUSEMOTION) {
        int mx = e->motion.x, my = e->motion.y;
        hov_item = -1;
        int item_h = 46;
        for (int i = 0; i < np; i++) {
            SDL_Rect ir = {0, 40 + i * item_h, LIST_W, item_h};
            if (pt_in(ir, mx, my)) { hov_item = i; break; }
        }
        return;
    }

    /* ── MOUSE CLICK ── */
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        int mx = e->button.x, my = e->button.y;

        /* modal buttons */
        if (modal) {
            SDL_Rect ok_btn = {mrect.x + mrect.w - 100 - PAD, mrect.y + mrect.h - 44, 100, 32};
            SDL_Rect cn_btn = {mrect.x + PAD, mrect.y + mrect.h - 44, 80, 32};
            if (pt_in(ok_btn, mx, my)) { modal_confirm(); return; }
            if (pt_in(cn_btn, mx, my)) { modal = 0; SDL_StopTextInput(); return; }
            if (pt_in(mtf.rect, mx, my)) { mtf.cursor = mtf.len; return; }
            return;
        }

        /* product list */
        int item_h = 46;
        for (int i = 0; i < np; i++) {
            SDL_Rect ir = {0, 40 + i * item_h, LIST_W, item_h};
            if (pt_in(ir, mx, my)) {
                fields_to_prod(sel);
                sel = i;
                fields_from_prod(sel);
                tf_deactivate();
                return;
            }
        }

        /* add product */
        if (pt_in(btn_add, mx, my) && np < MAX_PRODS) {
            fields_to_prod(sel);
            Prod *p = &prods[np];
            memset(p, 0, sizeof(Prod));
            p->id = nxtid++;
            strcpy(p->name,  "New Perfume");
            strcpy(p->price, "0");
            strcpy(p->size,  "50ml");
            strcpy(p->icon,  "🌺");
            sel = np++;
            fields_from_prod(sel);
            dirty = 1;
            return;
        }

        /* delete product */
        if (pt_in(btn_del, mx, my) && np > 0) {
            fields_to_prod(sel);
            memmove(&prods[sel], &prods[sel+1], (np - sel - 1) * sizeof(Prod));
            np--;
            if (sel >= np && sel > 0) sel--;
            fields_from_prod(sel);
            free_img();
            dirty = 1;
            return;
        }

        /* text fields */
        for (int i = 0; i < NFIELDS; i++) {
            if (pt_in(tfs[i].rect, mx, my)) {
                fields_to_prod(sel);
                tf_activate(tfs, i);
                return;
            }
        }

        /* browse image */
        if (pt_in(btn_browse, mx, my)) {
            fields_to_prod(sel);
            tf_deactivate();
            tf_activate_modal();
            return;
        }

        /* save */
        if (pt_in(btn_save, mx, my)) {
            fields_to_prod(sel);
            save_html();
            return;
        }

        /* push */
        if (pt_in(btn_push, mx, my)) {
            fields_to_prod(sel);
            save_html();
            git_push();
            return;
        }

        /* click elsewhere = deactivate */
        tf_deactivate();
        return;
    }

    /* ── KEY DOWN ── */
    if (e->type == SDL_KEYDOWN) {
        SDL_Keycode k = e->key.keysym.sym;

        if (modal) {
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { modal_confirm(); return; }
            if (k == SDLK_ESCAPE)  { modal = 0; SDL_StopTextInput(); return; }
            if (k == SDLK_BACKSPACE) tf_backspace(&mtf);
            return;
        }

        if (atf < 0) return;
        TF *t = &tfs[atf];

        if (k == SDLK_BACKSPACE) { tf_backspace(t); fields_to_prod(sel); }
        else if (k == SDLK_LEFT  && t->cursor > 0)    t->cursor--;
        else if (k == SDLK_RIGHT && t->cursor < t->len) t->cursor++;
        else if (k == SDLK_HOME) t->cursor = 0;
        else if (k == SDLK_END)  t->cursor = t->len;
        else if (k == SDLK_a && (e->key.keysym.mod & KMOD_CTRL)) {
            tf_select_all(t); dirty = 1;
        }
        else if (k == SDLK_TAB) {
            fields_to_prod(sel);
            int next = (atf + 1) % NFIELDS;
            tf_activate(tfs, next);
        }
        else if (k == SDLK_RETURN) tf_deactivate();
        else if (k == SDLK_ESCAPE) tf_deactivate();
        /* Ctrl+S = save */
        else if (k == SDLK_s && (e->key.keysym.mod & KMOD_CTRL)) {
            fields_to_prod(sel);
            save_html();
        }
        return;
    }

    /* ── TEXT INPUT ── */
    if (e->type == SDL_TEXTINPUT) {
        if (modal) { tf_insert(&mtf, e->text.text); return; }
        if (atf >= 0) { tf_insert(&tfs[atf], e->text.text); fields_to_prod(sel); }
        return;
    }
}

/* ═══════════════════════════════════════════════════════════════════
   main
   ═══════════════════════════════════════════════════════════════════ */
int main(void) {
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);

    gwin = SDL_CreateWindow(
        "Perfume Store — Product Editor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    gren = SDL_CreateRenderer(gwin, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(gren, SDL_BLENDMODE_BLEND);

    gfnt = TTF_OpenFont(FONT_PATH, 14);
    gfsm = TTF_OpenFont(FONT_PATH, 11);
    gflg = TTF_OpenFont(FONT_PATH, 17);

    if (!gfnt || !gfsm || !gflg) {
        fprintf(stderr, "TTF_OpenFont failed: %s\n", TTF_GetError());
        return 1;
    }

    setup_layout();
    load_html();
    if (np > 0) fields_from_prod(0);

    SDL_Event e;
    while (1) {
        while (SDL_PollEvent(&e)) handle_event(&e);
        render();
        SDL_Delay(16);
    }

    TTF_CloseFont(gfnt);
    TTF_CloseFont(gfsm);
    TTF_CloseFont(gflg);
    TTF_Quit();
    IMG_Quit();
    SDL_DestroyRenderer(gren);
    SDL_DestroyWindow(gwin);
    SDL_Quit();
    return 0;
}
