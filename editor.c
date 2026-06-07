/*
 * Perfume Store Product Editor — full rewrite
 * WebSocket runs in a background thread; SDL loop never blocks.
 * Build: make
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Paths ─────────────────────────────────────────────────── */
#define HTML_PATH  "/home/xmm/Projects/perfume-store/index.html"
#define IMG_DIR    "/home/xmm/Projects/perfume-store/images"
#define GIT_DIR    "/home/xmm/Projects/perfume-store"
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

/* ── Limits ─────────────────────────────────────────────────── */
#define MAX_PRODS  32
#define BUF_MAX    (1<<21)   /* 2 MB */
#define FB_MAX     1024
#define FB_ROW      28

/* ── Colors ─────────────────────────────────────────────────── */
#define C(r,g,b)  ((SDL_Color){r,g,b,255})
static const SDL_Color
    BG    = C( 12, 12, 12),
    PANEL = C( 20, 20, 20),
    CARD  = C( 26, 26, 26),
    BDR   = C( 42, 42, 42),
    GOLD  = C(201,168, 76),
    LGOLD = C(228,198,118),
    TEXT  = C(240,234,214),
    MUTED = C(105,105,105),
    SELB  = C( 38, 30,  8),
    HOVB  = C( 26, 22,  6),
    GRN   = C( 37,175, 85),
    RED   = C(160, 48, 48),
    BLUE  = C( 60,110,200),
    FBHOV = C( 28, 36, 50),
    FBSEL = C( 38, 58, 90);

/* ── Product ─────────────────────────────────────────────────── */
typedef struct {
    int  id;
    char name[64];
    char desc[128];
    char price[20];
    char size[16];
    char badge[32];
    char icon[16];
    char image[256];
    char brand[48];
} Prod;

/* ── Brands ──────────────────────────────────────────────────── */
#define MAX_BRANDS 24
static char brands[MAX_BRANDS][48];
static int  nb = 0;

/* ── Text field ──────────────────────────────────────────────── */
typedef struct {
    char     buf[256];
    int      len, cur, active;
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
static char       wsq_buf[WSQ_CAP][8192];
static int        wsq_head = 0, wsq_tail = 0;
static pthread_mutex_t wsq_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct lws_context *ws_ctx = NULL;
static struct lws         *ws_wsi = NULL;

static void wsq_push(const char *json) {
    pthread_mutex_lock(&wsq_mtx);
    int next = (wsq_tail + 1) % WSQ_CAP;
    if (next != wsq_head) {
        strncpy(wsq_buf[wsq_tail], json, 8191);
        wsq_tail = next;
    }
    pthread_mutex_unlock(&wsq_mtx);
    if (ws_ctx) lws_cancel_service(ws_ctx);
}

/* ── WS state (atomic-ish, read from main thread) ───────────── */
typedef enum { WS_OFF=0, WS_CONN, WS_READY } WsState;
static volatile WsState ws_state  = WS_OFF;
static volatile int     ws_orders = 0;
static volatile int     ws_running = 1;

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

enum { FN=0, FD, FP, FS, FB_F, FB_BRAND, NF };
static TF   tfs[NF];
static int  atf = -1;

static SDL_Texture *imgtex = NULL;
static int imgw = 0, imgh = 0;

static char   stmsg[256] = "Loading...";

static SDL_Rect
    btn_add, btn_del,
    btn_browse, btn_paste,
    btn_save, btn_push, btn_wssync, btn_brands;

static int hov_item = -1;
static int dirty    = 0;

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

static void btn(SDL_Rect r,const char *lbl,SDL_Color bg,SDL_Color fg){
    fr(r,bg); dr(r,BDR); dtctr(lbl,r,fMD,fg);
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
        ws_wsi   = wsi;
        ws_state = WS_CONN;
        wev_push(WEV_CONNECTED, NULL);
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        /* send auth first, then any queued messages */
        static int authed = 0;
        char msg[8192+LWS_PRE];
        int  mlen = 0;

        pthread_mutex_lock(&wsq_mtx);
        if(!authed){
            mlen = snprintf(msg+LWS_PRE, sizeof(msg)-LWS_PRE,
                "{\"type\":\"admin_auth\",\"secret\":\"%s\"}", WS_SECRET);
            authed = 1;
        } else if(wsq_head != wsq_tail){
            mlen = (int)strlen(wsq_buf[wsq_head]);
            memcpy(msg+LWS_PRE, wsq_buf[wsq_head], mlen);
            wsq_head = (wsq_head+1) % WSQ_CAP;
        }
        int more = (wsq_head != wsq_tail);
        pthread_mutex_unlock(&wsq_mtx);

        if(mlen>0)
            lws_write(wsi,(unsigned char*)(msg+LWS_PRE),mlen,LWS_WRITE_TEXT);
        if(more)
            lws_callback_on_writable(wsi);
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
    static int authed_flag = 0;   /* reset on each reconnect */

    while(ws_running){
        if(ws_ctx) lws_service(ws_ctx, 50);

        /* reconnect logic */
        Uint32 now = SDL_GetTicks();
        if(ws_state==WS_OFF && now-last_try > 5000){
            last_try = now;
            authed_flag = 0;

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

static void ws_send_products(void){
    if(ws_state != WS_READY){ set_st("WebSocket not connected yet"); return; }
    char json[8192];
    int  pos = 0;
    pos += snprintf(json+pos, sizeof(json)-pos,
        "{\"type\":\"update_products\",\"secret\":\"%s\",\"products\":[", WS_SECRET);
    for(int i=0;i<np&&pos<(int)sizeof(json)-512;i++){
        Prod *p=&prods[i];
        char bj[48],ij[280];
        if(p->badge[0]) snprintf(bj,sizeof(bj),"\"%s\"",p->badge); else strcpy(bj,"null");
        if(p->image[0]) snprintf(ij,sizeof(ij),"\"%s\"",p->image); else strcpy(ij,"null");
        pos += snprintf(json+pos, sizeof(json)-pos,
            "{\"id\":%d,\"name\":\"%s\",\"desc\":\"%s\","
            "\"price\":%s,\"size\":\"%s\",\"icon\":\"%s\","
            "\"badge\":%s,\"image\":%s}%s",
            p->id,p->name,p->desc,
            p->price[0]?p->price:"0",
            p->size,p->icon[0]?p->icon:"🌺",
            bj,ij, i<np-1?",":"");
    }
    snprintf(json+pos, sizeof(json)-pos,"]}");
    wsq_push(json);
    set_st("Syncing products to live website...");
}

/* ═══════════════════════════════════════════════════════════════
   Image
   ═══════════════════════════════════════════════════════════════ */
static void img_free(void){
    if(imgtex){ SDL_DestroyTexture(imgtex); imgtex=NULL; }
}
static void img_load(const char *path){
    img_free();
    if(!path||!path[0]) return;
    SDL_Surface *s=IMG_Load(path);
    if(!s){ char full[512]; snprintf(full,sizeof(full),"%s/%s",GIT_DIR,path); s=IMG_Load(full); }
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
        exfield(blk,"name", pr->name, sizeof(pr->name));
        exfield(blk,"desc", pr->desc, sizeof(pr->desc));
        exfield(blk,"price",pr->price,sizeof(pr->price));
        exfield(blk,"size", pr->size, sizeof(pr->size));
        exfield(blk,"badge",pr->badge,sizeof(pr->badge));
        exfield(blk,"icon", pr->icon, sizeof(pr->icon));
        exfield(blk,"image",pr->image,sizeof(pr->image));
        exfield(blk,"brand",pr->brand,sizeof(pr->brand));
        if(!strcmp(pr->badge,"null")) pr->badge[0]=0;
        if(!strcmp(pr->image,"null")) pr->image[0]=0;
        if(!strcmp(pr->brand,"null")) pr->brand[0]=0;
        if(pr->id>=nxtid) nxtid=pr->id+1;
        if(pr->name[0]) np++;
        free(blk); p=cb;
    }
    free(buf);
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
        char bj[80],ij[300],brj[80];
        if(p->badge[0]) snprintf(bj,sizeof(bj),"\"%s\"",p->badge); else strcpy(bj,"null");
        if(p->image[0]) snprintf(ij,sizeof(ij),"\"%s\"",p->image); else strcpy(ij,"null");
        if(p->brand[0]) snprintf(brj,sizeof(brj),"\"%s\"",p->brand); else strcpy(brj,"null");
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
            "      brand: %s\n"
            "    }%s\n",
            p->id,p->name,p->desc,
            p->price[0]?p->price:"0",
            p->size,p->icon[0]?p->icon:"🌹",
            bj,ij,brj, i<np-1?",":"");
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
        bp2+=snprintf(brnjs+bp2,2047-bp2,"  ]");

        int pre=(int)(brands_start-buf);
        int after_end=(int)strlen(brands_end+strlen("\n  ];")); /* rest after brands ] */
        /* reconstruct buf with new brands */
        char *tmp2=malloc(BUF_MAX*2);
        memcpy(tmp2,buf,pre);
        memcpy(tmp2+pre,brnjs,bp2);
        strcpy(tmp2+pre+bp2,brands_end+strlen("\n  ];")-1);
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
   Git push
   ═══════════════════════════════════════════════════════════════ */
static void git_push(void){
    set_st("Pushing to GitHub...");
    char cmd[512];
    snprintf(cmd,sizeof(cmd),
        "cd %s && git add -A && git commit -m 'Update products' && git push 2>&1",GIT_DIR);
    FILE *fp=popen(cmd,"r"); if(!fp){ set_st("ERROR: popen failed"); return; }
    char line[256],last[256]={0};
    while(fgets(line,sizeof(line),fp)) strncpy(last,line,255);
    pclose(fp);
    int l=strlen(last); while(l>0&&(last[l-1]=='\n'||last[l-1]=='\r')) last[--l]=0;
    set_st(last[0]?last:"Pushed to GitHub!");
}

/* ═══════════════════════════════════════════════════════════════
   Product <-> form
   ═══════════════════════════════════════════════════════════════ */
static void tf_set(TF *t,const char *s){
    strncpy(t->buf,s?s:"",255); t->buf[255]=0;
    t->len=strlen(t->buf); t->cur=t->len;
}
static void p2f(int i){
    if(i<0||i>=np) return;
    Prod *p=&prods[i];
    tf_set(&tfs[FN],p->name); tf_set(&tfs[FD],p->desc);
    tf_set(&tfs[FP],p->price);tf_set(&tfs[FS],p->size);
    tf_set(&tfs[FB_F],p->badge); tf_set(&tfs[FB_BRAND],p->brand);
    img_load(p->image);
}
static void f2p(int i){
    if(i<0||i>=np) return;
    Prod *p=&prods[i];
    strncpy(p->name, tfs[FN].buf, 63);
    strncpy(p->desc, tfs[FD].buf,127);
    strncpy(p->price,tfs[FP].buf, 19);
    strncpy(p->size, tfs[FS].buf, 15);
    strncpy(p->badge,tfs[FB_F].buf,31);
    strncpy(p->brand,tfs[FB_BRAND].buf,47);
    dirty=1;
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
    /* try X clipboard image */
    const char *tmp="/tmp/_cb.png";
    if(!system("xclip -selection clipboard -t image/png -o > /tmp/_cb.png 2>/dev/null")){
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
   Layout
   ═══════════════════════════════════════════════════════════════ */
static void layout(void){
    int ry=WIN_H-BAR_H-STAT_H;
    int rx=LIST_W+1, rw=WIN_W-rx;

    /* left panel buttons */
    int bw=(LIST_W-PAD*2-8)/2;
    btn_add   =(SDL_Rect){PAD,     ry+(BAR_H-34)/2, bw,   34};
    btn_del   =(SDL_Rect){PAD+bw+8,ry+(BAR_H-34)/2, bw,   34};
    btn_brands=(SDL_Rect){PAD,     ry-42,           LIST_W-PAD*2, 28};

    /* right panel bottom buttons */
    int pw=138;
    btn_wssync=(SDL_Rect){rx+PAD,              ry+(BAR_H-34)/2, pw,   34};
    btn_save  =(SDL_Rect){WIN_W-PAD-pw*2-8,    ry+(BAR_H-34)/2, pw,   34};
    btn_push  =(SDL_Rect){WIN_W-PAD-pw,         ry+(BAR_H-34)/2, pw,   34};

    /* text fields */
    int fx=rx+PAD, fw=rw-PAD*2;
    int fy=56, gap=TF_H+26;

    tfs[FN].rect=(SDL_Rect){fx,         fy,       fw,      TF_H};
    tfs[FD].rect=(SDL_Rect){fx,         fy+gap,   fw,      TF_H};
    tfs[FP].rect=(SDL_Rect){fx,         fy+gap*2, fw/4-4,  TF_H};
    tfs[FS].rect=(SDL_Rect){fx+fw/4+4,  fy+gap*2, fw/4-4,  TF_H};
    tfs[FB_F].rect=(SDL_Rect){fx+fw/2+4,fy+gap*2, fw/4-4,  TF_H};
    tfs[FB_BRAND].rect=(SDL_Rect){fx+fw*3/4+6,fy+gap*2, fw/4-6, TF_H};

    strcpy(tfs[FN].label,"Name");
    strcpy(tfs[FN].hint,"e.g. Rose Oud");
    strcpy(tfs[FD].label,"Description");
    strcpy(tfs[FD].hint,"Short tagline");
    strcpy(tfs[FP].label,"Price");
    strcpy(tfs[FP].hint,"130000");
    strcpy(tfs[FS].label,"Size");
    strcpy(tfs[FS].hint,"100ml");
    strcpy(tfs[FB_F].label,"Badge");
    strcpy(tfs[FB_F].hint,"Nuevo");
    strcpy(tfs[FB_BRAND].label,"Brand");
    strcpy(tfs[FB_BRAND].hint,"Lattafa");

    int img_y=fy+gap*3+8;
    btn_browse=(SDL_Rect){fx,       img_y,160,34};
    btn_paste =(SDL_Rect){fx+168,   img_y,160,34};
}

/* ═══════════════════════════════════════════════════════════════
   Text field
   ═══════════════════════════════════════════════════════════════ */
static void tf_draw(TF *t,int active){
    fr(t->rect,CARD); dr(t->rect,active?GOLD:BDR);
    SDL_Rect clip={t->rect.x+8,t->rect.y,t->rect.w-16,t->rect.h};
    if(t->len) dtclip(t->buf,clip,fMD,TEXT); else dtclip(t->hint,clip,fMD,MUTED);
    if(active&&(SDL_GetTicks()/530)%2==0){
        int cx=t->rect.x+8;
        if(t->cur>0){ char tmp[256]; int n=t->cur<t->len?t->cur:t->len;
            memcpy(tmp,t->buf,n); tmp[n]=0;
            int tw,th; TTF_SizeUTF8(fMD,tmp,&tw,&th); cx+=tw; }
        sc(GOLD); SDL_RenderDrawLine(gren,cx,t->rect.y+5,cx,t->rect.y+t->rect.h-5);
    }
}
static void tf_on(int idx){
    if(atf>=0&&atf<NF) tfs[atf].active=0;
    if(idx>=0&&idx<NF){ tfs[idx].active=1; tfs[idx].cur=tfs[idx].len; }
    atf=idx; SDL_StartTextInput();
}
static void tf_off(void){
    if(atf>=0&&atf<NF) tfs[atf].active=0;
    atf=-1; SDL_StopTextInput();
}
static void tf_ins(TF *t,const char *s){
    int sl=strlen(s); if(t->len+sl>=(int)sizeof(t->buf)) return;
    memmove(t->buf+t->cur+sl,t->buf+t->cur,t->len-t->cur+1);
    memcpy(t->buf+t->cur,s,sl); t->len+=sl; t->cur+=sl;
}
static void tf_bs(TF *t){
    if(!t->cur) return;
    memmove(t->buf+t->cur-1,t->buf+t->cur,t->len-t->cur+1);
    t->len--; t->cur--;
}
static void tf_clr(TF *t){ t->buf[0]=0; t->len=t->cur=0; }

/* ═══════════════════════════════════════════════════════════════
   Render
   ═══════════════════════════════════════════════════════════════ */
static void render(void){
    sc(BG); SDL_RenderClear(gren);

    int bary=WIN_H-BAR_H-STAT_H;
    int rx=LIST_W+1;

    /* ── LEFT PANEL ── */
    fr((SDL_Rect){0,0,LIST_W,bary},PANEL);
    fr((SDL_Rect){0,0,LIST_W,42},CARD);
    dt("Products",PAD,12,fLG,GOLD);
    hl(0,LIST_W,42,BDR);

    /* product list — scrollable */
    int vis_items=(bary-42)/ITEM_H;
    int max_scroll=np-vis_items; if(max_scroll<0) max_scroll=0;
    if(list_scroll>max_scroll) list_scroll=max_scroll;

    SDL_RenderSetClipRect(gren,&(SDL_Rect){0,42,LIST_W,bary-42});
    for(int i=0;i<np;i++){
        int iy=42+(i-list_scroll)*ITEM_H;
        if(iy+ITEM_H<42||iy>bary) continue;
        SDL_Rect ir={0,iy,LIST_W,ITEM_H};
        SDL_Color bg=(i==sel)?SELB:(i==hov_item)?HOVB:PANEL;
        fr(ir,bg); hl(0,LIST_W,iy+ITEM_H-1,BDR);
        if(i==sel) fr((SDL_Rect){0,iy,3,ITEM_H},GOLD);
        SDL_Rect nc={PAD+4,iy+8,LIST_W-PAD*2,18};
        dtclip(prods[i].name[0]?prods[i].name:"(unnamed)",nc,fMD,(i==sel)?LGOLD:TEXT);
        char pt[32]; snprintf(pt,sizeof(pt),"$%s / %s",prods[i].price,prods[i].size);
        SDL_Rect pc={PAD+4,iy+28,LIST_W-PAD*2,14}; dtclip(pt,pc,fSM,MUTED);
    }
    SDL_RenderSetClipRect(gren,NULL);

    /* scrollbar for product list */
    if(np>vis_items){
        int sbh=bary-42, sby=42;
        int th=sbh*vis_items/np;
        int ty=sby+(list_scroll*(sbh-th))/(np-vis_items);
        fr((SDL_Rect){LIST_W-5,sby,4,sbh},CARD);
        fr((SDL_Rect){LIST_W-5,ty,4,th},MUTED);
    }

    /* left bar */
    fr((SDL_Rect){0,bary,LIST_W,BAR_H},PANEL);
    hl(0,LIST_W,bary,BDR);
    btn(btn_add,"+ Add",   C(18,38,14),GRN);
    btn(btn_del,"Delete",  C(38,14,14),RED);

    /* ── DIVIDER ── */
    fr((SDL_Rect){LIST_W,0,1,WIN_H},BDR);

    /* ── RIGHT PANEL HEADER ── */
    fr((SDL_Rect){rx,0,WIN_W-rx,42},CARD);
    hl(rx,WIN_W,42,BDR);

    if(np==0){
        char htitle[64]="Editing: —";
        dt(htitle,rx+PAD,12,fLG,MUTED);
    } else {
        char htitle[80]; snprintf(htitle,sizeof(htitle),"Editing: %s",
            prods[sel].name[0]?prods[sel].name:"(new)");
        dt(htitle,rx+PAD,12,fLG,TEXT);
    }

    /* WS status (top-right) */
    SDL_Color dc = ws_state==WS_READY?GRN : ws_state==WS_CONN?C(200,160,30):RED;
    fr((SDL_Rect){WIN_W-136,16,10,10},dc);
    const char *wsl = ws_state==WS_READY?"Live" : ws_state==WS_CONN?"Connecting...":"Offline";
    dt(wsl,WIN_W-122,12,fSM,dc);
    char oc[32]; snprintf(oc,sizeof(oc),"Orders: %d",ws_orders);
    dt(oc,WIN_W-122,24,fSM,MUTED);

    /* ── EDIT FORM ── */
    if(np>0){
        for(int i=0;i<NF;i++){
            dt(tfs[i].label,tfs[i].rect.x,tfs[i].rect.y-16,fSM,MUTED);
            tf_draw(&tfs[i],atf==i);
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
    fr((SDL_Rect){rx,bary,WIN_W-rx,BAR_H},CARD);
    hl(rx,WIN_W,bary,BDR);

    SDL_Color wbc=ws_state==WS_READY?C(18,45,65):C(28,28,28);
    SDL_Color wfc=ws_state==WS_READY?TEXT:MUTED;
    btn(btn_wssync,"Sync Live",wbc,wfc);
    btn(btn_save,  "Save HTML",GRN,C(8,8,8));
    btn(btn_push,  "Push GitHub",C(20,40,20),GRN);
    if(dirty) dt("*",btn_save.x-14,btn_save.y+10,fMD,GOLD);

    /* ── STATUS BAR ── */
    int sy=WIN_H-STAT_H;
    fr((SDL_Rect){0,sy,WIN_W,STAT_H},C(10,10,10));
    hl(0,WIN_W,sy,BDR);
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
    fr(btn_brands,bbc); dr(btn_brands,bp.open?GOLD:BDR);
    dtctr("Manage Brands",btn_brands,fSM,bfc);

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
            SDL_Rect bnc={listR.x+PAD,iy,listR.w-48,32}; dtclip(brands[i],bnc,fMD,TEXT);
            SDL_Rect del={r.x+r.w-38,iy+6,22,20};
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
                    SDL_Rect del={r.x+r.w-38,r.y+40+idx*32+6,22,20};
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

    /* ── mouse wheel: scroll product list ── */
    if(e->type==SDL_MOUSEWHEEL&&e->wheel.x==0){
        if(e->motion.x<LIST_W||!e->motion.x){
            list_scroll-=e->wheel.y;
            if(list_scroll<0) list_scroll=0;
        }
        return;
    }

    /* ── mouse motion ── */
    if(e->type==SDL_MOUSEMOTION){
        int mx=e->motion.x,my=e->motion.y; hov_item=-1;
        for(int i=0;i<np;i++){
            SDL_Rect ir={0,42+(i-list_scroll)*ITEM_H,LIST_W,ITEM_H};
            if(pin(ir,mx,my)){ hov_item=i; break; }
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

        /* product list */
        for(int i=0;i<np;i++){
            SDL_Rect ir={0,42+(i-list_scroll)*ITEM_H,LIST_W,ITEM_H};
            if(pin(ir,mx,my)){
                f2p(sel); sel=i; p2f(sel); tf_off(); return;
            }
        }

        /* add */
        if(pin(btn_add,mx,my)&&np<MAX_PRODS){
            f2p(sel);
            Prod *p=&prods[np]; memset(p,0,sizeof(Prod));
            p->id=nxtid++; strcpy(p->name,"New Perfume");
            strcpy(p->price,"0"); strcpy(p->size,"100ml"); strcpy(p->icon,"🌹");
            sel=np++; p2f(sel); dirty=1; return;
        }
        /* delete */
        if(pin(btn_del,mx,my)&&np>0){
            f2p(sel);
            memmove(&prods[sel],&prods[sel+1],(np-sel-1)*sizeof(Prod));
            np--; if(sel>=np&&sel>0) sel--;
            if(np>0) p2f(sel); else img_free(); dirty=1; return;
        }
        /* text fields */
        for(int i=0;i<NF;i++){
            if(pin(tfs[i].rect,mx,my)){ f2p(sel); tf_on(i); return; }
        }
        /* browse */
        if(pin(btn_browse,mx,my)){ f2p(sel); tf_off(); fb_open_browser(); return; }
        /* paste */
        if(pin(btn_paste,mx,my)){ f2p(sel); tf_off(); paste_clip(); return; }
        /* WS sync */
        if(pin(btn_brands,mx,my)){ bp.open=!bp.open; SDL_StartTextInput(); return; }
        if(pin(btn_wssync,mx,my)){ f2p(sel); ws_send_products(); return; }
        /* save */
        if(pin(btn_save,mx,my)){ f2p(sel); html_save(); return; }
        /* push */
        if(pin(btn_push,mx,my)){ f2p(sel); html_save(); git_push(); return; }

        tf_off(); return;
    }

    /* ── key ── */
    if(e->type==SDL_KEYDOWN){
        if(atf<0) return;
        SDL_Keycode k=e->key.keysym.sym;
        TF *t=&tfs[atf];
        if(k==SDLK_BACKSPACE){ tf_bs(t); f2p(sel); }
        else if(k==SDLK_LEFT&&t->cur>0)    t->cur--;
        else if(k==SDLK_RIGHT&&t->cur<t->len) t->cur++;
        else if(k==SDLK_HOME) t->cur=0;
        else if(k==SDLK_END)  t->cur=t->len;
        else if(k==SDLK_a&&(e->key.keysym.mod&KMOD_CTRL)){ tf_clr(t); f2p(sel); }
        else if(k==SDLK_s&&(e->key.keysym.mod&KMOD_CTRL)){ f2p(sel); html_save(); }
        else if(k==SDLK_TAB){ f2p(sel); tf_on((atf+1)%NF); }
        else if(k==SDLK_RETURN||k==SDLK_ESCAPE) tf_off();
        return;
    }

    /* ── text input ── */
    if(e->type==SDL_TEXTINPUT&&atf>=0){
        tf_ins(&tfs[atf],e->text.text); f2p(sel); return;
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
            case WEV_ORDER:        set_st("New order received!"); break;
        }
    }
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

    /* ── layout + load ── */
    layout();
    html_load();
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
