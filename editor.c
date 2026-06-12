/*
 * Perfume Store Product Editor — full rewrite
 * WebSocket runs in a background thread; SDL loop never blocks.
 * Build: make
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_syswm.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include "icon_data.h"
#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

/* ── Paths ─────────────────────────────────────────────────── */
#define HTML_PATH  "/home/xmm/Projects/perfume-store/index.html"
#define IMG_DIR    "/home/xmm/Projects/perfume-store/images"
#define GIT_DIR    "/home/xmm/Projects/perfume-store"
#define FB_DB      "https://yaradutyfree-a7751-default-rtdb.firebaseio.com"
#define GH_RAW     "https://raw.githubusercontent.com/yaradutyfree/perfume-store/main"
#define GH_REPO    "yaradutyfree/perfume-store"
#define FONT_PATH  "/usr/share/fonts/truetype/lato/Lato-Regular.ttf"
#define WS_HOST    "perfume-store-ssls.onrender.com"
#define WS_PORT    443
#define WS_PATH    "/"
#define WS_SECRET  "YaraOud2024"

/* ── Layout ─────────────────────────────────────────────────── */
#define WIN_W     920
#define WIN_H     660
#define LIST_W    240
#define BAR_H      52
#define STAT_H     26
#define PAD        14
#define TF_H       36
#define ITEM_H     50
#define LIST_TOP   80       /* list starts below search bar */

/* ── Orders log ─────────────────────────────────────────────── */
#define ORDERS_FILE "/home/xmm/Projects/perfume-store/orders.jsonl"

/* ── Limits ─────────────────────────────────────────────────── */
#define MAX_PRODS  128
#define BUF_MAX    (1<<21)   /* 2 MB */
#define FB_MAX     1024
#define FB_ROW      28

/* ── Colors ─────────────────────────────────────────────────── */
#define C(r,g,b)    ((SDL_Color){r,g,b,255})
#define CA(r,g,b,a) ((SDL_Color){r,g,b,a})
static const SDL_Color
    BG    = C(  6,  5,  9),
    PANEL = C( 11, 10, 17),
    CARD  = C( 17, 15, 26),
    BDR   = C( 44, 40, 66),
    GOLD  = C(212,175, 55),
    LGOLD = C(248,215,100),
    DGOLD = C(130,100, 22),
    TEXT  = C(242,236,222),
    MUTED = C( 95, 88,108),
    SELB  = C( 32, 24,  5),
    HOVB  = C( 22, 17,  4),
    GRN   = C( 42,195, 92),
    RED   = C(190, 52, 52),
    BLUE  = C( 55,120,225),
    FBHOV = C( 22, 28, 48),
    FBSEL = C( 32, 52, 88);

/* ── Product ─────────────────────────────────────────────────── */
typedef struct {
    int  id;
    char name[64];
    char desc[128];
    char price[20];
    char sale_price[20];
    char size[16];
    char badge[32];
    char icon[16];
    char image[256];
    char brand[48];
    int  in_stock;  /* 1=available, 0=out of stock */
} Prod;

/* ── Brands ──────────────────────────────────────────────────── */
#define MAX_BRANDS 24
static char brands[MAX_BRANDS][48];
static int  nb = 0;

/* ── Text field ──────────────────────────────────────────────── */
typedef struct {
    char     buf[256];
    int      len, cur, active;
    int      sel;   /* selection anchor; -1 = no selection */
    SDL_Rect rect;
    char     label[32];
    char     hint[64];
} TF;

/* ── File browser entry ──────────────────────────────────────── */
typedef struct { char name[256]; char path[512]; int is_dir; } FBEntry;

/* ── WS event queue (ws-thread → main-thread) ───────────────── */
typedef enum { WEV_CONNECTED, WEV_DISCONNECTED, WEV_ORDER, WEV_SYNCED } WEvKind;
typedef struct { WEvKind kind; char data[512]; } WEvent;
#define WEV_CAP 16
static WEvent     wev_buf[WEV_CAP];
static int        wev_head = 0, wev_tail = 0;
static pthread_mutex_t wev_mtx = PTHREAD_MUTEX_INITIALIZER;

static void wev_push(WEvKind k, const char *d) {
    pthread_mutex_lock(&wev_mtx);
    int next = (wev_tail + 1) % WEV_CAP;
    if (next != wev_head) {
        wev_buf[wev_tail].kind = k;
        strncpy(wev_buf[wev_tail].data, d ? d : "", 511);
        wev_tail = next;
    }
    pthread_mutex_unlock(&wev_mtx);
}

static int wev_pop(WEvent *out) {
    pthread_mutex_lock(&wev_mtx);
    if (wev_head == wev_tail) { pthread_mutex_unlock(&wev_mtx); return 0; }
    *out = wev_buf[wev_head];
    wev_head = (wev_head + 1) % WEV_CAP;
    pthread_mutex_unlock(&wev_mtx);
    return 1;
}

/* ── WS send queue (main-thread → ws-thread) ────────────────── */
#define WSQ_CAP 4
static char      *wsq_buf[WSQ_CAP]; /* heap-allocated, any size */
static int        wsq_head = 0, wsq_tail = 0;
static pthread_mutex_t wsq_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct lws_context *ws_ctx = NULL;
static struct lws         *ws_wsi = NULL;
static int                 ws_authed = 0;

static void wsq_push(const char *json) {
    pthread_mutex_lock(&wsq_mtx);
    int next = (wsq_tail + 1) % WSQ_CAP;
    if (next != wsq_head) {
        free(wsq_buf[wsq_tail]);
        wsq_buf[wsq_tail] = strdup(json);
        wsq_tail = next;
    }
    pthread_mutex_unlock(&wsq_mtx);
    if (ws_ctx) lws_cancel_service(ws_ctx);
}

/* ── WS state (atomic-ish, read from main thread) ───────────── */
typedef enum { WS_OFF=0, WS_CONN, WS_READY } WsState;
static volatile WsState ws_state         = WS_OFF;
static volatile int     ws_orders        = 0;
static volatile int     ws_running       = 1;
static volatile int     ws_force_reconnect = 0;

/* ── Notification ────────────────────────────────────────────── */
static struct {
    int    on;
    char   name[64];
    char   city[64];
    char   total[24];
    int    order_no;
    Uint32 at;
} notif;
static pthread_mutex_t notif_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── Globals ─────────────────────────────────────────────────── */
static SDL_Window   *gwin = NULL;
static SDL_Renderer *gren = NULL;
static TTF_Font     *fMD  = NULL;   /* 14 */
static TTF_Font     *fSM  = NULL;   /* 11 */
static TTF_Font     *fLG  = NULL;   /* 18 */

static Prod prods[MAX_PRODS];
static int  np = 0, sel = 0, nxtid = 100;
static int  list_scroll = 0;

enum { FN=0, FD, FP, FSP, FS, FB_F, FB_BRAND, NF };
static TF   tfs[NF];
static int  atf = -1;
static int  brand_dd = 0;

static SDL_Texture *imgtex = NULL;
static int imgw = 0, imgh = 0;

static char   stmsg[256] = "Loading...";

static SDL_Rect
    btn_add, btn_del,
    btn_browse, btn_paste, btn_instock,
    btn_save, btn_push, btn_wssync, btn_brands,
    btn_backup, btn_restore, btn_reconnect;

static int hov_item = -1;
static int dirty    = 0;

/* ── Search ──────────────────────────────────────────────────── */
static char     search_buf[64]  = {0};
static int      search_len      = 0;
static int      search_active   = 0;
static SDL_Rect search_rect;
static int      filt[MAX_PRODS];   /* filtered product indices */
static int      nf = 0;

/* ── Drag-to-reorder ─────────────────────────────────────────── */
static struct {
    int active;    /* dragging in progress */
    int held;      /* mouse button held, might start drag */
    int from_fi;   /* filtered index being dragged */
    int to_fi;     /* insertion point (filtered index) */
    int start_y;
    int cur_y;
} drag;

/* ── Order history panel ─────────────────────────────────────── */
#define HIST_LINES 200
static struct {
    int      open;
    int      scroll;
    char     lines[HIST_LINES][192];
    int      nlines;
    SDL_Rect rect;
} hist;
static SDL_Rect btn_history;

/* full order JSON from WS thread → main thread */
static char     order_json_buf[4096] = {0};
static pthread_mutex_t order_json_mtx = PTHREAD_MUTEX_INITIALIZER;

/* file browser */
static struct {
    int     open;
    char    path[512];
    FBEntry ents[FB_MAX];
    int     cnt, scroll, hov, sel2;
    SDL_Rect rect, list;
} fb;

/* brands panel */
static struct {
    int      open;
    char     addbuf[48];
    int      addlen;
    int      hov;
    SDL_Rect rect;
} bp;

/* ═══════════════════════════════════════════════════════════════
   Drawing
   ═══════════════════════════════════════════════════════════════ */
static void sc(SDL_Color c) { SDL_SetRenderDrawColor(gren,c.r,c.g,c.b,c.a); }
static void fr(SDL_Rect r, SDL_Color c) { sc(c); SDL_RenderFillRect(gren,&r); }
static void dr(SDL_Rect r, SDL_Color c) { sc(c); SDL_RenderDrawRect(gren,&r); }
static void hl(int x1,int x2,int y,SDL_Color c){ sc(c); SDL_RenderDrawLine(gren,x1,y,x2,y); }

static SDL_Texture *mktex(const char *t,TTF_Font *f,SDL_Color c,int*w,int*h){
    if(!t||!t[0]) return NULL;
    SDL_Surface *s=TTF_RenderUTF8_Blended(f,t,c);
    if(!s) return NULL;
    SDL_Texture *tx=SDL_CreateTextureFromSurface(gren,s);
    if(w)*w=s->w; if(h)*h=s->h;
    SDL_FreeSurface(s); return tx;
}

static void dt(const char *t,int x,int y,TTF_Font *f,SDL_Color c){
    int w,h; SDL_Texture *tx=mktex(t,f,c,&w,&h);
    if(!tx) return;
    SDL_Rect d={x,y,w,h}; SDL_RenderCopy(gren,tx,NULL,&d); SDL_DestroyTexture(tx);
}

static void dtclip(const char *t,SDL_Rect clip,TTF_Font *f,SDL_Color c){
    int w,h; SDL_Texture *tx=mktex(t,f,c,&w,&h);
    if(!tx) return;
    SDL_Rect src={0,0,w,h};
    SDL_Rect dst={clip.x, clip.y+(clip.h-h)/2, w, h};
    if(dst.x+dst.w > clip.x+clip.w){ src.w=clip.x+clip.w-dst.x; dst.w=src.w; }
    if(src.w<=0){ SDL_DestroyTexture(tx); return; }
    SDL_RenderSetClipRect(gren,&clip);
    SDL_RenderCopy(gren,tx,&src,&dst);
    SDL_RenderSetClipRect(gren,NULL);
    SDL_DestroyTexture(tx);
}

static void dtctr(const char *t,SDL_Rect r,TTF_Font *f,SDL_Color c){
    int w,h; SDL_Texture *tx=mktex(t,f,c,&w,&h);
    if(!tx) return;
    SDL_Rect d={r.x+(r.w-w)/2, r.y+(r.h-h)/2, w, h};
    SDL_RenderCopy(gren,tx,NULL,&d); SDL_DestroyTexture(tx);
}

/* vertical gradient fill (scanline-by-scanline) */
static void frgrad(SDL_Rect r,SDL_Color t,SDL_Color b){
    if(r.h<=0||r.w<=0) return;
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
    for(int i=0;i<r.h;i++){
        float p=(float)i/(r.h>1?r.h-1:1);
        SDL_SetRenderDrawColor(gren,
            (Uint8)(t.r+(b.r-t.r)*p),
            (Uint8)(t.g+(b.g-t.g)*p),
            (Uint8)(t.b+(b.b-t.b)*p),255);
        SDL_RenderDrawLine(gren,r.x,r.y+i,r.x+r.w-1,r.y+i);
    }
}
/* outer glow — expanding rects with decaying alpha */
static void glow(SDL_Rect r,SDL_Color c,int layers){
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
    for(int i=layers;i>0;i--){
        SDL_Rect g={r.x-i,r.y-i,r.w+i*2,r.h+i*2};
        SDL_SetRenderDrawColor(gren,c.r,c.g,c.b,(Uint8)(55/i));
        SDL_RenderDrawRect(gren,&g);
    }
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
}
/* top shine — two semi-transparent white lines */
static void shine(SDL_Rect r){
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gren,255,255,255,40);
    SDL_RenderDrawLine(gren,r.x+1,r.y+1,r.x+r.w-2,r.y+1);
    SDL_SetRenderDrawColor(gren,255,255,255,16);
    SDL_RenderDrawLine(gren,r.x+2,r.y+2,r.x+r.w-3,r.y+2);
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
}
/* bottom shadow line */
static void shadow_line(SDL_Rect r){
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gren,0,0,0,80);
    SDL_RenderDrawLine(gren,r.x+1,r.y+r.h-1,r.x+r.w-2,r.y+r.h-1);
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
}
/* elegant button: gradient + gold border on hover, shine, shadow */
static void btn(SDL_Rect r,const char *lbl,SDL_Color bg,SDL_Color fg){
    SDL_Color top={bg.r+18>255?255:bg.r+18, bg.g+14>255?255:bg.g+14, bg.b+22>255?255:bg.b+22,255};
    SDL_Color bot={bg.r>12?bg.r-12:0,       bg.g>10?bg.g-10:0,       bg.b>16?bg.b-16:0,       255};
    frgrad(r,top,bot);
    dr(r,BDR);
    shine(r);
    shadow_line(r);
    dtctr(lbl,r,fMD,fg);
}

static void set_st(const char *m){
    strncpy(stmsg,m,255); stmsg[255]=0;
}

/* ═══════════════════════════════════════════════════════════════
   JSON helpers (no external lib)
   ═══════════════════════════════════════════════════════════════ */
static int jstr(const char *j,const char *key,char *out,int sz){
    char k[64]; snprintf(k,sizeof(k),"\"%s\"",key);
    const char *p=strstr(j,k); if(!p) return 0;
    p+=strlen(k); while(*p==':'||*p==' ')p++;
    if(*p=='"'){ p++; int i=0; while(*p&&*p!='"'&&i<sz-1) out[i++]=*p++; out[i]=0; return 1; }
    if(strncmp(p,"null",4)==0){ out[0]=0; return 1; }
    int i=0; while(*p&&*p!=','&&*p!='}'&&*p!='\n'&&i<sz-1) out[i++]=*p++;
    while(i>0&&isspace((unsigned char)out[i-1])) i--;
    out[i]=0; return 1;
}

/* ═══════════════════════════════════════════════════════════════
   WebSocket (runs in background thread)
   ═══════════════════════════════════════════════════════════════ */
static int ws_cb(struct lws *wsi, enum lws_callback_reasons reason,
                 void *user, void *in, size_t len)
{
    (void)user;
    switch(reason){

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        ws_wsi    = wsi;
        ws_state  = WS_CONN;
        ws_authed = 0;
        wev_push(WEV_CONNECTED, NULL);
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        char *payload = NULL;
        int   mlen = 0;
        int   free_payload = 0;

        pthread_mutex_lock(&wsq_mtx);
        if(!ws_authed){
            static char auth[256];
            mlen = snprintf(auth, sizeof(auth),
                "{\"type\":\"admin_auth\",\"secret\":\"%s\"}", WS_SECRET);
            payload = auth;
            ws_authed = 1;
        } else if(wsq_head != wsq_tail){
            payload = wsq_buf[wsq_head];
            wsq_buf[wsq_head] = NULL;
            wsq_head = (wsq_head+1) % WSQ_CAP;
            free_payload = 1;
        }
        int more = (wsq_head != wsq_tail);
        pthread_mutex_unlock(&wsq_mtx);

        if(payload && (mlen = mlen ? mlen : (int)strlen(payload)) > 0){
            unsigned char *buf = malloc(LWS_PRE + mlen);
            if(buf){
                memcpy(buf+LWS_PRE, payload, mlen);
                lws_write(wsi, buf+LWS_PRE, mlen, LWS_WRITE_TEXT);
                free(buf);
            }
            if(free_payload) free(payload);
        }
        if(more) lws_callback_on_writable(wsi);
        break;
    }

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        char *j = (char*)in;
        char  type[32]={0};
        jstr(j,"type",type,sizeof(type));

        if(!strcmp(type,"state")||!strcmp(type,"ack")){
            char cnt[16]={0}; jstr(j,"orderCount",cnt,sizeof(cnt));
            ws_orders = atoi(cnt);
            ws_state  = WS_READY;
            wev_push(WEV_SYNCED, NULL);
        }
        else if(!strcmp(type,"new_order")){
            /* save full JSON for order history */
            pthread_mutex_lock(&order_json_mtx);
            strncpy(order_json_buf,(char*)in,4095); order_json_buf[4095]=0;
            pthread_mutex_unlock(&order_json_mtx);

            pthread_mutex_lock(&notif_mtx);
            char cnt[16]={0};
            jstr(j,"name",  notif.name,  sizeof(notif.name));
            jstr(j,"city",  notif.city,  sizeof(notif.city));
            jstr(j,"total", notif.total, sizeof(notif.total));
            jstr(j,"orderCount",cnt,sizeof(cnt));
            ws_orders   = atoi(cnt);
            notif.order_no = ws_orders;
            notif.on    = 1;
            notif.at    = SDL_GetTicks();
            pthread_mutex_unlock(&notif_mtx);
            wev_push(WEV_ORDER, NULL);
        }
        else if(!strcmp(type,"order_count")){
            char cnt[16]={0}; jstr(j,"count",cnt,sizeof(cnt));
            ws_orders = atoi(cnt);
        }
        /* check if more data pending */
        if(lws_remaining_packet_payload(wsi)) lws_callback_on_writable(wsi);
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    case LWS_CALLBACK_CLIENT_CLOSED:
        ws_wsi   = NULL;
        ws_state = WS_OFF;
        wev_push(WEV_DISCONNECTED, NULL);
        break;

    default: break;
    }
    return 0;
}

static struct lws_protocols ws_protos[] = {
    {"perfume-store", ws_cb, 0, 65536, 0, NULL, 0},
    LWS_PROTOCOL_LIST_TERM
};

static void *ws_thread(void *arg){
    (void)arg;
    Uint32 last_try = 0;

    while(ws_running){
        if(ws_ctx) lws_service(ws_ctx, 50);

        /* force reconnect request from UI */
        if(ws_force_reconnect){
            ws_force_reconnect = 0;
            ws_authed = 0;
            if(ws_wsi){ lws_close_reason(ws_wsi,LWS_CLOSE_STATUS_NORMAL,NULL,0); ws_wsi=NULL; }
            ws_state = WS_OFF;
            last_try = 0;
        }

        /* reconnect logic */
        Uint32 now = SDL_GetTicks();
        if(ws_state==WS_OFF && now-last_try > 5000){
            last_try = now;

            struct lws_client_connect_info cc; memset(&cc,0,sizeof(cc));
            cc.context     = ws_ctx;
            cc.address     = WS_HOST;
            cc.port        = WS_PORT;
            cc.path        = WS_PATH;
            cc.host        = WS_HOST;
            cc.origin      = WS_HOST;
            cc.protocol    = "perfume-store";
            cc.ssl_connection = LCCSCF_USE_SSL
                              | LCCSCF_ALLOW_SELFSIGNED
                              | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;

            ws_state = WS_CONN;
            struct lws *w = lws_client_connect_via_info(&cc);
            if(!w){ ws_state = WS_OFF; }
        }

        /* if queued sends, wake up writable */
        pthread_mutex_lock(&wsq_mtx);
        int has = (wsq_head != wsq_tail);
        pthread_mutex_unlock(&wsq_mtx);
        if(has && ws_wsi) lws_callback_on_writable(ws_wsi);
    }
    if(ws_ctx) lws_context_destroy(ws_ctx);
    return NULL;
}

static const char B64T[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void write_img_b64(FILE *out, const char *relpath); /* defined later */
static void fj(FILE *out, const char *s);                  /* defined later */

/* Convert a local image path to a raw GitHub URL for use in index.html / Firebase.
   "images/prod_26486.jpg" → "https://raw.githubusercontent.com/.../images/prod_26486.jpg"
   A URL that already starts with http is passed through unchanged. */
static void img_to_raw_url(const char *local, char *out, int sz){
    if(!local||!local[0]){ out[0]=0; return; }
    if(strncmp(local,"http",4)==0){ snprintf(out,sz,"%s",local); return; }
    if(strncmp(local,"images/",7)==0)
        snprintf(out,sz,"%s/%s",GH_RAW,local);
    else
        snprintf(out,sz,"%s",local);
}

static void firebase_sync(void){
    set_st("Syncing to Firebase...");
    char *buf=NULL; size_t bsz=0;
    FILE *out=open_memstream(&buf,&bsz);
    if(!out){ set_st("ERROR: open_memstream"); return; }
    fputs("{\"products\":[",out);
    for(int i=0;i<np;i++){
        Prod *p=&prods[i];
        char spj[40], imgurl[512];
        if(p->sale_price[0]) snprintf(spj,sizeof(spj),"%s",p->sale_price); else strcpy(spj,"null");
        img_to_raw_url(p->image, imgurl, sizeof(imgurl));
        fprintf(out,"{\"id\":%d,",p->id);
        fputs("\"name\":",out);    fj(out,p->name);
        fputs(",\"desc\":",out);   fj(out,p->desc);
        fprintf(out,",\"price\":%s,",p->price[0]?p->price:"0");
        fputs("\"size\":",out);    fj(out,p->size[0]?p->size:"100ml");
        fputs(",\"icon\":",out);   fj(out,p->icon[0]?p->icon:"🌹");
        fputs(",\"badge\":",out);  p->badge[0]?fj(out,p->badge):fputs("null",out);
        if(imgurl[0]){ fputs(",\"image\":",out); fj(out,imgurl); }
        else fputs(",\"image\":null",out);
        fputs(",\"brand\":",out);  p->brand[0]?fj(out,p->brand):fputs("null",out);
        fprintf(out,",\"inStock\":%s,\"salePrice\":%s}%s",
            p->in_stock?"true":"false", spj, i<np-1?",":"");
    }
    fputs("],\"brands\":[",out);
    for(int i=0;i<nb;i++){ fj(out,brands[i]); if(i<nb-1) fputc(',',out); }
    fputs("]}",out);
    fclose(out);

    const char *tmp="/tmp/ydf_firebase.json";
    FILE *tf=fopen(tmp,"wb");
    if(!tf){ free(buf); set_st("ERROR: cannot write temp file"); return; }
    fwrite(buf,1,bsz,tf); fclose(tf); free(buf);

    char cmd[512];
    snprintf(cmd,sizeof(cmd),
        "curl -s -X PUT -H 'Content-Type: application/json' --data @%s '%s/store.json' >/dev/null 2>&1",
        tmp, FB_DB);
    int r=system(cmd);
    set_st(r==0?"Synced to Firebase!":"ERROR: Firebase sync failed (check internet)");
}

static size_t b64_to_file(const char*,size_t,const char*); /* forward decl */
static char  *jfield_big(const char*,const char*);         /* forward decl */

/* ═══════════════════════════════════════════════════════════════
   Image
   ═══════════════════════════════════════════════════════════════ */
static void img_free(void){
    if(imgtex){ SDL_DestroyTexture(imgtex); imgtex=NULL; }
}
static void img_load(const char *path){
    img_free();
    if(!path||!path[0]) return;
    SDL_Surface *s=NULL;

    if(strncmp(path,"data:",5)==0){
        /* base64 data URL — decode to temp file then load */
        const char *comma=strchr(path,',');
        if(!comma) return;
        comma++;
        size_t blen=strlen(comma);
        const char *tmp="/tmp/ydf_preview_img";
        b64_to_file(comma,blen,tmp);
        s=IMG_Load(tmp);
    } else {
        s=IMG_Load(path);
        if(!s){ char full[512]; snprintf(full,sizeof(full),"%s/%s",GIT_DIR,path); s=IMG_Load(full); }
    }

    if(!s) return;
    imgw=s->w; imgh=s->h;
    imgtex=SDL_CreateTextureFromSurface(gren,s); SDL_FreeSurface(s);
}
static int img_is(const char *n){
    const char *e=strrchr(n,'.'); if(!e) return 0; e++;
    return !strcasecmp(e,"jpg")||!strcasecmp(e,"jpeg")||!strcasecmp(e,"png")
          ||!strcasecmp(e,"gif")||!strcasecmp(e,"webp")||!strcasecmp(e,"bmp");
}
static int img_copy(const char *src,char *rel,int relsz){
    mkdir(IMG_DIR,0755);
    const char *b=strrchr(src,'/'); b=b?b+1:src;
    char dst[512]; snprintf(dst,sizeof(dst),"%s/%s",IMG_DIR,b);
    FILE *fi=fopen(src,"rb"); if(!fi) return 0;
    FILE *fo=fopen(dst,"wb"); if(!fo){ fclose(fi); return 0; }
    char buf[8192]; size_t n;
    while((n=fread(buf,1,sizeof(buf),fi))>0) fwrite(buf,1,n,fo);
    fclose(fi); fclose(fo);
    snprintf(rel,relsz,"images/%s",b); return 1;
}

/* ═══════════════════════════════════════════════════════════════
   HTML parse / save
   ═══════════════════════════════════════════════════════════════ */
static int exfield(const char *blk,const char *key,char *out,int sz){
    /* match "key": (JSON) or unquoted key: (JS object literal) */
    char qk[80]; snprintf(qk,sizeof(qk),"\"%s\"",key);
    const char *p=strstr(blk,qk);
    if(p){ p+=strlen(qk); }
    else {
        /* look for non-alnum + key + : */
        const char *q=blk; int kl=(int)strlen(key);
        while(*q){
            if(!isalnum((unsigned char)*q)&&*q!='_'){
                if(!strncmp(q+1,key,kl)&&q[1+kl]==':'){p=q+1+kl; break;}
            }
            q++;
        }
    }
    if(!p) return 0;
    while(*p==':'||*p==' ')p++;
    if(*p=='"'){ p++; int i=0; while(*p&&*p!='"'&&i<sz-1) out[i++]=*p++; out[i]=0; return 1; }
    if(!strncmp(p,"null",4)){ out[0]=0; return 1; }
    int i=0; while(*p&&*p!=','&&*p!='}'&&*p!='\n'&&*p!='\r'&&i<sz-1) out[i++]=*p++;
    while(i>0&&isspace((unsigned char)out[i-1])) i--; out[i]=0; return 1;
}

static void apply_filter(void);                          /* forward decl */

/* Like exfield but returns malloc'd full string value (handles quoted + unquoted JS keys) */
static char *exfield_big(const char *blk, const char *key){
    char qk[80]; snprintf(qk,sizeof(qk),"\"%s\"",key);
    const char *p=strstr(blk,qk);
    if(p){ p+=strlen(qk); }
    else {
        const char *q=blk; int kl=(int)strlen(key);
        while(*q){
            if(!isalnum((unsigned char)*q)&&*q!='_'){
                if(!strncmp(q+1,key,kl)&&q[1+kl]==':'){p=q+1+kl; break;}
            }
            q++;
        }
    }
    if(!p) return NULL;
    while(*p==':'||*p==' ') p++;
    if(*p!='"') return NULL;
    p++;
    const char *s=p; size_t len=0;
    while(*p&&*p!='"'){ p++; len++; }
    char *out=malloc(len+1); if(!out) return NULL;
    memcpy(out,s,len); out[len]=0; return out;
}

static void html_load(void){
    FILE *f=fopen(HTML_PATH,"r"); if(!f){ set_st("ERROR: Cannot open index.html"); return; }
    char *buf=malloc(BUF_MAX); if(!buf){ fclose(f); return; }
    int sz=(int)fread(buf,1,BUF_MAX-1,f); buf[sz]=0; fclose(f);

    /* parse brands array */
    nb=0;
    char *ba=strstr(buf,"let brands = [");
    if(ba){
        ba+=strlen("let brands = [");
        char *be=strstr(ba,"\n  ];"); if(!be) be=strstr(ba,"];");
        if(be){
            char *q=ba;
            while(q<be&&nb<MAX_BRANDS){
                char *qs=strchr(q,'"'); if(!qs||qs>=be) break;
                qs++;
                char *qe=strchr(qs,'"'); if(!qe||qe>=be) break;
                int l=(int)(qe-qs); if(l>47) l=47;
                memcpy(brands[nb],qs,l); brands[nb][l]=0; nb++;
                q=qe+1;
            }
        }
    }
    /* defaults if no brands found */
    if(nb==0){
        nb=6;
        strcpy(brands[0],"Lattafa"); strcpy(brands[1],"Afnan");
        strcpy(brands[2],"Armaf");   strcpy(brands[3],"Zakat");
        strcpy(brands[4],"Al Maission"); strcpy(brands[5],"Alhambra");
    }

    /* parse products array */
    np=0; nxtid=1;
    char *arr=strstr(buf,"let products = [");
    if(!arr){ set_st("ERROR: products array not found"); free(buf); return; }
    arr+=strlen("let products = [");

    char *arr_end=strstr(arr,"\n  ];"); if(!arr_end) arr_end=strstr(arr,"\n  ]");
    if(!arr_end){ free(buf); set_st("ERROR: products end not found"); return; }

    char *p=arr;
    while(p<arr_end&&np<MAX_PRODS){
        char *ob=strchr(p,'{'); if(!ob||ob>=arr_end) break;
        int depth=1; char *cb=ob+1;
        while(*cb&&depth){ if(*cb=='{')depth++; else if(*cb=='}')depth--; cb++; }
        int bl=(int)(cb-ob); char *blk=malloc(bl+1);
        memcpy(blk,ob,bl); blk[bl]=0;
        Prod *pr=&prods[np]; memset(pr,0,sizeof(Prod));
        char tmp[32];
        if(exfield(blk,"id",tmp,sizeof(tmp))) pr->id=atoi(tmp);
        exfield(blk,"name",      pr->name,       sizeof(pr->name));
        exfield(blk,"desc",      pr->desc,       sizeof(pr->desc));
        exfield(blk,"price",     pr->price,      sizeof(pr->price));
        exfield(blk,"size",      pr->size,       sizeof(pr->size));
        exfield(blk,"badge",     pr->badge,      sizeof(pr->badge));
        exfield(blk,"icon",      pr->icon,       sizeof(pr->icon));
        exfield(blk,"image",     pr->image,      sizeof(pr->image));
        exfield(blk,"brand",     pr->brand,      sizeof(pr->brand));
        exfield(blk,"salePrice", pr->sale_price, sizeof(pr->sale_price));
        if(!strcmp(pr->badge,"null"))      pr->badge[0]=0;
        if(!strcmp(pr->image,"null"))      pr->image[0]=0;
        /* raw GitHub URL → extract local path "images/prod_<id>.<ext>" */
        if(strncmp(pr->image,"https://raw.githubusercontent.com/",34)==0){
            const char *lp=strstr(pr->image,"images/prod_");
            if(lp) snprintf(pr->image,sizeof(pr->image),"%s",lp);
            else pr->image[0]=0;
        }
        /* if image is a base64 data URL, decode to local file */
        if(strncmp(pr->image,"data:",5)==0){
            char *b64full=exfield_big(blk,"image");
            if(b64full&&b64full[0]){
                const char *comma=strchr(b64full,',');
                if(comma){
                    comma++;
                    mkdir(IMG_DIR,0755);
                    char imgpath[512];
                    snprintf(imgpath,sizeof(imgpath),"%s/prod_%d.jpg",IMG_DIR,pr->id);
                    if(b64_to_file(comma,strlen(comma),imgpath)>0)
                        snprintf(pr->image,sizeof(pr->image),"images/prod_%d.jpg",pr->id);
                }
            }
            free(b64full);
        }
        if(!strcmp(pr->brand,"null"))      pr->brand[0]=0;
        if(!strcmp(pr->sale_price,"null")) pr->sale_price[0]=0;
        /* inStock: default true; false only if explicitly "false" */
        {char stk[8]="true"; exfield(blk,"inStock",stk,sizeof(stk));
         pr->in_stock = strcmp(stk,"false")?1:0;}
        if(pr->id>=nxtid) nxtid=pr->id+1;
        if(pr->name[0]) np++;
        free(blk); p=cb;
    }
    free(buf);
    apply_filter();
    char msg[80]; snprintf(msg,sizeof(msg),"Loaded %d products, %d brands",np,nb);
    set_st(msg); dirty=0;
}

static void fix_tpl(char *buf){
    const char *old="<span>${p.icon}</span>";
    const char *nw ="${p.image?`<img src=\"${p.image}\" alt=\"${p.name}\" />`:`<span>${p.icon}</span>`}";
    char *pos=strstr(buf,old); if(!pos) return;
    int ol=strlen(old),nl=strlen(nw);
    memmove(pos+nl,pos+ol,strlen(pos+ol)+1);
    memcpy(pos,nw,nl);
}

static void html_save(void){
    FILE *f=fopen(HTML_PATH,"r");
    if(!f){ set_st("ERROR: Cannot open HTML"); return; }
    char *buf=malloc(BUF_MAX*2); if(!buf){ fclose(f); return; }
    int sz=(int)fread(buf,1,BUF_MAX-1,f); buf[sz]=0; fclose(f);

    fix_tpl(buf);
    sz=(int)strlen(buf);   /* IMPORTANT: update sz after template fix */

    char *as=strstr(buf,"let products = [");
    if(!as){ set_st("ERROR: marker not found"); free(buf); return; }
    as+=strlen("let products = [");

    char *ae=strstr(as,"\n  ];");
    if(!ae){ set_st("ERROR: end not found"); free(buf); return; }

    char *njs=malloc(BUF_MAX); if(!njs){ free(buf); return; }

    /* build new products JS block */
    int pos=0; pos+=snprintf(njs+pos,BUF_MAX-pos,"\n");
    for(int i=0;i<np;i++){
        Prod *p=&prods[i];
        char bj[80],ij[300],brj[80],spj[40];
        if(p->badge[0])      snprintf(bj, sizeof(bj), "\"%s\"",p->badge); else strcpy(bj,"null");
        if(p->image[0]){ char raw[512]; img_to_raw_url(p->image,raw,sizeof(raw)); snprintf(ij,sizeof(ij),"\"%s\"",raw); } else strcpy(ij,"null");
        if(p->brand[0])      snprintf(brj,sizeof(brj),"\"%s\"",p->brand); else strcpy(brj,"null");
        if(p->sale_price[0]) snprintf(spj,sizeof(spj),"%s",    p->sale_price); else strcpy(spj,"null");
        pos+=snprintf(njs+pos,BUF_MAX-pos,
            "    {\n"
            "      id: %d,\n"
            "      name: \"%s\",\n"
            "      desc: \"%s\",\n"
            "      price: %s,\n"
            "      size: \"%s\",\n"
            "      icon: \"%s\",\n"
            "      badge: %s,\n"
            "      image: %s,\n"
            "      brand: %s,\n"
            "      inStock: %s,\n"
            "      salePrice: %s\n"
            "    }%s\n",
            p->id,p->name,p->desc,
            p->price[0]?p->price:"0",
            p->size,p->icon[0]?p->icon:"🌹",
            bj,ij,brj,
            p->in_stock?"true":"false",
            spj,
            i<np-1?",":"");
    }

    /* find brands section in original buf to replace it */
    char *brands_start=strstr(buf,"let brands = [");
    char *brands_end  = brands_start ? strstr(brands_start,"\n  ];") : NULL;
    if(brands_start && brands_end){
        /* build new brands block */
        char *brnjs=malloc(2048); int bp2=0;
        bp2+=snprintf(brnjs+bp2,2047,"let brands = [\n");
        for(int i=0;i<nb;i++)
            bp2+=snprintf(brnjs+bp2,2047-bp2,"    \"%s\"%s\n",brands[i],i<nb-1?",":"");
        bp2+=snprintf(brnjs+bp2,2047-bp2,"  ];");

        int pre=(int)(brands_start-buf);
        /* skip past ]; in original, handling any extra semicolons */
        const char *after_brnds = brands_end + strlen("\n  ]");
        while(*after_brnds == ';') after_brnds++;
        /* reconstruct buf with new brands */
        char *tmp2=malloc(BUF_MAX*2);
        memcpy(tmp2,buf,pre);
        memcpy(tmp2+pre,brnjs,bp2);
        strcpy(tmp2+pre+bp2, after_brnds);
        /* update buf pointer (re-read needed) */
        sz=(int)strlen(tmp2); tmp2[sz]=0;
        free(buf); buf=tmp2; free(brnjs);
        /* re-find products markers in new buf */
        fix_tpl(buf); sz=(int)strlen(buf);
        as=strstr(buf,"let products = [");
        if(!as){ set_st("ERROR: re-find fail"); free(buf);free(njs); return; }
        as+=strlen("let products = [");
        ae=strstr(as,"\n  ];");
        if(!ae){ set_st("ERROR: re-find end"); free(buf);free(njs); return; }
    }

    int blen=(int)(as-buf);
    int alen=(int)strlen(ae);
    char *out=malloc(blen+pos+alen+8);
    if(!out){ free(buf); free(njs); return; }
    memcpy(out,buf,blen);
    memcpy(out+blen,njs,pos);
    memcpy(out+blen+pos,ae,alen);
    out[blen+pos+alen]=0;

    f=fopen(HTML_PATH,"w");
    if(!f){ set_st("ERROR: Cannot write HTML"); free(out);free(buf);free(njs); return; }
    fputs(out,f); fclose(f);
    free(out); free(buf); free(njs);
    dirty=0; set_st("HTML saved! Use Push or Sync to publish.");
}

/* ═══════════════════════════════════════════════════════════════
   Write data.json (base64 images) for Android app
   ═══════════════════════════════════════════════════════════════ */
static void data_json_save(void){
    char path[512]; snprintf(path,sizeof(path),"%s/data.json",GIT_DIR);
    FILE *out=fopen(path,"wb"); if(!out) return;
    time_t t=time(NULL); struct tm *lt=localtime(&t);
    char ts[32]; strftime(ts,sizeof(ts),"%Y-%m-%dT%H:%M:%S",lt);
    fprintf(out,"{\n  \"version\": 1,\n  \"timestamp\": \"%s\",\n",ts);
    fprintf(out,"  \"brands\": [");
    for(int i=0;i<nb;i++) fprintf(out,"\"%s\"%s",brands[i],i<nb-1?",":"");
    fprintf(out,"],\n");
    fprintf(out,"  \"products\": [\n");
    for(int i=0;i<np;i++){
        Prod *p=&prods[i];
        char bj[80],brj[80],spj[40];
        if(p->badge[0]) snprintf(bj,sizeof(bj),"\"%s\"",p->badge); else strcpy(bj,"null");
        if(p->brand[0]) snprintf(brj,sizeof(brj),"\"%s\"",p->brand); else strcpy(brj,"null");
        if(p->sale_price[0]) snprintf(spj,sizeof(spj),"%s",p->sale_price); else strcpy(spj,"null");
        fprintf(out,"    {\n");
        fprintf(out,"      \"id\": %d,\n",p->id);
        fprintf(out,"      \"name\": \"%s\",\n",p->name);
        fprintf(out,"      \"desc\": \"%s\",\n",p->desc);
        fprintf(out,"      \"price\": %s,\n",p->price[0]?p->price:"0");
        fprintf(out,"      \"salePrice\": %s,\n",spj);
        fprintf(out,"      \"size\": \"%s\",\n",p->size);
        fprintf(out,"      \"icon\": \"%s\",\n",p->icon[0]?p->icon:"🌹");
        fprintf(out,"      \"badge\": %s,\n",bj);
        fprintf(out,"      \"brand\": %s,\n",brj);
        fprintf(out,"      \"inStock\": %s,\n",p->in_stock?"true":"false");
        fprintf(out,"      \"image\": ");
        if(p->image[0]) write_img_b64(out,p->image); else fputs("null",out);
        fprintf(out,"\n    }%s\n",i<np-1?",":"");
    }
    fprintf(out,"  ]\n}\n");
    fclose(out);
}

/* ═══════════════════════════════════════════════════════════════
   Git push
   ═══════════════════════════════════════════════════════════════ */
static void git_push(void){
    set_st("Pushing to GitHub...");
    data_json_save(); /* keep data.json in sync for Android app */
    char cmd[600];
    /* pull --rebase first so we never get a non-fast-forward rejection */
    snprintf(cmd,sizeof(cmd),
        "cd %s && git add -A && git commit -m 'Update products' ; "
        "git pull --rebase origin main 2>&1 && git push 2>&1",GIT_DIR);
    FILE *fp=popen(cmd,"r"); if(!fp){ set_st("ERROR: popen failed"); return; }
    char line[256],last[256]={0};
    int pushed=0;
    while(fgets(line,sizeof(line),fp)){
        strncpy(last,line,255);
        if(strstr(line,"main -> main")||strstr(line,"up-to-date")) pushed=1;
    }
    int rc=pclose(fp);
    int l=strlen(last); while(l>0&&(last[l-1]=='\n'||last[l-1]=='\r')) last[--l]=0;
    if(rc!=0||strstr(last,"error")||strstr(last,"rejected")){
        char msg[320]; snprintf(msg,sizeof(msg),"Push FAILED: %s",last);
        set_st(msg); return; /* do NOT firebase_sync on failure */
    }
    set_st(l?"Pushed!":"Pushed to GitHub!");
    firebase_sync();
}

/* ═══════════════════════════════════════════════════════════════
   Product <-> form
   ═══════════════════════════════════════════════════════════════ */
static void tf_set(TF *t,const char *s){
    strncpy(t->buf,s?s:"",255); t->buf[255]=0;
    t->len=strlen(t->buf); t->cur=t->len; t->sel=-1;
}
static void p2f(int i){
    if(i<0||i>=np) return;
    Prod *p=&prods[i];
    tf_set(&tfs[FN],p->name); tf_set(&tfs[FD],p->desc);
    tf_set(&tfs[FP],p->price); tf_set(&tfs[FSP],p->sale_price);
    tf_set(&tfs[FS],p->size);
    tf_set(&tfs[FB_F],p->badge); tf_set(&tfs[FB_BRAND],p->brand);
    img_load(p->image);
}
static void f2p(int i){
    if(i<0||i>=np) return;
    Prod *p=&prods[i];
    strncpy(p->name,      tfs[FN].buf,      63);
    strncpy(p->desc,      tfs[FD].buf,     127);
    strncpy(p->price,     tfs[FP].buf,      19);
    strncpy(p->sale_price,tfs[FSP].buf,     19);
    strncpy(p->size,      tfs[FS].buf,      15);
    strncpy(p->badge,     tfs[FB_F].buf,    31);
    strncpy(p->brand,     tfs[FB_BRAND].buf,47);
    dirty=1;
}

/* ── Delete product (with confirmation) ─────────────────────── */
static int confirm_delete(const char *name){
    SDL_MessageBoxButtonData btns[2]={
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,0,"Cancelar"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,1,"Eliminar"},
    };
    char msg[160];
    snprintf(msg,sizeof(msg),"¿Eliminar \"%s\" de la tienda?",
             name&&name[0]?name:"(unnamed)");
    SDL_MessageBoxData mb={SDL_MESSAGEBOX_WARNING,gwin,
                           "Eliminar producto",msg,2,btns,NULL};
    int hit=0;
    if(SDL_ShowMessageBox(&mb,&hit)<0) return 1; /* dialog unavailable → proceed */
    return hit==1;
}

static void delete_at(int pi){
    if(pi<0||pi>=np) return;
    char nm[64]; strncpy(nm,prods[pi].name,63); nm[63]=0;
    memmove(&prods[pi],&prods[pi+1],(np-pi-1)*sizeof(Prod));
    np--;
    if(sel>pi) sel--;
    else if(sel==pi){
        if(sel>=np) sel=np>0?np-1:0;
        if(np>0) p2f(sel); else img_free();
    }
    dirty=1; apply_filter();
    char msg[110];
    snprintf(msg,sizeof(msg),"\"%s\" eliminado — Save HTML para confirmar",nm[0]?nm:"(unnamed)");
    set_st(msg);
}

/* ═══════════════════════════════════════════════════════════════
   File browser
   ═══════════════════════════════════════════════════════════════ */
static int fb_cmp(const void *a,const void *b){
    const FBEntry *ea=a,*eb=b;
    if(ea->is_dir!=eb->is_dir) return eb->is_dir-ea->is_dir;
    return strcasecmp(ea->name,eb->name);
}
static void fb_scan(const char *path){
    fb.cnt=0; fb.scroll=0; fb.hov=-1; fb.sel2=-1;
    DIR *d=opendir(path); if(!d) return;
    struct dirent *e;
    while((e=readdir(d))&&fb.cnt<FB_MAX){
        if(!strcmp(e->d_name,".")) continue;
        if(e->d_name[0]=='.'&&strcmp(e->d_name,"..")) continue;
        char full[512]; snprintf(full,sizeof(full),"%s/%s",path,e->d_name);
        struct stat st; if(stat(full,&st)) continue;
        int isd=S_ISDIR(st.st_mode);
        if(!isd&&!img_is(e->d_name)) continue;
        FBEntry *fe=&fb.ents[fb.cnt++];
        strncpy(fe->name,e->d_name,255); strncpy(fe->path,full,511);
        fe->is_dir=isd;
    }
    closedir(d);
    qsort(fb.ents,fb.cnt,sizeof(FBEntry),fb_cmp);
}
static void fb_go(const char *path){
    strncpy(fb.path,path,511); fb_scan(path);
}
static void fb_open_browser(void){
    fb.open=1;
    const char *h=getenv("HOME"); fb_go(h?h:"/home");
    int fw=720,fh=480;
    fb.rect=(SDL_Rect){(WIN_W-fw)/2,(WIN_H-fh)/2,fw,fh};
    fb.list=(SDL_Rect){fb.rect.x+1,fb.rect.y+78,fw-2,fh-78-52};
}
static int fb_vis(void){ return fb.list.h/FB_ROW; }

static void fb_draw(void){
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gren,0,0,0,160); SDL_RenderFillRect(gren,NULL);
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);

    SDL_Rect r=fb.rect;
    fr(r,PANEL); dr(r,GOLD);

    /* title */
    fr((SDL_Rect){r.x,r.y,r.w,38},CARD);
    hl(r.x,r.x+r.w,r.y+38,BDR);
    dt("Select Image File",r.x+PAD,r.y+10,fLG,GOLD);

    SDL_Rect xb={r.x+r.w-34,r.y+4,28,28};
    fr(xb,RED); dtctr("X",xb,fMD,TEXT);

    /* path bar */
    fr((SDL_Rect){r.x,r.y+38,r.w,40},C(18,18,18));
    hl(r.x,r.x+r.w,r.y+78,BDR);
    SDL_Rect pc={r.x+PAD,r.y+46,r.w-100,24}; dtclip(fb.path,pc,fSM,MUTED);
    SDL_Rect ub={r.x+r.w-80,r.y+47,32,22}; fr(ub,CARD); dr(ub,BDR); dtctr("Up",ub,fSM,TEXT);
    SDL_Rect hb={r.x+r.w-44,r.y+47,34,22}; fr(hb,CARD); dr(hb,BDR); dtctr("Home",hb,fSM,GOLD);

    /* list */
    SDL_RenderSetClipRect(gren,&fb.list);
    int vis=fb_vis(), end=fb.scroll+vis; if(end>fb.cnt) end=fb.cnt;
    for(int i=fb.scroll;i<end;i++){
        FBEntry *e=&fb.ents[i];
        int iy=fb.list.y+(i-fb.scroll)*FB_ROW;
        SDL_Rect ir={fb.list.x,iy,fb.list.w,FB_ROW};
        SDL_Color bg=(i==fb.sel2)?FBSEL:(i==fb.hov)?FBHOV:BG;
        fr(ir,bg); hl(ir.x,ir.x+ir.w,iy+FB_ROW-1,BDR);
        const char *icon=e->is_dir?"/":"-";
        dt(icon,ir.x+8,iy+(FB_ROW-12)/2,fSM,e->is_dir?GOLD:MUTED);
        char dn[260]; if(e->is_dir) snprintf(dn,sizeof(dn),"%s/",e->name);
        else strncpy(dn,e->name,259);
        SDL_Rect nc={ir.x+24,iy,ir.w-28,FB_ROW};
        dtclip(dn,nc,fMD,e->is_dir?GOLD:TEXT);
    }
    SDL_RenderSetClipRect(gren,NULL);

    /* scrollbar */
    if(fb.cnt>vis){
        int sbx=r.x+r.w-7,sbh=fb.list.h,sby=fb.list.y;
        int th=sbh*vis/fb.cnt;
        int ty=sby+(int)((float)fb.scroll/(fb.cnt-vis)*(sbh-th));
        fr((SDL_Rect){sbx,sby,5,sbh},CARD);
        fr((SDL_Rect){sbx,ty,5,th},MUTED);
    }

    hl(r.x,r.x+r.w,r.y+r.h-52,BDR);

    /* bottom buttons */
    int bby=r.y+r.h-43,bw=150,bh=34;
    SDL_Rect pbtn={r.x+PAD,bby,bw,bh};
    SDL_Rect cbtn={r.x+r.w-PAD-bw*2-8,bby,bw,bh};
    SDL_Rect obtn={r.x+r.w-PAD-bw,bby,bw,bh};
    fr(pbtn,BLUE); dr(pbtn,BDR); dtctr("Paste Clipboard",pbtn,fSM,TEXT);
    fr(cbtn,RED);  dr(cbtn,BDR); dtctr("Cancel",cbtn,fMD,TEXT);
    SDL_Color oc=fb.sel2>=0?GRN:C(35,55,35);
    fr(obtn,oc); dr(obtn,BDR); dtctr("Open",obtn,fMD,TEXT);
    if(fb.sel2<0) dt("Click an image file",r.x+PAD+bw+12,bby+10,fSM,MUTED);
    else{ SDL_Rect hn={r.x+PAD+bw+12,bby,r.w-PAD*3-bw*3-16,bh}; dtclip(fb.ents[fb.sel2].name,hn,fSM,GOLD); }
}

/* ═══════════════════════════════════════════════════════════════
   Clipboard paste
   ═══════════════════════════════════════════════════════════════ */
static void paste_clip(void){
    char *clip=SDL_GetClipboardText();
    if(clip&&clip[0]){
        char path[512]; strncpy(path,clip,511);
        int l=strlen(path); while(l>0&&(path[l-1]=='\n'||path[l-1]=='\r'||path[l-1]==' ')) path[--l]=0;
        if(!access(path,F_OK)&&img_is(path)){
            char rel[256];
            if(img_copy(path,rel,sizeof(rel))){
                strncpy(prods[sel].image,rel,255);
                img_load(rel); dirty=1; fb.open=0;
                SDL_free(clip); set_st("Image set from clipboard path!"); return;
            }
        }
        SDL_free(clip);
    }
    /* try X clipboard image — use product ID in filename to avoid overwriting */
    char tmp[64]; snprintf(tmp,sizeof(tmp),"/tmp/prod_%d_cb.png",prods[sel].id);
    char xcmd[128]; snprintf(xcmd,sizeof(xcmd),
        "xclip -selection clipboard -t image/png -o > %s 2>/dev/null",tmp);
    if(!system(xcmd)){
        struct stat st; stat(tmp,&st);
        if(st.st_size>0){
            char rel[256];
            if(img_copy(tmp,rel,sizeof(rel))){
                strncpy(prods[sel].image,rel,255);
                img_load(rel); dirty=1; fb.open=0;
                set_st("Image pasted from clipboard!"); return;
            }
        }
    }
    set_st("Clipboard: no image found. Copy an image or file path first.");
}

/* ═══════════════════════════════════════════════════════════════
   Search / Filter
   ═══════════════════════════════════════════════════════════════ */
static void apply_filter(void){
    nf=0;
    if(!search_buf[0]){
        for(int i=0;i<np;i++) filt[nf++]=i;
        return;
    }
    char q[64]; strncpy(q,search_buf,63); q[63]=0;
    for(char *p=q;*p;p++) *p=(char)tolower((unsigned char)*p);
    for(int i=0;i<np;i++){
        char n[64],d[128],b[48];
        strncpy(n,prods[i].name,63); n[63]=0;
        strncpy(d,prods[i].desc,127); d[127]=0;
        strncpy(b,prods[i].brand,47); b[47]=0;
        for(char *p=n;*p;p++) *p=(char)tolower((unsigned char)*p);
        for(char *p=d;*p;p++) *p=(char)tolower((unsigned char)*p);
        for(char *p=b;*p;p++) *p=(char)tolower((unsigned char)*p);
        if(strstr(n,q)||strstr(d,q)||strstr(b,q)) filt[nf++]=i;
    }
}

/* ═══════════════════════════════════════════════════════════════
   Order History
   ═══════════════════════════════════════════════════════════════ */
static void save_order(void){
    pthread_mutex_lock(&order_json_mtx);
    char jcopy[4096]; strncpy(jcopy,order_json_buf,4095); jcopy[4095]=0;
    pthread_mutex_unlock(&order_json_mtx);
    if(!jcopy[0]) return;

    char name[64]={0},city[64]={0},total[32]={0},phone[32]={0};
    jstr(jcopy,"name",  name,  sizeof(name));
    jstr(jcopy,"city",  city,  sizeof(city));
    jstr(jcopy,"total", total, sizeof(total));
    jstr(jcopy,"phone", phone, sizeof(phone));

    time_t now=time(NULL);
    struct tm *lt=localtime(&now);
    char ts[32]; strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M",lt);

    FILE *f=fopen(ORDERS_FILE,"a");
    if(!f) return;
    fprintf(f,"{\"ts\":\"%s\",\"name\":\"%s\",\"city\":\"%s\",\"phone\":\"%s\",\"total\":\"%s\"}\n",
        ts, name[0]?name:"?", city[0]?city:"", phone[0]?phone:"", total[0]?total:"?");
    fclose(f);
}

static void load_hist(void){
    hist.nlines=0; hist.scroll=0;
    FILE *f=fopen(ORDERS_FILE,"r"); if(!f) return;
    /* read all lines into a temp array, then reverse */
    static char tmp[HIST_LINES][192];
    int n=0;
    char line[512];
    while(fgets(line,sizeof(line),f)&&n<HIST_LINES){
        int ll=strlen(line);
        while(ll>0&&(line[ll-1]=='\n'||line[ll-1]=='\r')) line[--ll]=0;
        if(!ll) continue;
        /* parse and format */
        char ts[32]={0},name[64]={0},city[64]={0},phone[32]={0},total[32]={0};
        jstr(line,"ts",    ts,   sizeof(ts));
        jstr(line,"name",  name, sizeof(name));
        jstr(line,"city",  city, sizeof(city));
        jstr(line,"phone", phone,sizeof(phone));
        jstr(line,"total", total,sizeof(total));
        snprintf(tmp[n],sizeof(tmp[n]),"%s  %-20s  %s%s  $%s",
            ts, name,
            city[0]?city:"", phone[0]?" / ":"", total);
        n++;
    }
    fclose(f);
    /* reverse so newest first */
    for(int i=0;i<n;i++)
        strncpy(hist.lines[i],tmp[n-1-i],191);
    hist.nlines=n;
}

static void hist_draw(void){
    SDL_Rect r=hist.rect;
    fr(r,C(16,16,16)); dr(r,GOLD);
    dt("Historial de Pedidos",r.x+PAD,r.y+10,fLG,GOLD);
    SDL_Rect xb={r.x+r.w-34,r.y+8,26,26};
    fr(xb,C(40,14,14)); dr(xb,RED); dtctr("X",xb,fMD,RED);

    hl(r.x,r.x+r.w,r.y+40,BDR);

    if(hist.nlines==0){
        dt("No hay pedidos guardados.",r.x+PAD,r.y+54,fMD,MUTED);
    } else {
        int row_h=28, vis=(r.h-52)/row_h;
        int max_s=hist.nlines-vis; if(max_s<0) max_s=0;
        if(hist.scroll>max_s) hist.scroll=max_s;
        SDL_RenderSetClipRect(gren,&(SDL_Rect){r.x,r.y+42,r.w,r.h-52});
        for(int i=0;i<hist.nlines;i++){
            int iy=r.y+42+(i-hist.scroll)*row_h;
            if(iy+row_h<r.y+42||iy>r.y+r.h-10) continue;
            SDL_Color tc=(i%2==0)?TEXT:MUTED;
            dt(hist.lines[i],r.x+PAD,iy+6,fSM,tc);
            hl(r.x,r.x+r.w,iy+row_h-1,BDR);
        }
        SDL_RenderSetClipRect(gren,NULL);
        /* scrollbar */
        if(hist.nlines>vis){
            int sbh=r.h-52,sby=r.y+42;
            int th=sbh*vis/hist.nlines;
            int ty=sby+(hist.scroll*(sbh-th))/(hist.nlines-vis);
            fr((SDL_Rect){r.x+r.w-5,sby,4,sbh},C(30,30,30));
            fr((SDL_Rect){r.x+r.w-5,ty,4,th},MUTED);
        }
        /* count */
        char cnt[32]; snprintf(cnt,sizeof(cnt),"%d pedido%s",hist.nlines,hist.nlines!=1?"s":"");
        dt(cnt,r.x+PAD,r.y+r.h-20,fSM,MUTED);
    }
}

/* ═══════════════════════════════════════════════════════════════
   Backup / Restore
   ═══════════════════════════════════════════════════════════════ */

/* write image file as base64 data-URL directly to output file */
static void write_img_b64(FILE *out, const char *relpath){
    char full[512];
    FILE *f=fopen(relpath,"rb");
    if(!f){ snprintf(full,sizeof(full),"%s/%s",GIT_DIR,relpath); f=fopen(full,"rb"); }
    if(!f){ fputs("null",out); return; }
    /* detect mime from magic bytes — extension can lie (png saved as .jpg) */
    unsigned char hdr[12]={0};
    size_t hn=fread(hdr,1,12,f);
    fseek(f,0,SEEK_SET);
    char mime[32]="image/jpeg";
    if(hn>=8&&hdr[0]==0x89&&hdr[1]=='P'&&hdr[2]=='N'&&hdr[3]=='G') strcpy(mime,"image/png");
    else if(hn>=12&&!memcmp(hdr,"RIFF",4)&&!memcmp(hdr+8,"WEBP",4)) strcpy(mime,"image/webp");
    else if(hn>=6&&!memcmp(hdr,"GIF8",4)) strcpy(mime,"image/gif");
    else if(hn>=2&&hdr[0]==0xFF&&hdr[1]==0xD8) strcpy(mime,"image/jpeg");
    else {
        const char *ext=strrchr(relpath,'.');
        if(ext&&strcasecmp(ext+1,"jpg")&&strcasecmp(ext+1,"jpeg"))
            snprintf(mime,sizeof(mime),"image/%s",ext+1);
    }
    fprintf(out,"\"data:%s;base64,",mime);
    unsigned char ib[3]; size_t n;
    while((n=fread(ib,1,3,f))>0){
        unsigned char b0=ib[0],b1=(n>1?ib[1]:0),b2=(n>2?ib[2]:0);
        fputc(B64T[(b0>>2)&0x3F],out);
        fputc(B64T[((b0&3)<<4)|((b1>>4)&0xF)],out);
        fputc(n>1?B64T[((b1&0xF)<<2)|((b2>>6)&3)]:'=',out);
        fputc(n>2?B64T[b2&0x3F]:'=',out);
    }
    fclose(f); fputc('"',out);
}

/* JSON-escape a string directly to file */
static void fj(FILE *out, const char *s){
    if(!s||!s[0]){ fputs("null",out); return; }
    fputc('"',out);
    for(;*s;s++){
        if(*s=='"')  fputs("\\\"",out);
        else if(*s=='\\') fputs("\\\\",out);
        else if(*s=='\n') fputs("\\n",out);
        else fputc(*s,out);
    }
    fputc('"',out);
}

static void do_backup(void){
    /* ask for save path via zenity */
    char path[512]={0};
    time_t now=time(NULL);
    struct tm *lt=localtime(&now);
    char defname[80];
    strftime(defname,sizeof(defname),"yaradutyfree_backup_%Y%m%d_%H%M%S.json",lt);
    char cmd[700];
    snprintf(cmd,sizeof(cmd),
        "zenity --file-selection --save --confirm-overwrite "
        "--filename=\"%s/%s\" 2>/dev/null",
        getenv("HOME")?getenv("HOME"):"/home/xmm", defname);
    FILE *zp=popen(cmd,"r");
    if(!zp){ set_st("ERROR: zenity not found"); return; }
    fgets(path,sizeof(path),zp); pclose(zp);
    int pl=strlen(path);
    while(pl>0&&(path[pl-1]=='\n'||path[pl-1]=='\r')) path[--pl]=0;
    if(!pl){ set_st("Backup cancelled"); return; }

    FILE *out=fopen(path,"wb");
    if(!out){ set_st("ERROR: cannot create backup file"); return; }
    set_st("Saving backup...");

    char ts[32]; strftime(ts,sizeof(ts),"%Y-%m-%dT%H:%M:%S",lt);
    fprintf(out,"{\n  \"version\": 1,\n  \"timestamp\": \"%s\",\n",ts);

    /* brands */
    fprintf(out,"  \"brands\": [");
    for(int i=0;i<nb;i++) fprintf(out,"\"%s\"%s",brands[i],i<nb-1?",":"");
    fprintf(out,"],\n");

    /* products */
    fprintf(out,"  \"products\": [\n");
    for(int i=0;i<np;i++){
        Prod *p=&prods[i];
        fprintf(out,"    {\n");
        fprintf(out,"      \"id\": %d,\n",p->id);
        fprintf(out,"      \"name\": "); fj(out,p->name); fprintf(out,",\n");
        fprintf(out,"      \"desc\": "); fj(out,p->desc); fprintf(out,",\n");
        fprintf(out,"      \"price\": %s,\n",p->price[0]?p->price:"0");
        fprintf(out,"      \"salePrice\": %s,\n",p->sale_price[0]?p->sale_price:"null");
        fprintf(out,"      \"size\": "); fj(out,p->size); fprintf(out,",\n");
        fprintf(out,"      \"icon\": "); fj(out,p->icon[0]?p->icon:"🌹"); fprintf(out,",\n");
        fprintf(out,"      \"badge\": "); fj(out,p->badge); fprintf(out,",\n");
        fprintf(out,"      \"brand\": "); fj(out,p->brand); fprintf(out,",\n");
        fprintf(out,"      \"inStock\": %s,\n",p->in_stock?"true":"false");
        fprintf(out,"      \"image\": ");
        if(p->image[0]) write_img_b64(out,p->image);
        else fputs("null",out);
        fprintf(out,"\n    }%s\n",i<np-1?",":"");
    }
    fprintf(out,"  ]\n}\n");
    fclose(out);
    char msg[128]; snprintf(msg,sizeof(msg),"Backup saved: %d products",np);
    set_st(msg);
}

static int b64val(char c){
    if(c>='A'&&c<='Z') return c-'A';
    if(c>='a'&&c<='z') return c-'a'+26;
    if(c>='0'&&c<='9') return c-'0'+52;
    if(c=='+') return 62; if(c=='/') return 63; return -1;
}

/* decode base64 string to a file, return bytes written */
static size_t b64_to_file(const char *b64, size_t blen, const char *outpath){
    FILE *f=fopen(outpath,"wb"); if(!f) return 0;
    size_t written=0;
    for(size_t i=0;i<blen;){
        int v[4]={0,0,0,0}; int k=0;
        while(k<4&&i<blen){
            char c=b64[i++];
            if(c=='='){v[k++]=0;}
            else{ int x=b64val(c); if(x>=0) v[k++]=x; }
        }
        if(k<2) break;
        fputc((v[0]<<2)|(v[1]>>4),f); written++;
        if(k>2){ fputc(((v[1]&0xF)<<4)|(v[2]>>2),f); written++; }
        if(k>3){ fputc(((v[2]&3)<<6)|v[3],f); written++; }
    }
    fclose(f); return written;
}

/* find next JSON object { } in array, return pointer and set *endp past it */
static const char *next_obj(const char *p, const char **endp){
    while(*p&&*p!='{'&&*p!=']') p++;
    if(!*p||*p==']') return NULL;
    const char *start=p++; int depth=1;
    while(*p&&depth>0){
        if(*p=='"'){ p++; while(*p&&*p!='"'){ if(*p=='\\') p++; p++; } }
        else if(*p=='{') depth++;
        else if(*p=='}') depth--;
        p++;
    }
    if(endp) *endp=p;
    return start;
}

/* allocate and return value of a JSON string field (caller frees), handles large values */
static char *jfield_big(const char *json, const char *key){
    char kbuf[80]; snprintf(kbuf,sizeof(kbuf),"\"%s\"",key);
    const char *p=strstr(json,kbuf); if(!p) return NULL;
    p+=strlen(kbuf);
    while(*p==':'||*p==' '||*p=='\t') p++;
    if(strncmp(p,"null",4)==0) return strdup("");
    if(*p!='"') return NULL; p++;
    const char *s=p; size_t len=0;
    while(*p&&!(*p=='"'&&*(p-1)!='\\')){ p++; len++; }
    char *out=malloc(len+1); if(!out) return NULL;
    memcpy(out,s,len); out[len]=0; return out;
}

static void do_restore(void){
    char path[512]={0};
    char cmd[256]="zenity --file-selection --file-filter=\"JSON files | *.json\" 2>/dev/null";
    FILE *zp=popen(cmd,"r");
    if(!zp){ set_st("ERROR: zenity not found"); return; }
    fgets(path,sizeof(path),zp); pclose(zp);
    int pl=strlen(path);
    while(pl>0&&(path[pl-1]=='\n'||path[pl-1]=='\r')) path[--pl]=0;
    if(!pl){ set_st("Restore cancelled"); return; }

    /* read entire file */
    FILE *f=fopen(path,"rb"); if(!f){ set_st("ERROR: cannot open file"); return; }
    fseek(f,0,SEEK_END); long fsz=ftell(f); rewind(f);
    char *json=malloc(fsz+1); if(!json){ fclose(f); set_st("ERROR: out of memory"); return; }
    fread(json,1,fsz,f); fclose(f); json[fsz]=0;

    /* brands */
    const char *barr=strstr(json,"\"brands\"");
    if(barr){ barr=strchr(barr,'['); if(barr){ barr++;
        nb=0;
        while(nb<MAX_BRANDS){
            while(*barr&&*barr!='"'&&*barr!=']') barr++;
            if(!*barr||*barr==']') break;
            barr++; int bi=0;
            while(*barr&&*barr!='"'&&bi<47) brands[nb][bi++]=*barr++;
            brands[nb][bi]=0; if(bi>0) nb++;
            if(*barr=='"') barr++;
        }
    }}

    /* products */
    const char *parr=strstr(json,"\"products\"");
    if(parr){ parr=strchr(parr,'['); if(parr){ parr++;
        np=0; int maxid=0;
        mkdir(IMG_DIR,0755);
        const char *end;
        const char *obj=next_obj(parr,&end);
        while(obj&&np<MAX_PRODS){
            /* extract into temporary buffer */
            size_t olen=end-obj;
            char *blk=malloc(olen+1); if(!blk) break;
            memcpy(blk,obj,olen); blk[olen]=0;

            Prod *p=&prods[np];
            memset(p,0,sizeof(Prod));
            p->in_stock=1;

            char tmp[64]={0};
            jstr(blk,"id",   tmp,sizeof(tmp)); p->id=atoi(tmp);
            jstr(blk,"name", p->name, sizeof(p->name));
            jstr(blk,"desc", p->desc, sizeof(p->desc));
            jstr(blk,"price",tmp,sizeof(tmp)); strncpy(p->price,tmp,19);
            jstr(blk,"salePrice",p->sale_price,sizeof(p->sale_price));
            jstr(blk,"size", p->size, sizeof(p->size));
            jstr(blk,"icon", p->icon, sizeof(p->icon));
            jstr(blk,"badge",p->badge,sizeof(p->badge));
            jstr(blk,"brand",p->brand,sizeof(p->brand));
            jstr(blk,"inStock",tmp,sizeof(tmp));
            if(!strcmp(tmp,"false")||!strcmp(tmp,"0")) p->in_stock=0;

            /* image — may be large base64 data-URL or a path */
            char *imgval=jfield_big(blk,"image");
            if(imgval&&imgval[0]){
                if(strncmp(imgval,"data:",5)==0){
                    /* decode base64 and save as file */
                    const char *b64=strstr(imgval,"base64,");
                    if(b64){ b64+=7;
                        char imgpath[512];
                        snprintf(imgpath,sizeof(imgpath),"%s/prod_%d.jpg",IMG_DIR,p->id);
                        b64_to_file(b64,strlen(b64),imgpath);
                        snprintf(p->image,sizeof(p->image),"images/prod_%d.jpg",p->id);
                    }
                } else {
                    strncpy(p->image,imgval,255);
                }
            }
            free(imgval); free(blk);

            if(p->id>maxid) maxid=p->id;
            np++;
            obj=next_obj(end,&end);
        }
        nxtid=maxid+1;
    }}

    free(json);
    if(sel>=np) sel=np>0?np-1:0;
    if(np>0) p2f(sel); else img_free();
    dirty=1; apply_filter();
    char msg[80]; snprintf(msg,sizeof(msg),"Restored %d products, %d brands",np,nb);
    set_st(msg);
}

/* ═══════════════════════════════════════════════════════════════
   Fill missing images from data.json
   The Android editor writes image:null into index.html and keeps
   the real base64 images only in data.json — so after an Android
   push, index.html alone has no images. Recover them here.
   ═══════════════════════════════════════════════════════════════ */
static void data_json_fill_images(void){
    char path[512]; snprintf(path,sizeof(path),"%s/data.json",GIT_DIR);
    FILE *f=fopen(path,"rb"); if(!f) return;
    fseek(f,0,SEEK_END); long fsz=ftell(f); rewind(f);
    char *json=malloc(fsz+1); if(!json){ fclose(f); return; }
    fread(json,1,fsz,f); fclose(f); json[fsz]=0;

    const char *parr=strstr(json,"\"products\"");
    if(!parr){ free(json); return; }
    parr=strchr(parr,'['); if(!parr){ free(json); return; }
    parr++;
    mkdir(IMG_DIR,0755);

    int filled=0;
    const char *end;
    const char *obj=next_obj(parr,&end);
    while(obj){
        size_t olen=end-obj;
        char *blk=malloc(olen+1); if(!blk) break;
        memcpy(blk,obj,olen); blk[olen]=0;

        char tmp[32]={0}; jstr(blk,"id",tmp,sizeof(tmp));
        int id=atoi(tmp);
        for(int i=0;i<np;i++){
            if(prods[i].id!=id) continue;
            /* skip only if image field is set AND the local file exists */
            if(prods[i].image[0]){
                char fpath[512];
                snprintf(fpath,sizeof(fpath),"%s/%s",GIT_DIR,prods[i].image);
                if(access(fpath,F_OK)==0) continue;
                prods[i].image[0]=0; /* file missing — re-decode */
            }
            char *imgval=jfield_big(blk,"image");
            if(imgval&&strncmp(imgval,"data:",5)==0){
                const char *b64=strstr(imgval,"base64,");
                if(b64){
                    const char *ext="jpg";
                    if(strstr(imgval,"image/png"))       ext="png";
                    else if(strstr(imgval,"image/webp")) ext="webp";
                    else if(strstr(imgval,"image/gif"))  ext="gif";
                    b64+=7;
                    char imgpath[512];
                    snprintf(imgpath,sizeof(imgpath),"%s/prod_%d.%s",IMG_DIR,id,ext);
                    if(b64_to_file(b64,strlen(b64),imgpath)>0){
                        snprintf(prods[i].image,sizeof(prods[i].image),
                                 "images/prod_%d.%s",id,ext);
                        filled++;
                    }
                }
            } else if(imgval&&strncmp(imgval,"https://raw.",12)==0){
                /* raw GitHub URL — download to local file if not cached */
                const char *fn=strrchr(imgval,'/');
                if(fn){ fn++;
                    const char *dot=strrchr(fn,'.'); const char *ext=dot?dot+1:"jpg";
                    char lpath[512];
                    snprintf(lpath,sizeof(lpath),"%s/prod_%d.%s",IMG_DIR,id,ext);
                    if(access(lpath,F_OK)!=0){
                        char cmd[768];
                        snprintf(cmd,sizeof(cmd),"curl -sL '%s' -o '%s' 2>/dev/null",imgval,lpath);
                        system(cmd);
                    }
                    if(access(lpath,F_OK)==0){
                        snprintf(prods[i].image,sizeof(prods[i].image),"images/prod_%d.%s",id,ext);
                        filled++;
                    }
                }
            }
            free(imgval);
            break;
        }
        free(blk);
        obj=next_obj(end,&end);
    }
    free(json);

    /* last resort: a previously decoded local file images/prod_<id>.* */
    for(int i=0;i<np;i++){
        if(prods[i].image[0]) continue;
        const char *exts[]={"jpg","png","webp","gif"};
        for(int e=0;e<4;e++){
            char fpath[512];
            snprintf(fpath,sizeof(fpath),"%s/prod_%d.%s",IMG_DIR,prods[i].id,exts[e]);
            if(!access(fpath,F_OK)){
                snprintf(prods[i].image,sizeof(prods[i].image),
                         "images/prod_%d.%s",prods[i].id,exts[e]);
                filled++;
                break;
            }
        }
    }

    if(filled){
        char msg[96];
        snprintf(msg,sizeof(msg),"Loaded %d products, %d brands — %d images",np,nb,filled);
        set_st(msg);
    }
}

/* ═══════════════════════════════════════════════════════════════
   Layout
   ═══════════════════════════════════════════════════════════════ */
static void layout(void){
    int ry=WIN_H-BAR_H-STAT_H;
    int rx=LIST_W+1, rw=WIN_W-rx;

    /* left panel buttons */
    int bw=(LIST_W-PAD*2-8)/2;
    btn_add   =(SDL_Rect){PAD,     ry+(BAR_H-34)/2, bw,   34};
    btn_del   =(SDL_Rect){PAD+bw+8,ry+(BAR_H-34)/2, bw,   34};
    btn_brands =(SDL_Rect){PAD,           ry-42,  LIST_W-PAD*2, 28};
    int bw2=(LIST_W-PAD*2-4)/2;
    btn_backup =(SDL_Rect){PAD,           ry-80,  bw2,          28};
    btn_restore=(SDL_Rect){PAD+bw2+4,     ry-80,  bw2,          28};

    /* right panel bottom buttons */
    int pw=120;
    btn_wssync   =(SDL_Rect){rx+PAD,             ry+(BAR_H-34)/2, pw,   34};
    btn_reconnect=(SDL_Rect){rx+PAD+pw+8,        ry+(BAR_H-34)/2, 34,   34};
    btn_history  =(SDL_Rect){rx+PAD+pw+8+34+6,   ry+(BAR_H-34)/2, pw,   34};
    btn_save     =(SDL_Rect){WIN_W-PAD-pw*2-8,   ry+(BAR_H-34)/2, pw,   34};
    btn_push     =(SDL_Rect){WIN_W-PAD-pw,        ry+(BAR_H-34)/2, pw,   34};

    /* search bar (left panel, between header and list) */
    search_rect=(SDL_Rect){PAD, 46, LIST_W-PAD*2, 28};

    /* history panel */
    hist.rect=(SDL_Rect){rx+12, 50, WIN_W-rx-24, WIN_H-BAR_H-STAT_H-60};

    /* text fields */
    int fx=rx+PAD, fw=rw-PAD*2;
    int fy=56, gap=TF_H+26;

    tfs[FN].rect=(SDL_Rect){fx,           fy,       fw,        TF_H};
    tfs[FD].rect=(SDL_Rect){fx,           fy+gap,   fw,        TF_H};
    tfs[FP].rect=(SDL_Rect){fx,           fy+gap*2, fw/5-4,    TF_H};
    tfs[FSP].rect=(SDL_Rect){fx+fw/5+4,   fy+gap*2, fw/5-4,    TF_H};
    tfs[FS].rect=(SDL_Rect){fx+fw*2/5+6,  fy+gap*2, fw/5-4,    TF_H};
    tfs[FB_F].rect=(SDL_Rect){fx+fw*3/5+8,fy+gap*2, fw/5-4,    TF_H};
    tfs[FB_BRAND].rect=(SDL_Rect){fx+fw*4/5+10,fy+gap*2,fw/5-10,TF_H};

    strcpy(tfs[FN].label,"Name");
    strcpy(tfs[FN].hint,"e.g. Rose Oud");
    strcpy(tfs[FD].label,"Description");
    strcpy(tfs[FD].hint,"Short tagline");
    strcpy(tfs[FP].label,"Price");
    strcpy(tfs[FP].hint,"130000");
    strcpy(tfs[FSP].label,"Sale Price");
    strcpy(tfs[FSP].hint,"empty=none");
    strcpy(tfs[FS].label,"Size");
    strcpy(tfs[FS].hint,"100ml");
    strcpy(tfs[FB_F].label,"Badge");
    strcpy(tfs[FB_F].hint,"Nuevo");
    strcpy(tfs[FB_BRAND].label,"Brand");
    strcpy(tfs[FB_BRAND].hint,"Lattafa");

    int img_y=fy+gap*3+8;
    btn_instock=(SDL_Rect){fx,          img_y-44, 120, 26};
    btn_browse =(SDL_Rect){fx,          img_y,    160, 34};
    btn_paste  =(SDL_Rect){fx+168,      img_y,    160, 34};
}

/* x-position in pixels of character index i inside field t */
static int tf_cx(TF *t,int i){
    if(i<=0) return t->rect.x+8;
    char tmp[256]; int n=i<t->len?i:t->len;
    memcpy(tmp,t->buf,n); tmp[n]=0;
    int tw,th; TTF_SizeUTF8(fMD,tmp,&tw,&th);
    return t->rect.x+8+tw;
}

/* ═══════════════════════════════════════════════════════════════
   Text field
   ═══════════════════════════════════════════════════════════════ */
static void tf_draw(TF *t,int active){
    fr(t->rect,CARD); dr(t->rect,active?GOLD:BDR);
    /* selection highlight */
    if(active&&t->sel>=0&&t->sel!=t->cur){
        int lo=t->sel<t->cur?t->sel:t->cur;
        int hi=t->sel<t->cur?t->cur:t->sel;
        int x0=tf_cx(t,lo), x1=tf_cx(t,hi);
        SDL_Rect sr={x0,t->rect.y+3,x1-x0,t->rect.h-6};
        sc(C(60,80,160)); SDL_RenderFillRect(gren,&sr);
    }
    SDL_Rect clip={t->rect.x+8,t->rect.y,t->rect.w-16,t->rect.h};
    if(t->len) dtclip(t->buf,clip,fMD,TEXT); else dtclip(t->hint,clip,fMD,MUTED);
    /* cursor: show when no selection and blink tick is on */
    if(active&&t->sel<0&&(SDL_GetTicks()/530)%2==0){
        int cx=tf_cx(t,t->cur);
        sc(GOLD); SDL_RenderDrawLine(gren,cx,t->rect.y+5,cx,t->rect.y+t->rect.h-5);
    }
}
static void tf_on(int idx){
    if(atf>=0&&atf<NF) tfs[atf].active=0;
    if(idx>=0&&idx<NF){ tfs[idx].active=1; tfs[idx].cur=tfs[idx].len; tfs[idx].sel=-1; }
    atf=idx; SDL_StartTextInput();
    brand_dd = (idx == FB_BRAND) ? 1 : 0;
}
static void tf_off(void){
    if(atf>=0&&atf<NF) tfs[atf].active=0;
    atf=-1; SDL_StopTextInput();
    brand_dd = 0;
}
/* Delete the selected range [lo,hi) and place cursor at lo. */
static void tf_del_sel(TF *t){
    if(t->sel<0) return;
    int lo=t->sel<t->cur?t->sel:t->cur;
    int hi=t->sel<t->cur?t->cur:t->sel;
    memmove(t->buf+lo,t->buf+hi,t->len-hi+1);
    t->len-=(hi-lo); t->cur=lo; t->sel=-1;
}
static void tf_ins(TF *t,const char *s){
    if(t->sel>=0) tf_del_sel(t);          /* replace selection */
    int sl=strlen(s); if(t->len+sl>=(int)sizeof(t->buf)) return;
    memmove(t->buf+t->cur+sl,t->buf+t->cur,t->len-t->cur+1);
    memcpy(t->buf+t->cur,s,sl); t->len+=sl; t->cur+=sl;
}
static void tf_bs(TF *t){
    if(t->sel>=0){ tf_del_sel(t); return; }  /* delete selection */
    if(!t->cur) return;
    memmove(t->buf+t->cur-1,t->buf+t->cur,t->len-t->cur+1);
    t->len--; t->cur--;
}
static void tf_clr(TF *t){ t->buf[0]=0; t->len=t->cur=t->sel=0; t->sel=-1; }
/* Ctrl+C / Ctrl+X */
static void tf_copy(TF *t){
    if(t->sel<0){ SDL_SetClipboardText(t->buf); return; }
    int lo=t->sel<t->cur?t->sel:t->cur;
    int hi=t->sel<t->cur?t->cur:t->sel;
    char tmp[256]; int n=hi-lo; if(n<=0)return;
    memcpy(tmp,t->buf+lo,n); tmp[n]=0;
    SDL_SetClipboardText(tmp);
}
static void tf_cut(TF *t){ tf_copy(t); if(t->sel>=0) tf_del_sel(t); else tf_clr(t); }
/* Ctrl+V */
static void tf_paste(TF *t){
    if(!SDL_HasClipboardText()) return;
    char *clip=SDL_GetClipboardText();
    if(clip){ tf_ins(t,clip); SDL_free(clip); }
}

/* ═══════════════════════════════════════════════════════════════
   Render
   ═══════════════════════════════════════════════════════════════ */
static void render(void){
    sc(BG); SDL_RenderClear(gren);

    int bary=WIN_H-BAR_H-STAT_H;
    int rx=LIST_W+1;

    /* ── LEFT PANEL ── */
    fr((SDL_Rect){0,0,LIST_W,bary},PANEL);
    /* header gradient: deep card → panel */
    frgrad((SDL_Rect){0,0,LIST_W,LIST_TOP}, C(22,18,32), C(14,12,22));
    /* thin gold accent top bar */
    frgrad((SDL_Rect){0,0,LIST_W,2}, LGOLD, GOLD);
    dt("PRODUCTS",PAD,10,fSM,GOLD);
    /* count badge */
    char pcnt[24]; snprintf(pcnt,sizeof(pcnt),"%d",np);
    {
        int cw=0; SDL_Surface *cs=TTF_RenderUTF8_Blended(fSM,pcnt,GOLD); if(cs){cw=cs->w;SDL_FreeSurface(cs);}
        SDL_Rect badge={LIST_W-PAD-cw-6, 8, cw+6, 14};
        frgrad(badge,C(38,28,8),C(28,18,4)); dr(badge,DGOLD);
        dt(pcnt,badge.x+3,badge.y+1,fSM,LGOLD);
    }

    /* search bar */
    {
        SDL_Color sbg=search_active?C(28,22,6):C(18,16,26);
        SDL_Color sbdr=search_active?GOLD:BDR;
        frgrad(search_rect,C(sbg.r+6,sbg.g+5,sbg.b+8),sbg);
        dr(search_rect,sbdr);
        if(search_active) glow(search_rect,GOLD,2);
        if(search_buf[0]){
            SDL_Rect sc2={search_rect.x+20,search_rect.y,search_rect.w-32,search_rect.h};
            dtclip(search_buf,sc2,fSM,TEXT);
            if(search_active){
                int cw=0;
                SDL_Surface *ss=TTF_RenderUTF8_Blended(fSM,search_buf,TEXT);
                if(ss){cw=ss->w;SDL_FreeSurface(ss);}
                int cx=search_rect.x+20+cw;
                SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,200);
                SDL_RenderDrawLine(gren,cx,search_rect.y+5,cx,search_rect.y+search_rect.h-5);
                SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
            }
        } else {
            dt("🔍",search_rect.x+4,search_rect.y+6,fSM,MUTED);
            dt("Search...",search_rect.x+20,search_rect.y+7,fSM,MUTED);
        }
        if(search_buf[0]){
            SDL_Rect clx={search_rect.x+search_rect.w-18,search_rect.y+7,12,14};
            dtctr("×",clx,fSM,MUTED);
        }
    }
    /* header bottom line — subtle gold */
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,60);
    SDL_RenderDrawLine(gren,0,LIST_TOP,LIST_W,LIST_TOP);
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);

    /* product list — filtered, stops 90px above bar */
    int list_bot=bary-90;
    int vis_items=(list_bot-LIST_TOP)/ITEM_H;
    int max_scroll=nf-vis_items; if(max_scroll<0) max_scroll=0;
    if(list_scroll>max_scroll) list_scroll=max_scroll;

    SDL_RenderSetClipRect(gren,&(SDL_Rect){0,LIST_TOP,LIST_W,list_bot-LIST_TOP});
    for(int fi=0;fi<nf;fi++){
        int pi=filt[fi];
        int iy=LIST_TOP+(fi-list_scroll)*ITEM_H;
        if(iy+ITEM_H<LIST_TOP||iy>list_bot) continue;

        /* drag insertion line */
        if(drag.active&&fi==drag.to_fi&&fi!=drag.from_fi){
            SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,200);
            SDL_RenderFillRect(gren,&(SDL_Rect){0,iy,LIST_W,2});
            SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
        }
        if(drag.active&&fi==drag.from_fi) continue;

        SDL_Rect ir={0,iy,LIST_W,ITEM_H};
        int is_sel=(pi==sel), is_hov=(pi==hov_item);
        if(is_sel){
            frgrad(ir, C(40,28,6), C(26,18,3));
        } else if(is_hov){
            frgrad(ir, C(24,20,5), C(18,14,3));
        } else {
            fr(ir,PANEL);
        }
        /* separator */
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gren,BDR.r,BDR.g,BDR.b,100);
        SDL_RenderDrawLine(gren,PAD,iy+ITEM_H-1,LIST_W-PAD,iy+ITEM_H-1);
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
        /* gold left accent bar with glow when selected */
        if(is_sel){
            frgrad((SDL_Rect){0,iy,4,ITEM_H},LGOLD,GOLD);
            SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
            for(int g=3;g>0;g--){
                SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,(Uint8)(40/g));
                SDL_RenderDrawLine(gren,g,iy,g,iy+ITEM_H-1);
            }
            SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
        }
        /* drag handle */
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gren,80,72,100,120);
        for(int d=0;d<3;d++){
            SDL_RenderDrawPoint(gren,6,iy+12+d*6);
            SDL_RenderDrawPoint(gren,9,iy+12+d*6);
        }
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
        /* name */
        SDL_Rect nc={PAD+10,iy+7,LIST_W-PAD*2-36,18};
        dtclip(prods[pi].name[0]?prods[pi].name:"(unnamed)",nc,fMD,is_sel?LGOLD:TEXT);
        /* price */
        char pt[32]; snprintf(pt,sizeof(pt),"$%s · %s",prods[pi].price,prods[pi].size);
        SDL_Rect pc={PAD+10,iy+27,LIST_W-PAD*2-36,14}; dtclip(pt,pc,fSM,is_sel?C(180,150,70):MUTED);
        /* out-of-stock dot */
        if(!prods[pi].in_stock){
            SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(gren,RED.r,RED.g,RED.b,200);
            SDL_RenderFillRect(gren,&(SDL_Rect){LIST_W-34,iy+22,6,6});
            SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
        }
        /* per-row delete × */
        {
            SDL_Rect xr={LIST_W-24,iy+(ITEM_H-18)/2,18,18};
            int lit=is_hov||is_sel;
            fr(xr,lit?C(60,18,18):C(30,13,16));
            dr(xr,lit?C(110,40,40):C(60,28,32));
            dtctr("×",xr,fMD,lit?C(235,110,110):C(140,70,75));
        }
    }
    /* drag ghost */
    if(drag.active&&drag.from_fi>=0&&drag.from_fi<nf){
        int pi=filt[drag.from_fi];
        int gy=drag.cur_y-ITEM_H/2;
        SDL_Rect gr={0,gy,LIST_W,ITEM_H};
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gren,40,30,8,200); SDL_RenderFillRect(gren,&gr);
        SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,160); SDL_RenderDrawRect(gren,&gr);
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
        SDL_Rect nc={PAD+10,gy+7,LIST_W-PAD*2,18};
        dtclip(prods[pi].name[0]?prods[pi].name:"(unnamed)",nc,fMD,LGOLD);
    }
    SDL_RenderSetClipRect(gren,NULL);

    /* scrollbar — thin gold-tinted */
    if(nf>vis_items){
        int sbh=list_bot-LIST_TOP, sby=LIST_TOP;
        int th=sbh*vis_items/nf; if(th<12)th=12;
        int ty=sby+(list_scroll*(sbh-th))/(nf-vis_items>0?nf-vis_items:1);
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gren,BDR.r,BDR.g,BDR.b,60);
        SDL_RenderFillRect(gren,&(SDL_Rect){LIST_W-4,sby,3,sbh});
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
        frgrad((SDL_Rect){LIST_W-4,ty,3,th},LGOLD,GOLD);
    }

    /* cover partial items + button area gradient */
    frgrad((SDL_Rect){0,list_bot,LIST_W,bary-list_bot},C(16,13,24),C(13,11,20));
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,40);
    SDL_RenderDrawLine(gren,0,list_bot,LIST_W,list_bot);
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);

    /* left bar */
    frgrad((SDL_Rect){0,bary,LIST_W,BAR_H},C(18,15,28),C(12,10,20));
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,50);
    SDL_RenderDrawLine(gren,0,bary,LIST_W,bary);
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
    btn(btn_add,"+ Add",   C(14,36,12),GRN);
    btn(btn_del,"Delete",  C(42,12,12),RED);

    /* ── DIVIDER — subtle vertical gold line ── */
    frgrad((SDL_Rect){LIST_W,0,1,WIN_H},DGOLD,C(20,15,6));

    /* ── RIGHT PANEL HEADER ── */
    frgrad((SDL_Rect){rx,0,WIN_W-rx,46},C(22,18,34),C(14,12,24));
    frgrad((SDL_Rect){rx,0,WIN_W-rx,2},LGOLD,GOLD);
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,50);
    SDL_RenderDrawLine(gren,rx,46,WIN_W,46);
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);

    if(np==0){
        dt("— No Products —",rx+PAD,13,fLG,MUTED);
    } else {
        char htitle[80]; snprintf(htitle,sizeof(htitle),"%s",
            prods[sel].name[0]?prods[sel].name:"(new)");
        dt(htitle,rx+PAD,13,fLG,LGOLD);
    }

    /* WS live indicator — pulsing dot */
    {
        SDL_Color dc = ws_state==WS_READY?GRN : ws_state==WS_CONN?C(200,165,30):RED;
        /* pulse: slightly vary brightness using ticks */
        if(ws_state==WS_READY){
            Uint32 t=SDL_GetTicks();
            float pulse=0.7f+0.3f*(float)(SDL_sin((double)t/600.0)*0.5+0.5);
            dc.r=(Uint8)(dc.r*pulse); dc.g=(Uint8)(dc.g*pulse); dc.b=(Uint8)(dc.b*pulse);
        }
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gren,dc.r,dc.g,dc.b,60);
        SDL_RenderFillRect(gren,&(SDL_Rect){WIN_W-138,12,14,14});
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
        fr((SDL_Rect){WIN_W-135,15,8,8},dc);
        const char *wsl=ws_state==WS_READY?"Live":ws_state==WS_CONN?"Connecting…":"Offline";
        dt(wsl,WIN_W-122,12,fSM,dc);
        char oc[32]; snprintf(oc,sizeof(oc),"Orders: %d",ws_orders);
        dt(oc,WIN_W-122,24,fSM,MUTED);
    }

    /* ── EDIT FORM ── */
    if(np>0){
        for(int i=0;i<NF;i++){
            dt(tfs[i].label,tfs[i].rect.x,tfs[i].rect.y-16,fSM,MUTED);
            tf_draw(&tfs[i],atf==i);
        }

        /* brand dropdown */
        if(brand_dd && nb>0){
            SDL_Rect bf = tfs[FB_BRAND].rect;
            int ddw = bf.w < 160 ? 160 : bf.w;
            int ddx = bf.x + bf.w - ddw;
            int ddy = bf.y + bf.h + 2;
            int rowh = 26;
            SDL_Rect bbox = {ddx, ddy, ddw, nb*rowh};
            fr(bbox, PANEL); dr(bbox, GOLD);
            for(int i=0;i<nb;i++){
                SDL_Rect row={ddx, ddy+i*rowh, ddw, rowh};
                int hov = (tfs[FB_BRAND].len>0 &&
                    strncasecmp(brands[i], tfs[FB_BRAND].buf, tfs[FB_BRAND].len)==0);
                fr(row, hov ? C(38,28,6) : (i%2==0?PANEL:CARD));
                SDL_Rect clip={row.x+8,row.y+4,row.w-16,row.h-4};
                dtclip(brands[i], clip, fSM, hov?LGOLD:TEXT);
            }
        }

        /* in_stock toggle */
        {
            int stk=prods[sel].in_stock;
            SDL_Color bc=stk?C(14,55,28):C(60,14,14);
            SDL_Color fc=stk?GRN:RED;
            btn(btn_instock, stk?"✓ En Stock":"✗ Agotado", bc,fc);
        }

        /* image section */
        dt("Product Image",btn_browse.x,btn_browse.y-16,fSM,MUTED);
        btn(btn_browse,"Browse Files", BLUE,TEXT);
        btn(btn_paste, "Paste Clipboard",C(20,55,35),GRN);
        dt("JPG PNG WEBP GIF BMP",btn_paste.x+btn_paste.w+12,btn_paste.y+11,fSM,MUTED);

        if(imgtex){
            int pw=150,ph=150;
            float ar=(float)imgw/(imgh>0?imgh:1);
            if(ar>1) ph=(int)(pw/ar); else pw=(int)(ph*ar);
            SDL_Rect pr={btn_browse.x,btn_browse.y+44,pw,ph};
            SDL_RenderCopy(gren,imgtex,NULL,&pr); dr(pr,BDR);
            SDL_Rect pc={btn_browse.x+pw+10,pr.y,WIN_W-btn_browse.x-pw-20,14};
            dtclip(prods[sel].image,pc,fSM,MUTED);
        } else {
            const char *imsg=prods[sel].image[0]?prods[sel].image:"No image — emoji used";
            dt(imsg,btn_browse.x,btn_browse.y+46,fSM,MUTED);
        }
    } else {
        SDL_Rect ctr={rx,42,WIN_W-rx,bary-42};
        dtctr("Click + Add to create your first product",ctr,fMD,MUTED);
    }

    /* ── BOTTOM BAR ── */
    frgrad((SDL_Rect){rx,bary,WIN_W-rx,BAR_H},C(18,15,28),C(10,9,18));
    /* top gold accent line */
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,55);
    SDL_RenderDrawLine(gren,rx,bary,WIN_W,bary);
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);

    SDL_Color wbc=ws_state==WS_READY?C(12,38,58):C(22,20,34);
    SDL_Color wfc=ws_state==WS_READY?C(120,210,255):MUTED;
    btn(btn_wssync,"Sync Live",wbc,wfc);
    if(ws_state==WS_READY) glow(btn_wssync,C(60,160,220),3);
    /* ↻ reconnect button */
    fr(btn_reconnect, C(28,22,40)); dr(btn_reconnect, C(70,55,90));
    dtctr("↻", btn_reconnect, fMD, ws_state==WS_READY?C(120,210,255):C(160,120,200));
    btn(btn_history,"History",C(22,20,38),hist.open?LGOLD:MUTED);
    if(hist.open) glow(btn_history,GOLD,2);
    btn(btn_save,  "Save HTML",C(10,40,16),GRN);
    btn(btn_push,  "Push GitHub",C(10,32,10),C(80,220,80));
    if(dirty){
        glow(btn_save,GOLD,3);
        dr(btn_save,GOLD);
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,200);
        SDL_RenderDrawPoint(gren,btn_save.x-10,btn_save.y+btn_save.h/2);
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
        dt("●",btn_save.x-14,btn_save.y+9,fSM,GOLD);
    }

    /* ── STATUS BAR ── */
    int sy=WIN_H-STAT_H;
    frgrad((SDL_Rect){0,sy,WIN_W,STAT_H},C(10,9,16),C(7,6,12));
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gren,GOLD.r,GOLD.g,GOLD.b,30);
    SDL_RenderDrawLine(gren,0,sy,WIN_W,sy);
    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);
    dt(stmsg,PAD,sy+7,fSM,MUTED);

    /* ── ORDER NOTIFICATION ── */
    pthread_mutex_lock(&notif_mtx);
    if(notif.on){
        Uint32 age=SDL_GetTicks()-notif.at;
        if(age>10000){ notif.on=0; }
        else {
            Uint8 alpha=age>8000?(Uint8)(255-(age-8000)*255/2000):255;
            int nw=290,nh=128;
            SDL_Rect nr={WIN_W-nw-10,WIN_H-STAT_H-nh-10,nw,nh};

            SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(gren,20,20,20,alpha); SDL_RenderFillRect(gren,&nr);
            SDL_SetRenderDrawColor(gren,37,175,85,alpha); SDL_RenderDrawRect(gren,&nr);
            SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);

            SDL_Color nc={240,234,214,alpha},gc={37,175,85,alpha},mc={105,105,105,alpha};
            dt("New Order!",nr.x+10,nr.y+8,fLG,gc);
            dt(notif.name[0]?notif.name:"Customer",nr.x+10,nr.y+34,fMD,nc);
            if(notif.city[0]) dt(notif.city,nr.x+10,nr.y+52,fSM,mc);
            char tot[48]; snprintf(tot,sizeof(tot),"Total: $%s",notif.total);
            dt(tot,nr.x+10,nr.y+68,fSM,mc);
            char ord[40]; snprintf(ord,sizeof(ord),"Order #%d today",notif.order_no);
            dt(ord,nr.x+10,nr.y+88,fSM,gc);
            SDL_Rect db={nr.x+nw-64,nr.y+nh-26,56,20};
            fr(db,C(45,45,45)); dtctr("dismiss",db,fSM,mc);
        }
    }
    pthread_mutex_unlock(&notif_mtx);

    /* ── BRANDS BUTTON (above left bar) ── */
    SDL_Color bbc = bp.open ? GOLD : CARD;
    SDL_Color bfc = bp.open ? BG   : MUTED;
    frgrad(btn_brands,bp.open?C(38,28,6):C(20,17,32),bp.open?C(26,18,4):C(14,12,24));
    dr(btn_brands,bp.open?GOLD:BDR);
    if(bp.open) glow(btn_brands,GOLD,2);
    shine(btn_brands);
    dtctr("✦ Manage Brands",btn_brands,fSM,bp.open?LGOLD:MUTED);

    /* ── BACKUP / RESTORE BUTTONS ── */
    btn(btn_backup, "Backup", C(10,28,10),GRN);
    btn(btn_restore,"Restore",C(30,15,5), GOLD);

    /* ── FILE BROWSER ── */
    if(fb.open) fb_draw();

    /* ── BRANDS PANEL ── */
    if(bp.open){
        int bpw=360, bph=440;
        bp.rect=(SDL_Rect){(WIN_W-bpw)/2,(WIN_H-bph)/2,bpw,bph};
        SDL_Rect r=bp.rect;

        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gren,0,0,0,160); SDL_RenderFillRect(gren,NULL);
        SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_NONE);

        fr(r,PANEL); dr(r,GOLD);
        fr((SDL_Rect){r.x,r.y,r.w,40},CARD); hl(r.x,r.x+r.w,r.y+40,BDR);
        dt("Marcas / Brands",r.x+PAD,r.y+10,fLG,GOLD);
        SDL_Rect xb={r.x+r.w-34,r.y+4,28,28};
        fr(xb,RED); dtctr("X",xb,fMD,TEXT);

        /* brand list */
        SDL_Rect listR={r.x,r.y+40,r.w,r.h-100};
        SDL_RenderSetClipRect(gren,&listR);
        for(int i=0;i<nb;i++){
            int iy=listR.y+i*32;
            if(iy+32<listR.y||iy>listR.y+listR.h) continue;
            SDL_Rect row={listR.x,iy,listR.w,32};
            fr(row,i==bp.hov?HOVB:PANEL);
            hl(listR.x,listR.x+listR.w,iy+31,BDR);
            SDL_Rect bnc={listR.x+PAD,iy,listR.w-96,32}; dtclip(brands[i],bnc,fMD,TEXT);
            SDL_Rect up ={r.x+r.w-90,iy+6,22,20};
            SDL_Rect dn ={r.x+r.w-64,iy+6,22,20};
            SDL_Rect del={r.x+r.w-38,iy+6,22,20};
            fr(up, i>0    ?C(28,40,60):C(18,18,24)); dtctr("^",up, fSM,i>0    ?BLUE:MUTED);
            fr(dn, i<nb-1 ?C(28,40,60):C(18,18,24)); dtctr("v",dn, fSM,i<nb-1 ?BLUE:MUTED);
            fr(del,C(50,18,18)); dtctr("x",del,fSM,RED);
        }
        SDL_RenderSetClipRect(gren,NULL);

        /* add brand input */
        int iny=r.y+r.h-56;
        hl(r.x,r.x+r.w,iny-4,BDR);
        SDL_Rect inr={r.x+PAD,iny,r.w-PAD*2-80,32};
        fr(inr,CARD); dr(inr,BDR);
        SDL_Rect clip2={inr.x+6,inr.y,inr.w-12,inr.h};
        if(bp.addlen) dtclip(bp.addbuf,clip2,fMD,TEXT);
        else dtclip("New brand name...",clip2,fMD,MUTED);
        SDL_Rect addb={r.x+r.w-74,iny,62,32};
        fr(addb,GRN); dtctr("Add",addb,fMD,BG);
    }

    /* ── HISTORY PANEL ── */
    if(hist.open) hist_draw();

    SDL_RenderPresent(gren);
}

/* ═══════════════════════════════════════════════════════════════
   File browser confirm
   ═══════════════════════════════════════════════════════════════ */
static void fb_confirm(void){
    if(fb.sel2<0) return;
    FBEntry *e=&fb.ents[fb.sel2];
    if(e->is_dir){ fb_go(e->path); return; }
    char rel[256];
    if(!img_copy(e->path,rel,sizeof(rel))){ set_st("ERROR: Could not copy image"); return; }
    strncpy(prods[sel].image,rel,255); img_load(rel);
    dirty=1; fb.open=0; set_st("Image set! Save HTML to keep it.");
}

/* ═══════════════════════════════════════════════════════════════
   Events
   ═══════════════════════════════════════════════════════════════ */
static int pin(SDL_Rect r,int x,int y){
    return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;
}

static void handle(SDL_Event *e){
    if(e->type==SDL_QUIT){ ws_running=0; SDL_Quit(); exit(0); }

    /* ── brands panel ── */
    if(bp.open){
        if(e->type==SDL_MOUSEMOTION){
            int mx=e->motion.x,my=e->motion.y; bp.hov=-1;
            SDL_Rect r=bp.rect;
            SDL_Rect listR={r.x,r.y+40,r.w,r.h-100};
            if(pin(listR,mx,my)){ int idx=(my-listR.y)/32; if(idx>=0&&idx<nb) bp.hov=idx; }
            return;
        }
        if(e->type==SDL_MOUSEBUTTONDOWN&&e->button.button==SDL_BUTTON_LEFT){
            int mx=e->button.x,my=e->button.y;
            SDL_Rect r=bp.rect;
            SDL_Rect xb={r.x+r.w-34,r.y+4,28,28};
            if(pin(xb,mx,my)){ bp.open=0; return; }
            /* delete brand */
            SDL_Rect listR={r.x,r.y+40,r.w,r.h-100};
            if(pin(listR,mx,my)){
                int idx=(my-listR.y)/32;
                if(idx>=0&&idx<nb){
                    SDL_Rect up ={r.x+r.w-90,r.y+40+idx*32+6,22,20};
                    SDL_Rect dn ={r.x+r.w-64,r.y+40+idx*32+6,22,20};
                    SDL_Rect del={r.x+r.w-38,r.y+40+idx*32+6,22,20};
                    if(pin(up,mx,my)&&idx>0){
                        char tmp[48]; memcpy(tmp,brands[idx],48);
                        memcpy(brands[idx],brands[idx-1],48);
                        memcpy(brands[idx-1],tmp,48);
                        dirty=1; return;
                    }
                    if(pin(dn,mx,my)&&idx<nb-1){
                        char tmp[48]; memcpy(tmp,brands[idx],48);
                        memcpy(brands[idx],brands[idx+1],48);
                        memcpy(brands[idx+1],tmp,48);
                        dirty=1; return;
                    }
                    if(pin(del,mx,my)){
                        memmove(&brands[idx],&brands[idx+1],(nb-idx-1)*sizeof(brands[0]));
                        nb--; dirty=1; return;
                    }
                    /* click on brand → set for current product */
                    if(sel>=0&&sel<np){
                        strncpy(prods[sel].brand,brands[idx],47);
                        tf_set(&tfs[FB_BRAND],prods[sel].brand);
                        dirty=1;
                    }
                }
                return;
            }
            /* add brand button */
            int iny=r.y+r.h-56;
            SDL_Rect addb={r.x+r.w-74,iny,62,32};
            if(pin(addb,mx,my)&&bp.addlen>0&&nb<MAX_BRANDS){
                strncpy(brands[nb],bp.addbuf,47); nb++;
                bp.addbuf[0]=0; bp.addlen=0; dirty=1; return;
            }
            return;
        }
        if(e->type==SDL_KEYDOWN){
            if(e->key.keysym.sym==SDLK_ESCAPE){ bp.open=0; return; }
            if(e->key.keysym.sym==SDLK_BACKSPACE&&bp.addlen>0){ bp.addbuf[--bp.addlen]=0; return; }
            if(e->key.keysym.sym==SDLK_RETURN&&bp.addlen>0&&nb<MAX_BRANDS){
                strncpy(brands[nb],bp.addbuf,47); nb++;
                bp.addbuf[0]=0; bp.addlen=0; dirty=1; return;
            }
            return;
        }
        if(e->type==SDL_TEXTINPUT&&bp.addlen<47){
            int sl=strlen(e->text.text);
            if(bp.addlen+sl<47){ memcpy(bp.addbuf+bp.addlen,e->text.text,sl); bp.addlen+=sl; bp.addbuf[bp.addlen]=0; }
            return;
        }
        return;
    }

    /* ── file browser ── */
    if(fb.open){
        if(e->type==SDL_MOUSEWHEEL){
            fb.scroll-=e->wheel.y*3;
            if(fb.scroll<0) fb.scroll=0;
            int mx=fb.cnt-fb_vis(); if(fb.scroll>mx&&mx>=0) fb.scroll=mx;
            return;
        }
        if(e->type==SDL_MOUSEMOTION){
            int mx=e->motion.x,my=e->motion.y; fb.hov=-1;
            if(pin(fb.list,mx,my)){
                int idx=fb.scroll+(my-fb.list.y)/FB_ROW;
                if(idx>=0&&idx<fb.cnt) fb.hov=idx;
            }
            return;
        }
        if(e->type==SDL_MOUSEBUTTONDOWN&&e->button.button==SDL_BUTTON_LEFT){
            int mx=e->button.x,my=e->button.y;
            SDL_Rect r=fb.rect;
            SDL_Rect xb={r.x+r.w-34,r.y+4,28,28};
            SDL_Rect ub={r.x+r.w-80,r.y+47,32,22};
            SDL_Rect hb2={r.x+r.w-44,r.y+47,34,22};
            int bby=r.y+r.h-43,bw=150,bh=34;
            SDL_Rect pbtn={r.x+PAD,bby,bw,bh};
            SDL_Rect cbtn={r.x+r.w-PAD-bw*2-8,bby,bw,bh};
            SDL_Rect obtn={r.x+r.w-PAD-bw,bby,bw,bh};

            if(pin(xb,mx,my)){ fb.open=0; return; }
            if(pin(ub,mx,my)){
                char par[512]; strncpy(par,fb.path,511);
                char *sl=strrchr(par,'/'); if(sl&&sl!=par){ *sl=0; fb_go(par); } return;
            }
            if(pin(hb2,mx,my)){ const char *h=getenv("HOME"); fb_go(h?h:"/home"); return; }
            if(pin(pbtn,mx,my)){ paste_clip(); return; }
            if(pin(cbtn,mx,my)){ fb.open=0; return; }
            if(pin(obtn,mx,my)){ fb_confirm(); return; }
            if(pin(fb.list,mx,my)){
                int idx=fb.scroll+(my-fb.list.y)/FB_ROW;
                if(idx>=0&&idx<fb.cnt){
                    FBEntry *fe=&fb.ents[idx];
                    if(fe->is_dir){ fb_go(fe->path); }
                    else {
                        static int li=-1; static Uint32 lt=0;
                        Uint32 now=SDL_GetTicks();
                        if(idx==li&&now-lt<400){ fb.sel2=idx; fb_confirm(); }
                        else fb.sel2=idx;
                        li=idx; lt=now;
                    }
                }
            }
            return;
        }
        if(e->type==SDL_KEYDOWN){
            if(e->key.keysym.sym==SDLK_ESCAPE) fb.open=0;
            if(e->key.keysym.sym==SDLK_RETURN) fb_confirm();
            return;
        }
        return;
    }

    /* ── history panel ── */
    if(hist.open){
        if(e->type==SDL_MOUSEWHEEL){
            hist.scroll-=e->wheel.y;
            if(hist.scroll<0) hist.scroll=0;
            return;
        }
        if(e->type==SDL_MOUSEBUTTONDOWN&&e->button.button==SDL_BUTTON_LEFT){
            int mx=e->button.x,my=e->button.y;
            SDL_Rect xb={hist.rect.x+hist.rect.w-32,hist.rect.y+6,26,26};
            if(!pin(hist.rect,mx,my)||pin(xb,mx,my)){ hist.open=0; return; }
            return;
        }
        if(e->type==SDL_KEYDOWN&&e->key.keysym.sym==SDLK_ESCAPE){ hist.open=0; return; }
        return;
    }

    /* ── mouse wheel: scroll product list ── */
    if(e->type==SDL_MOUSEWHEEL&&e->wheel.x==0){
        int mx2=0,my2=0; SDL_GetMouseState(&mx2,&my2);
        if(mx2<LIST_W){
            list_scroll-=e->wheel.y;
            if(list_scroll<0) list_scroll=0;
        }
        return;
    }

    /* ── mouse motion / drag ── */
    if(e->type==SDL_MOUSEMOTION){
        int mx=e->motion.x,my=e->motion.y; hov_item=-1;
        /* update drag */
        if(drag.held){
            if(!drag.active&&abs(my-drag.start_y)>8){
                drag.active=1;
            }
            if(drag.active){
                drag.cur_y=my;
                /* compute target position */
                int fi=(my-LIST_TOP)/ITEM_H+list_scroll;
                if(fi<0) fi=0; if(fi>=nf) fi=nf-1;
                drag.to_fi=fi;
            }
        }
        /* hover */
        for(int fi=0;fi<nf;fi++){
            SDL_Rect ir={0,LIST_TOP+(fi-list_scroll)*ITEM_H,LIST_W,ITEM_H};
            if(pin(ir,mx,my)){ hov_item=filt[fi]; break; }
        }
        return;
    }

    /* ── mouse button up (end drag) ── */
    if(e->type==SDL_MOUSEBUTTONUP&&e->button.button==SDL_BUTTON_LEFT){
        if(drag.held){
            if(drag.active&&drag.from_fi!=drag.to_fi&&nf>1){
                /* only reorder when no search filter (1:1 mapping to prods[]) */
                if(nf==np){
                    int from=drag.from_fi, to=drag.to_fi;
                    Prod tmp=prods[filt[from]];
                    if(from<to){
                        memmove(&prods[from],&prods[from+1],(to-from)*sizeof(Prod));
                    } else {
                        memmove(&prods[to+1],&prods[to],(from-to)*sizeof(Prod));
                    }
                    prods[to]=tmp;
                    /* keep sel pointing at same product */
                    if(sel==filt[from]) sel=to;
                    else if(sel>=to&&sel<from) sel++;
                    else if(sel>from&&sel<=to) sel--;
                    dirty=1;
                    apply_filter();
                }
            }
            drag.held=0; drag.active=0;
        }
        return;
    }

    /* ── mouse click ── */
    if(e->type==SDL_MOUSEBUTTONDOWN&&e->button.button==SDL_BUTTON_LEFT){
        int mx=e->button.x,my=e->button.y;

        /* dismiss notification */
        pthread_mutex_lock(&notif_mtx);
        if(notif.on){
            int nw=290,nh=128;
            SDL_Rect db={WIN_W-nw-10+nw-64, WIN_H-STAT_H-nh-10+nh-26, 56,20};
            if(pin(db,mx,my)){ notif.on=0; pthread_mutex_unlock(&notif_mtx); return; }
        }
        pthread_mutex_unlock(&notif_mtx);

        /* ── left panel overlay buttons (checked BEFORE list so they aren't blocked) ── */
        if(pin(btn_backup, mx,my)){ f2p(sel); do_backup();  return; }
        if(pin(btn_restore,mx,my)){ do_restore(); return; }
        if(pin(btn_brands, mx,my)){ bp.open=!bp.open; SDL_StartTextInput(); return; }

        /* search bar click */
        if(pin(search_rect,mx,my)){
            /* clear X button */
            if(search_buf[0]){
                SDL_Rect clx={search_rect.x+search_rect.w-20,search_rect.y+6,14,16};
                if(pin(clx,mx,my)){ search_buf[0]=0; search_len=0; apply_filter(); return; }
            }
            search_active=1; tf_off(); SDL_StartTextInput(); return;
        }
        search_active=0;

        /* product list (filtered) — exclude the overlay button area at bottom */
        int bary=WIN_H-BAR_H-STAT_H;
        SDL_Rect lzone={0,LIST_TOP,LIST_W,bary-LIST_TOP-90};
        if(pin(lzone,mx,my)){
            for(int fi=0;fi<nf;fi++){
                SDL_Rect ir={0,LIST_TOP+(fi-list_scroll)*ITEM_H,LIST_W,ITEM_H};
                if(pin(ir,mx,my)){
                    /* per-row delete × */
                    SDL_Rect xr={LIST_W-24,ir.y+(ITEM_H-18)/2,18,18};
                    if(pin(xr,mx,my)){
                        int pi=filt[fi];
                        f2p(sel);
                        if(confirm_delete(prods[pi].name)) delete_at(pi);
                        return;
                    }
                    f2p(sel); sel=filt[fi]; p2f(sel); tf_off();
                    /* start drag tracking */
                    drag.held=1; drag.active=0;
                    drag.from_fi=fi; drag.to_fi=fi;
                    drag.start_y=my; drag.cur_y=my;
                    return;
                }
            }
        }

        /* add */
        if(pin(btn_add,mx,my)&&np<MAX_PRODS){
            f2p(sel);
            Prod *p=&prods[np]; memset(p,0,sizeof(Prod));
            p->id=nxtid++; strcpy(p->name,"New Perfume");
            strcpy(p->price,"0"); strcpy(p->size,"100ml"); strcpy(p->icon,"🌹");
            p->in_stock=1;
            sel=np++; p2f(sel); dirty=1; apply_filter(); return;
        }
        /* delete */
        if(pin(btn_del,mx,my)&&np>0){
            f2p(sel);
            if(confirm_delete(prods[sel].name)) delete_at(sel);
            return;
        }
        /* brand dropdown pick */
        if(brand_dd && nb>0){
            SDL_Rect bf = tfs[FB_BRAND].rect;
            int ddw = bf.w < 160 ? 160 : bf.w;
            int ddx = bf.x + bf.w - ddw;
            int ddy = bf.y + bf.h + 2;
            int rowh = 26;
            SDL_Rect bbox = {ddx, ddy, ddw, nb*rowh};
            if(pin(bbox,mx,my)){
                int idx = (my - ddy) / rowh;
                if(idx>=0 && idx<nb){
                    tf_set(&tfs[FB_BRAND], brands[idx]);
                    if(sel>=0&&sel<np){
                        strncpy(prods[sel].brand, brands[idx], 47);
                        dirty=1;
                    }
                }
                brand_dd=0; tf_off(); return;
            }
            brand_dd=0;
        }
        /* text fields */
        for(int i=0;i<NF;i++){
            if(pin(tfs[i].rect,mx,my)){ f2p(sel); tf_on(i); return; }
        }
        /* in_stock toggle */
        if(pin(btn_instock,mx,my)&&np>0){
            f2p(sel); prods[sel].in_stock^=1; dirty=1; return;
        }
        /* browse */
        if(pin(btn_browse,mx,my)){ f2p(sel); tf_off(); fb_open_browser(); return; }
        /* paste */
        if(pin(btn_paste,mx,my)){ f2p(sel); tf_off(); paste_clip(); return; }
        if(pin(btn_wssync, mx,my)){
            /* refuse sync if there are uncommitted files — images not on GitHub yet */
            char chk[256];
            snprintf(chk,sizeof(chk),"cd %s && git status --porcelain 2>/dev/null | head -1",GIT_DIR);
            FILE *gf=popen(chk,"r"); char gline[64]={0};
            if(gf){ fgets(gline,sizeof(gline),gf); pclose(gf); }
            if(gline[0]){
                set_st("Uncommitted changes — use Push first to upload images, then Sync");
                return;
            }
            f2p(sel); firebase_sync(); return;
        }
        if(pin(btn_reconnect,mx,my)){ ws_force_reconnect=1; set_st("Reconnecting..."); return; }
        /* history */
        if(pin(btn_history,mx,my)){ hist.open=!hist.open; if(hist.open) load_hist(); return; }
        /* save */
        if(pin(btn_save,mx,my)){ f2p(sel); html_save(); return; }
        /* push */
        if(pin(btn_push,mx,my)){ f2p(sel); html_save(); git_push(); return; }

        tf_off(); return;
    }

    /* ── key ── */
    if(e->type==SDL_KEYDOWN){
        SDL_Keycode k=e->key.keysym.sym;

        /* search active: handle input */
        if(search_active){
            if(k==SDLK_ESCAPE||k==SDLK_RETURN){ search_active=0; SDL_StopTextInput(); return; }
            if(k==SDLK_BACKSPACE){
                if(search_len>0){
                    /* handle multi-byte UTF-8 backspace */
                    while(search_len>0&&(search_buf[search_len-1]&0xC0)==0x80) search_len--;
                    if(search_len>0) search_len--;
                    search_buf[search_len]=0; apply_filter();
                }
                return;
            }
            return;
        }

        if(k==SDLK_ESCAPE&&hist.open){ hist.open=0; return; }
        if(atf<0) return;
        TF *t=&tfs[atf];
        if(k==SDLK_BACKSPACE){ tf_bs(t); f2p(sel); }
        else if(k==SDLK_LEFT){
            if(e->key.keysym.mod&KMOD_SHIFT){
                if(t->sel<0) t->sel=t->cur;  /* start selection */
                if(t->cur>0) t->cur--;
                if(t->sel==t->cur) t->sel=-1; /* collapsed */
            } else { t->sel=-1; if(t->cur>0) t->cur--; }
        }
        else if(k==SDLK_RIGHT){
            if(e->key.keysym.mod&KMOD_SHIFT){
                if(t->sel<0) t->sel=t->cur;
                if(t->cur<t->len) t->cur++;
                if(t->sel==t->cur) t->sel=-1;
            } else { t->sel=-1; if(t->cur<t->len) t->cur++; }
        }
        else if(k==SDLK_HOME){
            if(e->key.keysym.mod&KMOD_SHIFT){ if(t->sel<0)t->sel=t->cur; t->cur=0; if(t->sel==t->cur)t->sel=-1; }
            else { t->sel=-1; t->cur=0; }
        }
        else if(k==SDLK_END){
            if(e->key.keysym.mod&KMOD_SHIFT){ if(t->sel<0)t->sel=t->cur; t->cur=t->len; if(t->sel==t->cur)t->sel=-1; }
            else { t->sel=-1; t->cur=t->len; }
        }
        else if(k==SDLK_a&&(e->key.keysym.mod&KMOD_CTRL)){ t->sel=0; t->cur=t->len; }
        else if(k==SDLK_c&&(e->key.keysym.mod&KMOD_CTRL)){ tf_copy(t); }
        else if(k==SDLK_x&&(e->key.keysym.mod&KMOD_CTRL)){ tf_cut(t); f2p(sel); }
        else if(k==SDLK_v&&(e->key.keysym.mod&KMOD_CTRL)){ tf_paste(t); f2p(sel); }
        else if(k==SDLK_s&&(e->key.keysym.mod&KMOD_CTRL)){ f2p(sel); html_save(); }
        else if(k==SDLK_TAB){ f2p(sel); tf_on((atf+1)%NF); }
        else if(k==SDLK_RETURN||k==SDLK_ESCAPE) tf_off();
        return;
    }

    /* ── text input ── */
    if(e->type==SDL_TEXTINPUT){
        if(search_active){
            int sl=strlen(e->text.text);
            if(search_len+sl<(int)sizeof(search_buf)-1){
                memcpy(search_buf+search_len,e->text.text,sl);
                search_len+=sl; search_buf[search_len]=0;
                apply_filter();
            }
            return;
        }
        if(atf>=0){ tf_ins(&tfs[atf],e->text.text); f2p(sel); }
        return;
    }
}

/* ═══════════════════════════════════════════════════════════════
   Process WS events on main thread
   ═══════════════════════════════════════════════════════════════ */
static void ws_poll(void){
    WEvent ev;
    while(wev_pop(&ev)){
        switch(ev.kind){
            case WEV_CONNECTED:    set_st("WebSocket connected — authenticating..."); break;
            case WEV_DISCONNECTED: set_st("WebSocket offline — reconnecting in 5s..."); break;
            case WEV_SYNCED:       set_st("Live! Products synced to website."); break;
            case WEV_ORDER:        save_order(); set_st("New order received!"); break;
        }
    }
}

/* Set _NET_WM_ICON via X11 directly (SDL_SetWindowIcon doesn't always
   propagate to the taskbar on all compositors). */
static void set_x11_icon(SDL_Window *win){
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if(!SDL_GetWindowWMInfo(win,&wm)||wm.subsystem!=SDL_SYSWM_X11) return;
    SDL_RWops *rw=SDL_RWFromConstMem(icon_png,icon_png_len);
    SDL_Surface *s=IMG_Load_RW(rw,1);
    if(!s) return;
    /* Convert to ARGB8888 so we can read pixels */
    SDL_Surface *a=SDL_ConvertSurfaceFormat(s,SDL_PIXELFORMAT_ARGB8888,0);
    SDL_FreeSurface(s);
    if(!a) return;
    int w=a->w, h=a->h;
    unsigned long *data=malloc((2+w*h)*sizeof(unsigned long));
    if(data){
        data[0]=(unsigned long)w; data[1]=(unsigned long)h;
        Uint32 *px=(Uint32*)a->pixels;
        for(int i=0;i<w*h;i++) data[2+i]=(unsigned long)px[i];
        Display *dpy=wm.info.x11.display;
        Window   xw =wm.info.x11.window;
        Atom prop=XInternAtom(dpy,"_NET_WM_ICON",False);
        XChangeProperty(dpy,xw,prop,XA_CARDINAL,32,PropModeReplace,
                        (unsigned char*)data,(2+w*h));
        XFlush(dpy);
        free(data);
    }
    SDL_FreeSurface(a);
}

/* ═══════════════════════════════════════════════════════════════
   main
   ═══════════════════════════════════════════════════════════════ */
int main(void){
    /* SDL hints for smoother rendering */
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

    if(SDL_Init(SDL_INIT_VIDEO)<0){
        fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1;
    }
    TTF_Init();
    IMG_Init(IMG_INIT_JPG|IMG_INIT_PNG|IMG_INIT_WEBP);

    gwin=SDL_CreateWindow("Perfume Store — Product Editor",
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        WIN_W,WIN_H, SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
    if(!gwin){ fprintf(stderr,"Window: %s\n",SDL_GetError()); return 1; }

    set_x11_icon(gwin);

    gren=SDL_CreateRenderer(gwin,-1,
        SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!gren){ fprintf(stderr,"Renderer: %s\n",SDL_GetError()); return 1; }

    SDL_SetRenderDrawBlendMode(gren,SDL_BLENDMODE_BLEND);

    fMD=TTF_OpenFont(FONT_PATH,14);
    fSM=TTF_OpenFont(FONT_PATH,11);
    fLG=TTF_OpenFont(FONT_PATH,18);
    if(!fMD||!fSM||!fLG){
        fprintf(stderr,"Font: %s\n",TTF_GetError()); return 1;
    }

    /* ── init WebSocket in background thread ── */
    lws_set_log_level(0,NULL);
    struct lws_context_creation_info info; memset(&info,0,sizeof(info));
    info.port      = CONTEXT_PORT_NO_LISTEN;
    info.protocols = ws_protos;
    info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    ws_ctx = lws_create_context(&info);

    pthread_t wt;
    pthread_create(&wt,NULL,ws_thread,NULL);
    pthread_detach(wt);

    /* ── pull latest from GitHub before loading ── */
    system("cd /home/xmm/Projects/perfume-store && git fetch origin && git checkout origin/main -- index.html data.json 2>/dev/null");

    /* ── layout + load ── */
    layout();
    html_load();
    data_json_fill_images();   /* index.html has image:null after Android pushes */
    if(np>0) p2f(0);

    /* ── main loop ── */
    SDL_Event ev;
    while(1){
        while(SDL_PollEvent(&ev)) handle(&ev);
        ws_poll();
        render();
    }

    ws_running=0;
    return 0;
}
