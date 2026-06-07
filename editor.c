/*
 * Perfume Store Product Editor
 * Build: make
 * Features: file browser, clipboard paste, webp/jpg/gif/png support
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

/* ── Paths ────────────────────────────────────────────────────────── */
#define HTML_PATH "/home/xmm/Projects/perfume-store/index.html"
#define IMG_DIR   "/home/xmm/Projects/perfume-store/images"
#define GIT_DIR   "/home/xmm/Projects/perfume-store"
#define FONT_PATH "/usr/share/fonts/truetype/lato/Lato-Regular.ttf"

/* ── Window layout ────────────────────────────────────────────────── */
#define WIN_W    900
#define WIN_H    660
#define LIST_W   230
#define BTN_BAR  52
#define STAT_H   26
#define PAD      14
#define TF_H     34

/* ── Limits ───────────────────────────────────────────────────────── */
#define MAX_PRODS   20
#define BUF_MAX     (1 << 20)
#define FB_MAX      1024
#define FB_ITEM_H   28

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
    C_DARK2 = {14,  14,  14,  255},
    C_FBHOV = {32,  38,  50,  255},
    C_FBSEL = {40,  60,  90,  255};

/* ── Product ──────────────────────────────────────────────────────── */
typedef struct {
    int  id;
    char name[64];
    char desc[128];
    char price[16];
    char size[16];
    char badge[32];
    char icon[16];
    char image[256];
} Prod;

/* ── Text field ───────────────────────────────────────────────────── */
typedef struct {
    char     buf[256];
    int      len, cursor, active;
    SDL_Rect rect;
    char     label[32];
    char     hint[64];
} TF;

/* ── File browser entry ───────────────────────────────────────────── */
typedef struct {
    char name[256];
    char fullpath[512];
    int  is_dir;
} FBEntry;

/* ── File browser state ───────────────────────────────────────────── */
static struct {
    int      open;
    char     path[512];
    FBEntry  entries[FB_MAX];
    int      count;
    int      scroll;
    int      hovered;
    int      selected;
    SDL_Rect rect;       /* full dialog rect */
    SDL_Rect list_rect;  /* scrollable list area */
} fb;

/* ── Globals ──────────────────────────────────────────────────────── */
static SDL_Window   *gwin  = NULL;
static SDL_Renderer *gren  = NULL;
static TTF_Font     *gfnt  = NULL;
static TTF_Font     *gfsm  = NULL;
static TTF_Font     *gflg  = NULL;

static Prod  prods[MAX_PRODS];
static int   np    = 0;
static int   sel   = 0;
static int   nxtid = 100;

enum { FN=0, FD, FP, FS, FB_F, NFIELDS };
static TF   tfs[NFIELDS];
static int  atf = -1;

static SDL_Texture *imgtex = NULL;
static int          imgw, imgh;

static char   stmsg[256] = "Ready";
static Uint32 sttime     = 0;

static SDL_Rect btn_add, btn_del, btn_save, btn_push, btn_browse, btn_paste;
static int hov_item = -1;
static int dirty    = 0;

/* ═══════════════════════════════════════════════════════════════════
   Drawing helpers
   ═══════════════════════════════════════════════════════════════════ */
static void scol(SDL_Color c) { SDL_SetRenderDrawColor(gren, c.r, c.g, c.b, c.a); }
static void frect(SDL_Rect r, SDL_Color c) { scol(c); SDL_RenderFillRect(gren, &r); }
static void drect(SDL_Rect r, SDL_Color c) { scol(c); SDL_RenderDrawRect(gren, &r); }
static void hline(int x1, int x2, int y, SDL_Color c) {
    scol(c); SDL_RenderDrawLine(gren, x1, y, x2, y);
}

static SDL_Texture *make_tex(const char *t, TTF_Font *f, SDL_Color c, int *ow, int *oh) {
    if (!t || !t[0]) return NULL;
    SDL_Surface *s = TTF_RenderUTF8_Blended(f, t, c);
    if (!s) return NULL;
    SDL_Texture *tx = SDL_CreateTextureFromSurface(gren, s);
    if (ow) *ow = s->w;
    if (oh) *oh = s->h;
    SDL_FreeSurface(s);
    return tx;
}

static void dtext(const char *t, int x, int y, TTF_Font *f, SDL_Color c) {
    int w, h;
    SDL_Texture *tx = make_tex(t, f, c, &w, &h);
    if (!tx) return;
    SDL_Rect d = {x, y, w, h};
    SDL_RenderCopy(gren, tx, NULL, &d);
    SDL_DestroyTexture(tx);
}

static void dtext_clip(const char *t, SDL_Rect clip, TTF_Font *f, SDL_Color c) {
    int w, h;
    SDL_Texture *tx = make_tex(t, f, c, &w, &h);
    if (!tx) return;
    SDL_Rect src = {0, 0, w, h};
    SDL_Rect dst = {clip.x, clip.y + (clip.h - h) / 2, w, h};
    if (dst.x + dst.w > clip.x + clip.w) {
        src.w = clip.x + clip.w - dst.x;
        dst.w = src.w;
    }
    if (src.w <= 0) { SDL_DestroyTexture(tx); return; }
    SDL_RenderSetClipRect(gren, &clip);
    SDL_RenderCopy(gren, tx, &src, &dst);
    SDL_RenderSetClipRect(gren, NULL);
    SDL_DestroyTexture(tx);
}

static void dtext_center(const char *t, SDL_Rect r, TTF_Font *f, SDL_Color c) {
    int w, h;
    SDL_Texture *tx = make_tex(t, f, c, &w, &h);
    if (!tx) return;
    SDL_Rect d = {r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
    SDL_RenderCopy(gren, tx, NULL, &d);
    SDL_DestroyTexture(tx);
}

static void set_status(const char *msg) {
    strncpy(stmsg, msg, sizeof(stmsg) - 1);
    stmsg[sizeof(stmsg)-1] = '\0';
    sttime = SDL_GetTicks();
}

/* ═══════════════════════════════════════════════════════════════════
   File browser
   ═══════════════════════════════════════════════════════════════════ */
static int is_image_ext(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return 0;
    ext++;
    return strcasecmp(ext, "jpg")  == 0 ||
           strcasecmp(ext, "jpeg") == 0 ||
           strcasecmp(ext, "png")  == 0 ||
           strcasecmp(ext, "gif")  == 0 ||
           strcasecmp(ext, "webp") == 0 ||
           strcasecmp(ext, "bmp")  == 0 ||
           strcasecmp(ext, "tga")  == 0;
}

static int fb_cmp(const void *a, const void *b) {
    const FBEntry *ea = (const FBEntry *)a;
    const FBEntry *eb = (const FBEntry *)b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    return strcasecmp(ea->name, eb->name);
}

static void fb_scan(const char *path) {
    fb.count  = 0;
    fb.scroll = 0;

    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) && fb.count < FB_MAX) {
        if (strcmp(ent->d_name, ".") == 0) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        int is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !is_image_ext(ent->d_name)) continue;
        /* skip hidden files but allow ".." */
        if (ent->d_name[0] == '.' && strcmp(ent->d_name, "..") != 0) continue;

        FBEntry *e = &fb.entries[fb.count++];
        strncpy(e->name,     ent->d_name, sizeof(e->name) - 1);
        strncpy(e->fullpath, full,        sizeof(e->fullpath) - 1);
        e->is_dir = is_dir;
    }
    closedir(d);

    qsort(fb.entries, fb.count, sizeof(FBEntry), fb_cmp);
    fb.hovered  = -1;
    fb.selected = -1;
}

static void fb_navigate(const char *path) {
    strncpy(fb.path, path, sizeof(fb.path) - 1);
    fb.path[sizeof(fb.path)-1] = '\0';
    fb_scan(path);
}

static void fb_open(void) {
    fb.open = 1;
    const char *home = getenv("HOME");
    fb_navigate(home ? home : "/home");

    /* layout */
    int fw = 700, fh = 480;
    fb.rect      = (SDL_Rect){ (WIN_W-fw)/2, (WIN_H-fh)/2, fw, fh };
    fb.list_rect = (SDL_Rect){
        fb.rect.x + 1,
        fb.rect.y + 78,
        fw - 2,
        fh - 78 - 52
    };
}

/* visible item count */
static int fb_vis(void) { return fb.list_rect.h / FB_ITEM_H; }

static void fb_draw(void) {
    /* dim background */
    SDL_SetRenderDrawBlendMode(gren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gren, 0, 0, 0, 170);
    SDL_RenderFillRect(gren, NULL);
    SDL_SetRenderDrawBlendMode(gren, SDL_BLENDMODE_NONE);

    SDL_Rect r = fb.rect;

    /* dialog background */
    frect(r, (SDL_Color){18, 18, 18, 255});
    drect(r, C_GOLD);

    /* title bar */
    SDL_Rect title_r = {r.x, r.y, r.w, 38};
    frect(title_r, (SDL_Color){28, 28, 28, 255});
    hline(r.x, r.x + r.w, r.y + 38, C_BDR);
    dtext("Select Image", r.x + PAD, r.y + 10, gflg, C_GOLD);

    /* close button */
    SDL_Rect close_btn = {r.x + r.w - 36, r.y + 4, 30, 30};
    frect(close_btn, C_RED);
    dtext_center("X", close_btn, gfnt, C_TEXT);

    /* path bar */
    SDL_Rect path_r = {r.x, r.y + 38, r.w, 40};
    frect(path_r, (SDL_Color){22, 22, 22, 255});
    hline(r.x, r.x + r.w, r.y + 78, C_BDR);

    /* up button */
    SDL_Rect up_btn  = {r.x + r.w - 80, r.y + 46, 34, 24};
    SDL_Rect hom_btn = {r.x + r.w - 42, r.y + 46, 34, 24};
    frect(up_btn,  C_CARD); drect(up_btn,  C_BDR);
    frect(hom_btn, C_CARD); drect(hom_btn, C_BDR);
    dtext_center("↑",  up_btn,  gfnt, C_TEXT);
    dtext_center("⌂",  hom_btn, gfnt, C_GOLD);

    /* current path (clipped) */
    SDL_Rect path_clip = {r.x + PAD, r.y + 46, r.w - 100, 24};
    dtext_clip(fb.path, path_clip, gfsm, C_MUTE);

    /* ── list ── */
    SDL_RenderSetClipRect(gren, &fb.list_rect);

    int vis   = fb_vis();
    int start = fb.scroll;
    int end   = start + vis;
    if (end > fb.count) end = fb.count;

    for (int i = start; i < end; i++) {
        FBEntry *e = &fb.entries[i];
        int iy = fb.list_rect.y + (i - start) * FB_ITEM_H;
        SDL_Rect ir = {fb.list_rect.x, iy, fb.list_rect.w, FB_ITEM_H};

        SDL_Color bg = (i == fb.selected) ? C_FBSEL :
                       (i == fb.hovered)  ? C_FBHOV : C_BG;
        frect(ir, bg);
        hline(ir.x, ir.x + ir.w, iy + FB_ITEM_H - 1, C_BDR);

        /* icon: colored slash for dir, dot for image */
        SDL_Color nc = e->is_dir ? C_GOLD : C_TEXT;
        const char *icon = e->is_dir ? "/" : "-";
        dtext(icon, ir.x + 10, iy + (FB_ITEM_H - 14) / 2, gfnt,
              e->is_dir ? C_GOLD : C_MUTE);

        /* name — append "/" to dirs for clarity */
        char display[258];
        if (e->is_dir) snprintf(display, sizeof(display), "%s/", e->name);
        else           strncpy(display, e->name, sizeof(display) - 1);
        SDL_Rect nc_clip = {ir.x + 26, iy, ir.w - 30, FB_ITEM_H};
        dtext_clip(display, nc_clip, gfnt, nc);
    }
    SDL_RenderSetClipRect(gren, NULL);

    /* scrollbar */
    if (fb.count > vis) {
        int sb_x  = r.x + r.w - 8;
        int sb_h  = fb.list_rect.h;
        int sb_y  = fb.list_rect.y;
        float pct = (float)fb.scroll / (fb.count - vis);
        int   th  = sb_h * vis / fb.count;
        int   ty  = sb_y + (int)(pct * (sb_h - th));
        SDL_Rect sb_bg  = {sb_x, sb_y,  6, sb_h};
        SDL_Rect sb_thr = {sb_x, ty,    6, th  };
        frect(sb_bg,  C_CARD);
        frect(sb_thr, C_MUTE);
    }

    /* separator */
    hline(r.x, r.x + r.w, r.y + r.h - 52, C_BDR);

    /* bottom buttons */
    int bbw = 160, bbh = 34;
    int bby = r.y + r.h - 43;

    /* paste clipboard */
    SDL_Rect paste_btn = {r.x + PAD, bby, bbw, bbh};
    frect(paste_btn, C_PURP); drect(paste_btn, C_BDR);
    dtext_center("Paste Clipboard", paste_btn, gfsm, C_TEXT);

    /* open / cancel */
    SDL_Rect open_btn   = {r.x + r.w - PAD - bbw,       bby, bbw, bbh};
    SDL_Rect cancel_btn = {r.x + r.w - PAD - bbw*2 - 8, bby, bbw, bbh};
    SDL_Color open_col  = fb.selected >= 0 ? C_GRN
                                           : (SDL_Color){40,60,40,255};
    frect(open_btn,   open_col);  drect(open_btn,   C_BDR);
    frect(cancel_btn, C_RED);     drect(cancel_btn, C_BDR);
    dtext_center("Open",   open_btn,   gfnt, C_TEXT);
    dtext_center("Cancel", cancel_btn, gfnt, C_TEXT);

    /* hint when nothing selected */
    if (fb.selected < 0) {
        dtext("Click an image file to select it",
              r.x + PAD + bbw + 12, bby + 10, gfsm, C_MUTE);
    } else {
        SDL_Rect hint = {r.x + PAD + bbw + 12, bby, r.w - PAD*3 - bbw*2 - 20, bbh};
        dtext_clip(fb.entries[fb.selected].name, hint, gfsm, C_GOLD);
    }
}

/* ═══════════════════════════════════════════════════════════════════
   Clipboard paste
   ═══════════════════════════════════════════════════════════════════ */
static void free_img(void);
static void load_preview(const char *path);
static int  copy_image(const char *src, char *dest_rel, int dest_sz);

static void try_paste_clipboard(void) {
    /* 1) text clipboard → check if it's a valid image file path */
    char *clip = SDL_GetClipboardText();
    if (clip && clip[0]) {
        /* trim whitespace/newlines */
        char path[512];
        strncpy(path, clip, sizeof(path) - 1);
        path[sizeof(path)-1] = '\0';
        int l = (int)strlen(path);
        while (l > 0 && (path[l-1] == '\n' || path[l-1] == '\r' || path[l-1] == ' '))
            path[--l] = '\0';

        if (access(path, F_OK) == 0 && is_image_ext(path)) {
            char dest_rel[256];
            if (copy_image(path, dest_rel, sizeof(dest_rel))) {
                strncpy(prods[sel].image, dest_rel, sizeof(prods[sel].image) - 1);
                load_preview(dest_rel);
                dirty = 1;
                fb.open = 0;
                SDL_free(clip);
                set_status("Image set from clipboard path!");
                return;
            }
        }
        SDL_free(clip);
    }

    /* 2) image data from X clipboard via xclip (png) */
    const char *tmp_png = "/tmp/_cb_img.png";
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "xclip -selection clipboard -t image/png -o > %s 2>/dev/null", tmp_png);
    if (system(cmd) == 0 && access(tmp_png, F_OK) == 0) {
        struct stat st;
        stat(tmp_png, &st);
        if (st.st_size > 0) {
            char dest_rel[256];
            if (copy_image(tmp_png, dest_rel, sizeof(dest_rel))) {
                strncpy(prods[sel].image, dest_rel, sizeof(prods[sel].image) - 1);
                load_preview(dest_rel);
                dirty = 1;
                fb.open = 0;
                set_status("Image pasted from clipboard!");
                return;
            }
        }
    }

    /* 3) try jpeg */
    const char *tmp_jpg = "/tmp/_cb_img.jpg";
    snprintf(cmd, sizeof(cmd),
        "xclip -selection clipboard -t image/jpeg -o > %s 2>/dev/null", tmp_jpg);
    if (system(cmd) == 0 && access(tmp_jpg, F_OK) == 0) {
        struct stat st;
        stat(tmp_jpg, &st);
        if (st.st_size > 0) {
            char dest_rel[256];
            if (copy_image(tmp_jpg, dest_rel, sizeof(dest_rel))) {
                strncpy(prods[sel].image, dest_rel, sizeof(prods[sel].image) - 1);
                load_preview(dest_rel);
                dirty = 1;
                fb.open = 0;
                set_status("Image pasted from clipboard!");
                return;
            }
        }
    }

    set_status("Clipboard: no image found. Copy an image or a file path first.");
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
    if (strncmp(p, "null", 4) == 0) { out[0] = '\0'; return 1; }
    int i = 0;
    while (*p && *p != ',' && *p != '\n' && *p != '\r' && i < outsz - 1)
        out[i++] = *p++;
    out[i] = '\0';
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

    np = 0; nxtid = 1;

    char *arr = strstr(buf, "const products = [");
    if (!arr) { set_status("ERROR: products array not found"); free(buf); return; }
    arr += strlen("const products = [");

    char *arr_end = strstr(arr, "\n  ];");
    if (!arr_end) arr_end = strstr(arr, "\n  ]");
    if (!arr_end) { free(buf); return; }

    char *p = arr;
    while (p < arr_end && np < MAX_PRODS) {
        char *ob = strchr(p, '{');
        if (!ob || ob >= arr_end) break;

        int depth = 1; char *cb = ob + 1;
        while (*cb && depth) { if (*cb=='{') depth++; else if(*cb=='}') depth--; cb++; }

        int blen = (int)(cb - ob);
        char *blk = malloc(blen + 1);
        memcpy(blk, ob, blen); blk[blen] = '\0';

        Prod *pr = &prods[np];
        memset(pr, 0, sizeof(Prod));

        char tmp[32];
        if (extract_field(blk, "id", tmp, sizeof(tmp))) pr->id = atoi(tmp);
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
static void fix_render_template(char *buf) {
    const char *old = "<span>${p.icon}</span>";
    const char *nw  =
        "${p.image ? `<img src=\"${p.image}\" alt=\"${p.name}\" />` : `<span>${p.icon}</span>`}";
    char *pos = strstr(buf, old);
    if (!pos) return;
    int ol = (int)strlen(old), nl = (int)strlen(nw);
    memmove(pos + nl, pos + ol, strlen(pos + ol) + 1);
    memcpy(pos, nw, nl);
}

static void save_html(void) {
    FILE *f = fopen(HTML_PATH, "r");
    if (!f) { set_status("ERROR: Cannot open index.html"); return; }
    char *buf = malloc(BUF_MAX * 2);
    if (!buf) { fclose(f); return; }
    int sz = (int)fread(buf, 1, BUF_MAX - 1, f);
    buf[sz] = '\0';
    fclose(f);

    fix_render_template(buf);

    char *arr_start = strstr(buf, "const products = [");
    if (!arr_start) { set_status("ERROR: products marker not found"); free(buf); return; }
    arr_start += strlen("const products = [");

    char *arr_end = strstr(arr_start, "\n  ];");
    if (!arr_end) { set_status("ERROR: products end not found"); free(buf); return; }

    char *newjs = malloc(BUF_MAX);
    if (!newjs) { free(buf); return; }
    int pos = 0;
    pos += snprintf(newjs + pos, BUF_MAX - pos, "\n");

    for (int i = 0; i < np; i++) {
        Prod *pr = &prods[i];
        char badge_js[64], image_js[300];

        if (pr->badge[0]) snprintf(badge_js, sizeof(badge_js), "\"%s\"", pr->badge);
        else strcpy(badge_js, "null");

        if (pr->image[0]) snprintf(image_js, sizeof(image_js), "\"%s\"", pr->image);
        else strcpy(image_js, "null");

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
            pr->id, pr->name, pr->desc,
            pr->price[0] ? pr->price : "0",
            pr->size, pr->icon[0] ? pr->icon : "🌺",
            badge_js, image_js,
            i < np - 1 ? "," : "");
    }

    int blen  = (int)(arr_start - buf);
    int astart = (int)(arr_end  - buf);
    int alen   = sz - astart;

    char *out = malloc(blen + pos + alen + 4);
    if (!out) { free(buf); free(newjs); return; }
    memcpy(out, buf, blen);
    memcpy(out + blen, newjs, pos);
    memcpy(out + blen + pos, arr_end, alen);
    out[blen + pos + alen] = '\0';

    f = fopen(HTML_PATH, "w");
    if (!f) { set_status("ERROR: Cannot write index.html"); free(out); free(buf); free(newjs); return; }
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
    SDL_Surface *s = IMG_Load(path);
    if (!s) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", GIT_DIR, path);
        s = IMG_Load(full);
    }
    if (!s) return;
    imgw = s->w; imgh = s->h;
    imgtex = SDL_CreateTextureFromSurface(gren, s);
    SDL_FreeSurface(s);
}

static int copy_image(const char *src, char *dest_rel, int dest_sz) {
    mkdir(IMG_DIR, 0755);
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;

    char dest_abs[512];
    snprintf(dest_abs, sizeof(dest_abs), "%s/%s", IMG_DIR, base);

    FILE *fin  = fopen(src,      "rb");
    if (!fin)  return 0;
    FILE *fout = fopen(dest_abs, "wb");
    if (!fout) { fclose(fin); return 0; }

    char cbuf[8192]; size_t n;
    while ((n = fread(cbuf, 1, sizeof(cbuf), fin)) > 0) fwrite(cbuf, 1, n, fout);
    fclose(fin); fclose(fout);

    snprintf(dest_rel, dest_sz, "images/%s", base);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
   Product <-> form sync
   ═══════════════════════════════════════════════════════════════════ */
static void tf_set(TF *t, const char *s) {
    strncpy(t->buf, s ? s : "", sizeof(t->buf) - 1);
    t->buf[sizeof(t->buf)-1] = '\0';
    t->len = (int)strlen(t->buf);
    t->cursor = t->len;
}

static void fields_from_prod(int i) {
    if (i < 0 || i >= np) return;
    Prod *p = &prods[i];
    tf_set(&tfs[FN], p->name);
    tf_set(&tfs[FD], p->desc);
    tf_set(&tfs[FP], p->price);
    tf_set(&tfs[FS], p->size);
    tf_set(&tfs[FB_F], p->badge);
    load_preview(p->image);
}

static void fields_to_prod(int i) {
    if (i < 0 || i >= np) return;
    Prod *p = &prods[i];
    strncpy(p->name,  tfs[FN].buf,   sizeof(p->name)  - 1);
    strncpy(p->desc,  tfs[FD].buf,   sizeof(p->desc)  - 1);
    strncpy(p->price, tfs[FP].buf,   sizeof(p->price) - 1);
    strncpy(p->size,  tfs[FS].buf,   sizeof(p->size)  - 1);
    strncpy(p->badge, tfs[FB_F].buf, sizeof(p->badge) - 1);
    dirty = 1;
}

/* ═══════════════════════════════════════════════════════════════════
   Layout
   ═══════════════════════════════════════════════════════════════════ */
static void setup_layout(void) {
    int ry  = WIN_H - BTN_BAR - STAT_H;
    int rx  = LIST_W + 1;
    int rw  = WIN_W - rx;

    int bw  = LIST_W / 2 - PAD - 4;
    btn_add = (SDL_Rect){ PAD,           ry + (BTN_BAR-34)/2, bw, 34 };
    btn_del = (SDL_Rect){ PAD + bw + 8,  ry + (BTN_BAR-34)/2, bw, 34 };

    int pw  = 200;
    btn_save = (SDL_Rect){ WIN_W - PAD - pw*2 - 12, ry + (BTN_BAR-34)/2, pw, 34 };
    btn_push = (SDL_Rect){ WIN_W - PAD - pw,         ry + (BTN_BAR-34)/2, pw, 34 };

    int fx = rx + PAD, fw = rw - PAD * 2;
    int fy = 60, gap = TF_H + 24;

    tfs[FN].rect = (SDL_Rect){ fx,             fy,        fw,         TF_H };
    tfs[FD].rect = (SDL_Rect){ fx,             fy+gap,    fw,         TF_H };
    tfs[FP].rect = (SDL_Rect){ fx,             fy+gap*2,  fw/3,       TF_H };
    tfs[FS].rect = (SDL_Rect){ fx+fw/3+12,     fy+gap*2,  fw/3-12,    TF_H };
    tfs[FB_F].rect=(SDL_Rect){ fx+fw*2/3+8,    fy+gap*2,  fw/3-8,     TF_H };

    strncpy(tfs[FN].label,  "Perfume Name", 31);
    strncpy(tfs[FD].label,  "Description",  31);
    strncpy(tfs[FP].label,  "Price ($)",    31);
    strncpy(tfs[FS].label,  "Size",         31);
    strncpy(tfs[FB_F].label,"Badge",        31);

    strncpy(tfs[FN].hint,   "e.g. Rose Oud",        63);
    strncpy(tfs[FD].hint,   "Short tagline",         63);
    strncpy(tfs[FP].hint,   "45",                    63);
    strncpy(tfs[FS].hint,   "50ml",                  63);
    strncpy(tfs[FB_F].hint, "New / Best Seller",     63);

    int img_y = fy + gap * 3 + 4;
    btn_browse = (SDL_Rect){ fx,       img_y, 170, 34 };
    btn_paste  = (SDL_Rect){ fx + 178, img_y, 170, 34 };
}

/* ═══════════════════════════════════════════════════════════════════
   Text field rendering
   ═══════════════════════════════════════════════════════════════════ */
static void draw_tf(TF *t, int is_active) {
    frect(t->rect, C_CARD);
    drect(t->rect, is_active ? C_GOLD : C_BDR);

    SDL_Rect clip = { t->rect.x + 8, t->rect.y, t->rect.w - 16, t->rect.h };
    if (t->len > 0) dtext_clip(t->buf,  clip, gfnt, C_TEXT);
    else            dtext_clip(t->hint, clip, gfnt, C_MUTE);

    if (is_active && (SDL_GetTicks() / 530) % 2 == 0) {
        int cx = t->rect.x + 8;
        if (t->cursor > 0) {
            char tmp[256];
            int n = t->cursor < t->len ? t->cursor : t->len;
            memcpy(tmp, t->buf, n); tmp[n] = '\0';
            int tw, th; TTF_SizeUTF8(gfnt, tmp, &tw, &th);
            cx += tw;
        }
        scol(C_GOLD);
        SDL_RenderDrawLine(gren, cx, t->rect.y + 5, cx, t->rect.y + t->rect.h - 5);
    }
}

/* ═══════════════════════════════════════════════════════════════════
   Text field input
   ═══════════════════════════════════════════════════════════════════ */
static void tf_activate(int idx) {
    if (atf >= 0 && atf < NFIELDS) tfs[atf].active = 0;
    if (idx >= 0 && idx < NFIELDS) { tfs[idx].active = 1; tfs[idx].cursor = tfs[idx].len; }
    atf = idx;
    SDL_StartTextInput();
}

static void tf_deactivate(void) {
    if (atf >= 0 && atf < NFIELDS) tfs[atf].active = 0;
    atf = -1;
    SDL_StopTextInput();
}

static void tf_insert(TF *t, const char *s) {
    int slen = (int)strlen(s);
    if (t->len + slen >= (int)sizeof(t->buf)) return;
    memmove(t->buf + t->cursor + slen, t->buf + t->cursor, t->len - t->cursor + 1);
    memcpy(t->buf + t->cursor, s, slen);
    t->len += slen; t->cursor += slen;
}

static void tf_backspace(TF *t) {
    if (t->cursor <= 0) return;
    memmove(t->buf + t->cursor - 1, t->buf + t->cursor, t->len - t->cursor + 1);
    t->len--; t->cursor--;
}

static void tf_clear(TF *t) { t->buf[0] = '\0'; t->len = t->cursor = 0; }

/* ═══════════════════════════════════════════════════════════════════
   Git push
   ═══════════════════════════════════════════════════════════════════ */
static void git_push(void) {
    set_status("Pushing to GitHub...");

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "cd %s && git add -A && git commit -m 'Update products' && git push 2>&1",
        GIT_DIR);

    FILE *fp = popen(cmd, "r");
    if (!fp) { set_status("ERROR: popen failed"); return; }

    char line[256], last[256] = {0};
    while (fgets(line, sizeof(line), fp)) strncpy(last, line, sizeof(last) - 1);
    pclose(fp);

    int ll = (int)strlen(last);
    while (ll > 0 && (last[ll-1]=='\n'||last[ll-1]=='\r')) last[--ll] = '\0';
    set_status(last[0] ? last : "Pushed! Website updated.");
}

/* ═══════════════════════════════════════════════════════════════════
   Rendering
   ═══════════════════════════════════════════════════════════════════ */
static void draw_button(SDL_Rect r, const char *lbl, SDL_Color bg, SDL_Color fg) {
    frect(r, bg); drect(r, C_BDR);
    dtext_center(lbl, r, gfnt, fg);
}

static void render(void) {
    scol(C_BG);
    SDL_RenderClear(gren);

    int bary  = WIN_H - BTN_BAR - STAT_H;
    int right = LIST_W + 1;

    /* ── LEFT PANEL ── */
    frect((SDL_Rect){0, 0, LIST_W, bary}, C_PANEL);
    frect((SDL_Rect){0, 0, LIST_W, 40},  C_CARD);
    dtext("Products", PAD, 12, gflg, C_GOLD);
    hline(0, LIST_W, 40, C_BDR);

    int item_h = 46;
    for (int i = 0; i < np; i++) {
        SDL_Rect ir = {0, 40 + i * item_h, LIST_W, item_h};
        SDL_Color bg = (i == sel) ? C_SEL : (i == hov_item) ? C_HOV : C_PANEL;
        frect(ir, bg);
        hline(0, LIST_W, ir.y + item_h - 1, C_BDR);
        if (i == sel) frect((SDL_Rect){0, ir.y, 3, item_h}, C_GOLD);

        SDL_Rect clip = {PAD + 4, ir.y + 8, LIST_W - PAD*2, 18};
        SDL_Color nc  = (i == sel) ? C_LGOLD : C_TEXT;
        dtext_clip(prods[i].name[0] ? prods[i].name : "(unnamed)", clip, gfnt, nc);

        char ptxt[24];
        snprintf(ptxt, sizeof(ptxt), "$%s / %s", prods[i].price, prods[i].size);
        SDL_Rect pc = {PAD + 4, ir.y + 28, LIST_W - PAD*2, 14};
        dtext_clip(ptxt, pc, gfsm, C_MUTE);
    }

    frect((SDL_Rect){0, bary, LIST_W, BTN_BAR}, C_PANEL);
    hline(0, LIST_W, bary, C_BDR);
    draw_button(btn_add, "+ Add",  (SDL_Color){20,40,15,255}, C_GRN);
    draw_button(btn_del, "Delete", (SDL_Color){40,15,15,255}, C_RED);

    /* ── DIVIDER ── */
    frect((SDL_Rect){LIST_W, 0, 1, WIN_H}, C_BDR);

    /* ── RIGHT PANEL ── */
    frect((SDL_Rect){right, 0, WIN_W-right, 40}, C_CARD);
    hline(right, WIN_W, 40, C_BDR);

    if (np == 0) {
        SDL_Rect c = {right, 40, WIN_W-right, bary-40};
        dtext_center("No products — click + Add", c, gfnt, C_MUTE);
    } else {
        char htitle[80];
        snprintf(htitle, sizeof(htitle), "Editing: %s",
                 prods[sel].name[0] ? prods[sel].name : "(new)");
        dtext(htitle, right + PAD, 12, gflg, C_TEXT);

        for (int i = 0; i < NFIELDS; i++) {
            TF *t = &tfs[i];
            dtext(t->label, t->rect.x, t->rect.y - 18, gfsm, C_MUTE);
            draw_tf(t, atf == i);
        }

        /* image section */
        dtext("Product Image", btn_browse.x, btn_browse.y - 18, gfsm, C_MUTE);

        draw_button(btn_browse, "Browse Files",     C_PURP, C_TEXT);
        draw_button(btn_paste,  "Paste Clipboard",
                    (SDL_Color){40,80,60,255}, C_GRN);

        /* accepted formats hint */
        dtext("JPG  PNG  WEBP  GIF  BMP",
              btn_paste.x + btn_paste.w + 14,
              btn_paste.y + 10, gfsm, C_MUTE);

        if (imgtex) {
            int pw = 160, ph = 160;
            float ar = (float)imgw / (imgh > 0 ? imgh : 1);
            if (ar > 1.0f) ph = (int)(pw / ar);
            else           pw = (int)(ph * ar);

            SDL_Rect pr = { btn_browse.x, btn_browse.y + 44, pw, ph };
            SDL_RenderCopy(gren, imgtex, NULL, &pr);
            drect(pr, C_BDR);

            SDL_Rect pclip = { btn_browse.x + pw + 12, pr.y, WIN_W-btn_browse.x-pw-24, 14 };
            dtext_clip(prods[sel].image, pclip, gfsm, C_MUTE);
        } else if (prods[sel].image[0]) {
            dtext(prods[sel].image, btn_browse.x, btn_browse.y + 46, gfsm, C_MUTE);
        } else {
            dtext("No image set — emoji icon used",
                  btn_browse.x, btn_browse.y + 46, gfsm, C_MUTE);
        }
    }

    /* ── BOTTOM BAR ── */
    frect((SDL_Rect){right, bary, WIN_W-right, BTN_BAR}, C_CARD);
    hline(right, WIN_W, bary, C_BDR);
    draw_button(btn_save, "Save & Update HTML", C_GRN, (SDL_Color){10,10,10,255});
    draw_button(btn_push, "Push to GitHub",     (SDL_Color){25,45,25,255}, C_GRN);
    if (dirty) dtext("*", btn_save.x - 14, btn_save.y + 9, gfnt, C_GOLD);

    /* ── STATUS BAR ── */
    int sy = WIN_H - STAT_H;
    frect((SDL_Rect){0, sy, WIN_W, STAT_H}, C_DARK2);
    hline(0, WIN_W, sy, C_BDR);
    dtext(stmsg, PAD, sy + 6, gfsm, C_MUTE);

    /* ── FILE BROWSER (overlay) ── */
    if (fb.open) fb_draw();

    SDL_RenderPresent(gren);
}

/* ═══════════════════════════════════════════════════════════════════
   Event handling
   ═══════════════════════════════════════════════════════════════════ */
static int pt_in(SDL_Rect r, int x, int y) {
    return x >= r.x && x < r.x+r.w && y >= r.y && y < r.y+r.h;
}

/* confirm file browser selection */
static void fb_confirm_sel(void) {
    if (fb.selected < 0) return;
    FBEntry *e = &fb.entries[fb.selected];
    if (e->is_dir) { fb_navigate(e->fullpath); return; }

    char dest_rel[256];
    if (!copy_image(e->fullpath, dest_rel, sizeof(dest_rel))) {
        set_status("ERROR: Could not copy image");
        return;
    }
    strncpy(prods[sel].image, dest_rel, sizeof(prods[sel].image) - 1);
    load_preview(dest_rel);
    dirty  = 1;
    fb.open = 0;
    set_status("Image set! Remember to Save & Update HTML.");
}

static void handle_event(SDL_Event *e) {
    if (e->type == SDL_QUIT) { SDL_Quit(); exit(0); }

    /* ── FILE BROWSER events ── */
    if (fb.open) {
        if (e->type == SDL_MOUSEWHEEL) {
            fb.scroll -= e->wheel.y * 3;
            if (fb.scroll < 0) fb.scroll = 0;
            int max = fb.count - fb_vis();
            if (fb.scroll > max && max >= 0) fb.scroll = max;
            return;
        }
        if (e->type == SDL_MOUSEMOTION) {
            int mx = e->motion.x, my = e->motion.y;
            fb.hovered = -1;
            if (pt_in(fb.list_rect, mx, my)) {
                int idx = fb.scroll + (my - fb.list_rect.y) / FB_ITEM_H;
                if (idx >= 0 && idx < fb.count) fb.hovered = idx;
            }
            return;
        }
        if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
            int mx = e->button.x, my = e->button.y;

            /* close button */
            SDL_Rect close_btn = {fb.rect.x + fb.rect.w - 36, fb.rect.y + 4, 30, 30};
            if (pt_in(close_btn, mx, my)) { fb.open = 0; return; }

            /* up button */
            SDL_Rect up_btn  = {fb.rect.x + fb.rect.w - 80, fb.rect.y + 46, 34, 24};
            SDL_Rect hom_btn = {fb.rect.x + fb.rect.w - 42, fb.rect.y + 46, 34, 24};
            if (pt_in(up_btn, mx, my)) {
                char parent[512];
                strncpy(parent, fb.path, sizeof(parent) - 1);
                char *sl = strrchr(parent, '/');
                if (sl && sl != parent) { *sl = '\0'; fb_navigate(parent); }
                return;
            }
            if (pt_in(hom_btn, mx, my)) {
                const char *home = getenv("HOME");
                fb_navigate(home ? home : "/home");
                return;
            }

            /* bottom buttons */
            int bbw = 160, bbh = 34, bby = fb.rect.y + fb.rect.h - 43;
            SDL_Rect paste_btn  = {fb.rect.x + PAD, bby, bbw, bbh};
            SDL_Rect open_btn   = {fb.rect.x + fb.rect.w - PAD - bbw,       bby, bbw, bbh};
            SDL_Rect cancel_btn = {fb.rect.x + fb.rect.w - PAD - bbw*2 - 8, bby, bbw, bbh};

            if (pt_in(paste_btn,  mx, my)) { try_paste_clipboard(); return; }
            if (pt_in(cancel_btn, mx, my)) { fb.open = 0; return; }
            if (pt_in(open_btn,   mx, my)) { fb_confirm_sel(); return; }

            /* list click */
            if (pt_in(fb.list_rect, mx, my)) {
                int idx = fb.scroll + (my - fb.list_rect.y) / FB_ITEM_H;
                if (idx >= 0 && idx < fb.count) {
                    FBEntry *ent = &fb.entries[idx];
                    if (ent->is_dir) {
                        fb_navigate(ent->fullpath);
                    } else {
                        /* single click selects, double click opens */
                        static int last_idx = -1;
                        static Uint32 last_t = 0;
                        Uint32 now = SDL_GetTicks();
                        if (idx == last_idx && now - last_t < 400) {
                            fb.selected = idx;
                            fb_confirm_sel();
                        } else {
                            fb.selected = idx;
                        }
                        last_idx = idx;
                        last_t   = now;
                    }
                }
            }
            return;
        }
        if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_ESCAPE) {
            fb.open = 0; return;
        }
        if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_RETURN) {
            fb_confirm_sel(); return;
        }
        return;  /* consume all other events while browser is open */
    }

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

        /* add */
        if (pt_in(btn_add, mx, my) && np < MAX_PRODS) {
            fields_to_prod(sel);
            Prod *p = &prods[np];
            memset(p, 0, sizeof(Prod));
            p->id = nxtid++;
            strcpy(p->name, "New Perfume");
            strcpy(p->price, "0");
            strcpy(p->size, "50ml");
            strcpy(p->icon, "🌺");
            sel = np++;
            fields_from_prod(sel);
            dirty = 1;
            return;
        }

        /* delete */
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
                tf_activate(i);
                return;
            }
        }

        /* browse */
        if (pt_in(btn_browse, mx, my)) {
            fields_to_prod(sel);
            tf_deactivate();
            fb_open();
            return;
        }

        /* paste clipboard */
        if (pt_in(btn_paste, mx, my)) {
            fields_to_prod(sel);
            tf_deactivate();
            try_paste_clipboard();
            return;
        }

        /* save */
        if (pt_in(btn_save, mx, my)) { fields_to_prod(sel); save_html(); return; }

        /* push */
        if (pt_in(btn_push, mx, my)) {
            fields_to_prod(sel);
            save_html();
            git_push();
            return;
        }

        tf_deactivate();
        return;
    }

    /* ── KEY DOWN ── */
    if (e->type == SDL_KEYDOWN) {
        if (atf < 0) return;
        SDL_Keycode k = e->key.keysym.sym;
        TF *t = &tfs[atf];

        if (k == SDLK_BACKSPACE) { tf_backspace(t); fields_to_prod(sel); }
        else if (k == SDLK_LEFT  && t->cursor > 0)      t->cursor--;
        else if (k == SDLK_RIGHT && t->cursor < t->len) t->cursor++;
        else if (k == SDLK_HOME) t->cursor = 0;
        else if (k == SDLK_END)  t->cursor = t->len;
        else if (k == SDLK_a && (e->key.keysym.mod & KMOD_CTRL)) {
            tf_clear(t); dirty = 1;
        }
        else if (k == SDLK_TAB) {
            fields_to_prod(sel);
            tf_activate((atf + 1) % NFIELDS);
        }
        else if (k == SDLK_RETURN || k == SDLK_ESCAPE) tf_deactivate();
        else if (k == SDLK_s && (e->key.keysym.mod & KMOD_CTRL)) {
            fields_to_prod(sel); save_html();
        }
        return;
    }

    /* ── TEXT INPUT ── */
    if (e->type == SDL_TEXTINPUT) {
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
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);

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
        fprintf(stderr, "Font load failed: %s\n", TTF_GetError());
        return 1;
    }

    memset(&fb, 0, sizeof(fb));
    setup_layout();
    load_html();
    if (np > 0) fields_from_prod(0);

    SDL_Event ev;
    while (1) {
        while (SDL_PollEvent(&ev)) handle_event(&ev);
        render();
        SDL_Delay(16);
    }

    TTF_CloseFont(gfnt); TTF_CloseFont(gfsm); TTF_CloseFont(gflg);
    TTF_Quit(); IMG_Quit();
    SDL_DestroyRenderer(gren);
    SDL_DestroyWindow(gwin);
    SDL_Quit();
    return 0;
}
