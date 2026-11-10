/* tactrifree 1.0 beta3 - SPDX-License-Identifier: GPL-3.0-or-later */
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "j2534.h"

#ifndef GUID_SEXPR

static const GUID OP2_IFACE = {0x6D1781B7,0xC987,0x4F6C,{0x8D,0x4F,0x1E,0xFC,0x09,0x8B,0xEA,0x67}};
#endif

#define PRODUCT     "TactriFree driver 1.0 beta3"
#define DLL_REPORT  "1.02.5000"
#define API_REPORT  "04.04"

#define MAX_CHANS 256

static CRITICAL_SECTION g_lock;
static char g_lasterr[256] = "";
static long g_lastcode = 0;
static int  g_init = 0;
static DWORD g_dbgflags = 0;

#define OP20_LOG_NAME "tactrifree-debug.log"
static CRITICAL_SECTION g_loglock; static int g_loginit = 0;
static FILE *g_logf = NULL; static long g_logbytes = 0; static long g_logmax = 0;
static long log_max(void){
    if(g_logmax>0) return g_logmax;
    long mb=8; char buf[32]; DWORD n=GetEnvironmentVariableA("TACTRIFREE_LOG_MB",buf,sizeof buf);
    if(n>0 && n<sizeof buf){ long v=atol(buf); if(v>=1 && v<=1024) mb=v; }
    g_logmax=mb*1024*1024; return g_logmax;
}

static DWORD g_logfail, g_rotfail;
static void log_open_locked(void){
    if(g_logf) return;
    if(g_logfail && GetTickCount()-g_logfail<30000) return;
    char t[260]; if(!GetTempPathA(sizeof t,t)) return;
    char path[300]; _snprintf(path,sizeof path,"%s%s",t,OP20_LOG_NAME); path[sizeof path-1]=0;
    g_logf=fopen(path,"ab");
    if(g_logf){ fseek(g_logf,0,SEEK_END); g_logbytes=ftell(g_logf); g_logfail=0; } else g_logfail=GetTickCount()|1;
}

static void fmt_bytes(const unsigned char *b, int n, char *out, int outn){
    int printable=1, i, p=0;
    for(i=0;i<n;i++){ unsigned char c=b[i]; if(c!=0x0D && c!=0x0A && (c<0x20||c>0x7E)){ printable=0; break; } }
    if(printable){ for(i=0;i<n && p<outn-5;i++){ unsigned char c=b[i];
        if(c=='\r'){ out[p++]='\\'; out[p++]='r'; } else if(c=='\n'){ out[p++]='\\'; out[p++]='n'; } else out[p++]=(char)c; } }
    else { int lim=n<32?n:32; for(i=0;i<lim && p<outn-4;i++) p+=snprintf(out+p,outn-p,"%02X ",b[i]);
           if(n>32 && p<outn-4){ out[p++]='.'; out[p++]='.'; } }
    out[p]=0;
}

static void dbg(const char *fmt, ...){
    if(!g_loginit) return;
    char line[1200]; SYSTEMTIME st; GetLocalTime(&st);
    int p=_snprintf(line,sizeof line,"%02d:%02d:%02d.%03d ",st.wHour,st.wMinute,st.wSecond,st.wMilliseconds);
    if(p<0) p=0;
    va_list ap; va_start(ap,fmt); _vsnprintf(line+p,(int)sizeof line-p-3,fmt,ap); va_end(ap);
    line[sizeof line-3]=0; int ln=(int)strlen(line); line[ln++]='\r'; line[ln++]='\n'; line[ln]=0;
    EnterCriticalSection(&g_loglock);
    log_open_locked();
    if(g_logf){
        fwrite(line,1,(size_t)ln,g_logf); fflush(g_logf); g_logbytes+=ln;
        if(g_logbytes>log_max() && (!g_rotfail || GetTickCount()-g_rotfail>60000)){
            fclose(g_logf); g_logf=NULL; g_logbytes=0;
            char t[260]; if(GetTempPathA(sizeof t,t)){ char path[300],oldp[320];
                _snprintf(path,sizeof path,"%s%s",t,OP20_LOG_NAME); path[sizeof path-1]=0;
                _snprintf(oldp,sizeof oldp,"%s%s.old",t,OP20_LOG_NAME); oldp[sizeof oldp-1]=0;
                g_rotfail = MoveFileExA(path,oldp,MOVEFILE_REPLACE_EXISTING) ? 0 : (GetTickCount()|1); }
            log_open_locked();
        }
    }
    LeaveCriticalSection(&g_loglock);
}
#define dbgv dbg

static void set_err(const char *s){ dbg("ERR: %s", s); }

static __declspec(thread) const char *t_spec; static __declspec(thread) long t_spec_code; static __declspec(thread) int t_keep;
static void spec(long code, const char *txt){ t_spec=txt; t_spec_code=code; }
static const char *errname(long c, char *buf, int n){
    static const char *tab[]={"ERR_SUCCESS","ERR_NOT_SUPPORTED","ERR_INVALID_CHANNEL_ID","ERR_INVALID_PROTOCOL_ID",
        "ERR_NULL_PARAMETER","ERR_INVALID_IOCTL_VALUE","ERR_INVALID_FLAGS","ERR_FAILED","ERR_DEVICE_NOT_CONNECTED",
        "ERR_TIMEOUT","ERR_INVALID_MSG","ERR_INVALID_TIME_INTERVAL","ERR_EXCEEDED_LIMIT","ERR_INVALID_MSG_ID",
        "ERR_DEVICE_IN_USE","ERR_INVALID_IOCTL_ID","ERR_BUFFER_EMPTY","ERR_BUFFER_FULL","ERR_BUFFER_OVERFLOW",
        "ERR_PIN_INVALID","ERR_CHANNEL_IN_USE","ERR_MSG_PROTOCOL_ID","ERR_INVALID_FILTER_ID","ERR_NO_FLOW_CONTROL",
        "ERR_NOT_UNIQUE","ERR_INVALID_BAUDRATE","ERR_INVALID_DEVICE_ID"};
    if(c>=0 && c<(long)(sizeof tab/sizeof tab[0])) return tab[c];
    if(c==0x77) return "ERR_OEM_VOLTAGE_TOO_HIGH";
    if(c==0x78) return "ERR_OEM_VOLTAGE_TOO_LOW";
    _snprintf(buf,n,"UNKNOWN (%ld)",c); buf[n-1]=0; return buf;
}
static void setlast(long code, const char *txt){
    EnterCriticalSection(&g_lock); g_lastcode=code; strncpy(g_lasterr,txt,sizeof g_lasterr-1); g_lasterr[sizeof g_lasterr-1]=0; LeaveCriticalSection(&g_lock); }
static volatile LONG g_emptyrun;
static long finish(long rc){
    if(t_keep){ t_keep=0; t_spec=NULL; return rc; }
    char b[40]; if(rc && t_spec && t_spec_code==rc) setlast(rc,t_spec); else setlast(rc,errname(rc,b,sizeof b));

    if(rc==ERR_BUFFER_EMPTY){ if(InterlockedIncrement(&g_emptyrun)<=3) dbg("-> rc=%ld (%s)", rc, g_lasterr); }
    else { LONG k=InterlockedExchange(&g_emptyrun,0); if(k>3) dbg("   (%ld more empty reads not logged)", (long)(k-3));
           if(rc) dbg("-> rc=%ld (%s)", rc, g_lasterr); }
    t_spec=NULL; return rc;
}
static long bad_chan(void){ spec(2,"invalid channel ID"); return 2; }
static long bad_dev(void){ spec(26,"invalid device ID"); return 26; }

#define REPLY_MAX 4200
typedef struct Reply { struct Reply *next; unsigned seq; long result; char text[REPLY_MAX]; int tlen; char rtype; DWORD born; } Reply;

typedef struct RxMsg { struct RxMsg *next; PASSTHRU_MSG msg; } RxMsg;

typedef struct Channel {
    int used; unsigned id; unsigned protocol; unsigned internal; unsigned flags; unsigned baud;
    struct Device *dev;
} Channel;

typedef struct Device {
    int used; unsigned id;
    char path[512];
    HANDLE h;
    OVERLAPPED ov_r, ov_w;

    int winusb; volatile LONG closing;
    WINUSB_INTERFACE_HANDLE wu0, wuassoc, wuio; UCHAR pin, pout; ULONG mps;
    struct { OVERLAPPED ov; unsigned char b[512]; int pend; } rs[10]; int rnext; int wuerr;
    HANDLE thread; volatile LONG run;
    HMODULE selfref;
    volatile unsigned seqctr;
    Reply *replies; CRITICAL_SECTION rlock;
    CONDITION_VARIABLE rcv;
    CRITICAL_SECTION wlock;

    int pend_len, pend_got; unsigned pend_seq; long pend_result; char pend_type; unsigned char pend_buf[REPLY_MAX];
    DWORD pend_at;

    struct { RxMsg *head, *tail; int count; } rxq[10]; CRITICAL_SECTION qlock;
    CONDITION_VARIABLE qcv;

    unsigned char fb[0x4400]; int fblen;
    int seg_mid;
    struct { unsigned char buf[0x1021]; int len; } iso;

    struct { unsigned char buf[0x1008]; int len; unsigned ts; } kacc[10];
    unsigned progv;
    char fw[16];
    volatile LONG gone;
    volatile LONG bprobe, bfound;
    char bbuf[640]; int blen;
    Channel chans[MAX_CHANS];
} Device;

static Device g_dev[8];

static volatile LONG g_next_chan = 3;

static unsigned proto_internal(unsigned pid){
    switch(pid){
      case CAN_CH1: return 5;  case J1850VPW_CH1: return 1; case J1850PWM_CH1: return 2;
      case ISO9141_CH1: return 3; case ISO9141_CH2: return 7; case ISO9141_CH3: return 9;
      case ISO14230_CH1: return 4; case ISO14230_CH2: return 8; case ISO15765_CH1: return 6;
      default: if(pid>=1 && pid<=6) return pid; return 0;
    }
}

static unsigned proto_j2534(unsigned internal){
    switch(internal){ case 1: case 2: case 3: case 4: case 5: case 6: return internal;
      case 7: return 0x9241; case 8: return 0x9321; case 9: return 0x9242; default: return 0; }
}

static unsigned long tx_timeout_us(unsigned pid, unsigned long us){
    if(us) return us;
    switch(pid){
      case 5: case 0x9000: return 50000;
      case 1: case 2: case 3: case 4: case 6: case 0x9080: case 0x9160: case 0x9240: case 0x9241:
      case 0x9320: case 0x9321: case 0x9400: return 1000000;
      default: return 0;
    }
}

static unsigned next_seq(Device *d){
    unsigned s = (unsigned)InterlockedIncrement((LONG*)&d->seqctr);
    if(s > 0xFFFF){ d->seqctr = 1; s = 1; }
    return s;
}

static int removal_error(DWORD e){
    return e==ERROR_DEVICE_NOT_CONNECTED || e==ERROR_BAD_COMMAND || e==ERROR_GEN_FAILURE || e==ERROR_INVALID_HANDLE ||
           e==ERROR_BROKEN_PIPE || e==ERROR_PIPE_NOT_CONNECTED || e==ERROR_NO_SUCH_DEVICE || e==ERROR_DEVICE_REMOVED ||
           e==ERROR_FILE_NOT_FOUND || e==ERROR_ACCESS_DENIED;
}
static void mark_gone(Device *d, DWORD e){
    if(InterlockedExchange(&d->gone,1)) return;
    dbg("cable removed (I/O error %lu)", (unsigned long)e);
    WakeAllConditionVariable(&d->rcv); WakeAllConditionVariable(&d->qcv);
}

static struct {
    HMODULE m;
    BOOL (WINAPI *Initialize)(HANDLE, PWINUSB_INTERFACE_HANDLE);
    BOOL (WINAPI *Free)(WINUSB_INTERFACE_HANDLE);
    BOOL (WINAPI *GetAssociatedInterface)(WINUSB_INTERFACE_HANDLE, UCHAR, PWINUSB_INTERFACE_HANDLE);
    BOOL (WINAPI *QueryInterfaceSettings)(WINUSB_INTERFACE_HANDLE, UCHAR, PUSB_INTERFACE_DESCRIPTOR);
    BOOL (WINAPI *QueryPipe)(WINUSB_INTERFACE_HANDLE, UCHAR, UCHAR, PWINUSB_PIPE_INFORMATION);
    BOOL (WINAPI *SetPipePolicy)(WINUSB_INTERFACE_HANDLE, UCHAR, ULONG, ULONG, PVOID);
    BOOL (WINAPI *ReadPipe)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED);
    BOOL (WINAPI *WritePipe)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED);
    BOOL (WINAPI *AbortPipe)(WINUSB_INTERFACE_HANDLE, UCHAR);
    BOOL (WINAPI *ResetPipe)(WINUSB_INTERFACE_HANDLE, UCHAR);
} wu;
static int wu_load(void){
    if(wu.m) return 1;
    HMODULE m=NULL;
    if(!m) m=LoadLibraryExA("winusb.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!m){ dbg("winusb.dll not loadable (err %lu)",(unsigned long)GetLastError()); return 0; }
#define WU_FN(f) if(!(*(FARPROC*)&wu.f=GetProcAddress(m,"WinUsb_" #f))){ dbg("winusb.dll lacks WinUsb_" #f); FreeLibrary(m); memset(&wu,0,sizeof wu); return 0; }
    WU_FN(Initialize) WU_FN(Free) WU_FN(GetAssociatedInterface) WU_FN(QueryInterfaceSettings) WU_FN(QueryPipe)
    WU_FN(SetPipePolicy) WU_FN(ReadPipe) WU_FN(WritePipe) WU_FN(AbortPipe)
#undef WU_FN
    *(FARPROC*)&wu.ResetPipe=GetProcAddress(m,"WinUsb_ResetPipe");
    wu.m=m; return 1;
}

static int wu_issue(Device *d, int i){
    d->rs[i].pend=0; ResetEvent(d->rs[i].ov.hEvent);
    if(d->gone || d->closing) return 0;
    if(!wu.ReadPipe(d->wuio, d->pin, d->rs[i].b, d->mps, NULL, &d->rs[i].ov)){
        DWORD e=GetLastError();
        if(e!=ERROR_IO_PENDING){ if(removal_error(e)) mark_gone(d,e); return 0; }
    }
    d->rs[i].pend=1; return 1;
}
#define WU_NREAD ((int)(sizeof ((Device*)0)->rs / sizeof ((Device*)0)->rs[0]))

static int wu_read(Device *d, unsigned char *buf, DWORD n, DWORD ms){
    int total=0;

    for(int k=0; k<WU_NREAD && !d->rs[d->rnext].pend; k++){
        int i=d->rnext, others=0; for(int j=0;j<WU_NREAD;j++) others |= d->rs[j].pend;
        if(!others){ if(!wu_issue(d,i)) return -1; break; }
        wu_issue(d,i); d->rnext=(i+1)%WU_NREAD; }
    if(!d->rs[d->rnext].pend) return -1;
    if(WaitForSingleObject(d->rs[d->rnext].ov.hEvent, ms)!=WAIT_OBJECT_0){
        return 0; }
    for(;;){
        int i=d->rnext; DWORD got=0;
        BOOL ok=GetOverlappedResult(d->h, &d->rs[i].ov, &got, FALSE); d->rs[i].pend=0;
        if(!ok){
            DWORD e=GetLastError();
            if(e!=ERROR_OPERATION_ABORTED && removal_error(e)){ mark_gone(d,e); return total?total:-1; }
            if(e!=ERROR_OPERATION_ABORTED){

                dbg("WinUSB: IN pipe error %lu (%d in a row)", (unsigned long)e, d->wuerr+1);
                if(++d->wuerr>3){ mark_gone(d,e); return total?total:-1; }
                if(wu.ResetPipe) wu.ResetPipe(d->wuio, d->pin);
            }

            wu_issue(d,i); d->rnext=(i+1)%WU_NREAD;
            return total?total:-1;
        }
        d->wuerr=0;
        if(got>n-(DWORD)total) got=n-(DWORD)total;
        memcpy(buf+total, d->rs[i].b, got); total+=(int)got;
        wu_issue(d,i); d->rnext=(i+1)%WU_NREAD;
        i=d->rnext;
        if((DWORD)total+d->mps>n || !d->rs[i].pend || WaitForSingleObject(d->rs[i].ov.hEvent,0)!=WAIT_OBJECT_0) break;
    }
    return total;
}

static DWORD write_budget_ms(DWORD n){ return 2000 + n*10*1000/1200; }
static int dev_write(Device *d, const void *buf, DWORD n){
    { char pv[220]; fmt_bytes((const unsigned char*)buf,(int)n,pv,sizeof pv); dbg("TX[%lu]: %s",(unsigned long)n,pv); }

    EnterCriticalSection(&d->wlock);
    DWORD wrote=0; int ok=1; ResetEvent(d->ov_w.hEvent);
    if(d->gone) ok=0;
    else if(!(d->winusb ? wu.WritePipe(d->wuio, d->pout, (PUCHAR)buf, n, NULL, &d->ov_w)
                        : WriteFile(d->h, buf, n, &wrote, &d->ov_w))){
        DWORD e=GetLastError();
        if(e!=ERROR_IO_PENDING){ ok=0; if(removal_error(e)) mark_gone(d,e); }
        else if(WaitForSingleObject(d->ov_w.hEvent, write_budget_ms(n))!=WAIT_OBJECT_0){

            if(d->winusb) wu.AbortPipe(d->wuio, d->pout); else CancelIo(d->h);
            GetOverlappedResult(d->h,&d->ov_w,&wrote,TRUE); ok=0; }
        else if(!GetOverlappedResult(d->h,&d->ov_w,&wrote,FALSE)){ ok=0; e=GetLastError(); if(removal_error(e)) mark_gone(d,e); }
    }
    else if(d->winusb) GetOverlappedResult(d->h,&d->ov_w,&wrote,FALSE);
    LeaveCriticalSection(&d->wlock);
    return ok && (int)wrote==(int)n;
}

static int dev_read(Device *d, void *buf, DWORD n, DWORD ms){
    if(d->winusb) return wu_read(d, (unsigned char*)buf, n, ms);
    DWORD got=0; ResetEvent(d->ov_r.hEvent);
    if(!ReadFile(d->h, buf, n, &got, &d->ov_r)){
        DWORD e=GetLastError();
        if(e!=ERROR_IO_PENDING){ if(removal_error(e)) mark_gone(d,e); return -1; }
        if(WaitForSingleObject(d->ov_r.hEvent, ms)!=WAIT_OBJECT_0){

            CancelIo(d->h);
            if(GetOverlappedResult(d->h,&d->ov_r,&got,TRUE)) return (int)got;
            e=GetLastError(); if(e!=ERROR_OPERATION_ABORTED && removal_error(e)) mark_gone(d,e);
            return 0;
        }
        if(!GetOverlappedResult(d->h,&d->ov_r,&got,FALSE)){ e=GetLastError(); if(removal_error(e)) mark_gone(d,e); return -1; }
    }
    return (int)got;
}

static void push_reply(Device *d, unsigned seq, long result, const char *text, int tlen, char rtype){
    Reply *r = (Reply*)calloc(1,sizeof *r); if(!r) return;
    r->seq=seq; r->result=result; r->rtype=rtype; r->born=GetTickCount();
    r->tlen = tlen<(int)sizeof r->text? tlen:(int)sizeof r->text-1;
    memcpy(r->text,text,r->tlen); if(r->tlen<(int)sizeof r->text) r->text[r->tlen]=0;
    EnterCriticalSection(&d->rlock); r->next=d->replies; d->replies=r;

    { int k=0; for(Reply *p=r, *prev=NULL; p; ){ Reply *nx=p->next;
          if(++k>256 || (p!=r && GetTickCount()-p->born>120000)){ if(prev) prev->next=nx; free(p); }
          else prev=p;
          p=nx; } }
    LeaveCriticalSection(&d->rlock);
    WakeAllConditionVariable(&d->rcv);
}

static double now_ms(void){ static LARGE_INTEGER f; LARGE_INTEGER t; if(!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t); return (double)t.QuadPart*1000.0/(double)f.QuadPart; }

static Reply* pop_reply(Device *d, unsigned seq){
    Reply *p,*prev=NULL,*out=NULL;
    EnterCriticalSection(&d->rlock);
    for(p=d->replies; p; prev=p,p=p->next){
        if(seq==0 || p->seq==seq){ if(prev)prev->next=p->next; else d->replies=p->next; out=p; break; }
    }
    LeaveCriticalSection(&d->rlock);
    return out;
}

static void route_line(Device *d, const char *line, int len){
    if(len<2 || line[0]!='a' || line[1]!='r') return;
    dbg("RX: %.*s", len, line);
    char type = len>=3 ? line[2] : 0;

    if(type=='i'){ push_reply(d, 0x10000001u, 0, line, len, 'i'); return; }

    int i, laststart=-1, nints=0; long vals[8];
    for(i=0;i<len;i++){
        if(line[i]>='0'&&line[i]<='9'){ if(laststart<0)laststart=i; }
        else { if(laststart>=0){ if(nints<8) vals[nints++]=atol(line+laststart); laststart=-1; } }
    }
    if(laststart>=0 && nints<8) vals[nints++]=atol(line+laststart);
    if(nints<1) return;
    unsigned seq=(unsigned)vals[nints-1];

    if((type=='p' || type=='y') && nints>=3){
        int datalen=(int)vals[1];
        if(datalen>0){ d->pend_result=vals[0]; d->pend_seq=seq; d->pend_type=type;
                       d->pend_len=datalen; d->pend_got=0; d->pend_at=GetTickCount(); return; }
        push_reply(d, seq, vals[0], "", 0, type); return;
    }

    if(!type || !strchr("oefmrgw", type)){ return; }

    { long result = nints>=2 ? vals[0] : 0; push_reply(d, seq, result, line, len, type); }
}

#define RXQ_CAP 4096
static void enqueue_q(Device *d, int internal, unsigned rxstatus, unsigned ts, const unsigned char *data, int len, int extra){
    if(internal<0 || internal>9) return; if(len<0) len=0; if(len>4128) len=4128;
    RxMsg *rx=(RxMsg*)calloc(1,sizeof *rx); if(!rx) return;

    rx->msg.ProtocolID=(unsigned)internal; rx->msg.RxStatus=rxstatus; rx->msg.DataSize=(unsigned)len;
    rx->msg.Timestamp=ts; rx->msg.ExtraDataIndex=(unsigned)extra;
    if(len) memcpy(rx->msg.Data, data, len);
    EnterCriticalSection(&d->qlock);
    if(d->rxq[internal].count>=RXQ_CAP){ RxMsg *o=d->rxq[internal].head;
        d->rxq[internal].head=o->next; if(!d->rxq[internal].head) d->rxq[internal].tail=NULL; d->rxq[internal].count--; free(o); }
    if(d->rxq[internal].tail) d->rxq[internal].tail->next=rx; else d->rxq[internal].head=rx;
    d->rxq[internal].tail=rx; d->rxq[internal].count++;
    LeaveCriticalSection(&d->qlock);
    WakeAllConditionVariable(&d->qcv);
}

static RxMsg* dequeue_locked(Device *d, int internal){
    RxMsg *rx=NULL;
    if(internal>=0 && internal<=9 && d->rxq[internal].head){ rx=d->rxq[internal].head;
        d->rxq[internal].head=rx->next; if(!rx->next) d->rxq[internal].tail=NULL; d->rxq[internal].count--; }
    return rx;
}

static void clear_q(Device *d, int internal){
    if(internal<0 || internal>9) return;
    EnterCriticalSection(&d->qlock);
    { RxMsg *p=d->rxq[internal].head; while(p){ RxMsg *n2=p->next; free(p); p=n2; } }
    d->rxq[internal].head=d->rxq[internal].tail=NULL; d->rxq[internal].count=0;
    LeaveCriticalSection(&d->qlock);
}

static unsigned be32(const unsigned char *p){ return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static void enqueue_rx(Device *d, int internal, unsigned char status, const unsigned char *data, int dcount){
    if(internal<1 || internal>9) return;
    unsigned ts = dcount>=4 ? be32(data) : 0;
    if(internal==6){
        int plen = dcount-4; if(plen<0) plen=0;
        const unsigned char *payload = data+4;
        int hdr = (status&4) ? 5 : 4;
        unsigned extbit = (status&4) ? 0x80u : 0u;
        unsigned txbit  = (unsigned)(status&2) << 7;
        if(status & 0x10){
            int n = plen<=(int)sizeof d->iso.buf ? plen : (int)sizeof d->iso.buf;
            memcpy(d->iso.buf, payload, n); d->iso.len = n;
            enqueue_q(d,internal, txbit|extbit|9u, ts, d->iso.buf, d->iso.len, 0);
            d->iso.len = 0;
        } else if(!(status & 0x80)){
            if(status & 0x40){
                const unsigned char *src; int n;
                if(d->iso.len==0){ src=payload; n=plen; }
                else { n=plen-hdr; if(n<0)n=0; src=payload+hdr; }
                if(n>0 && d->iso.len+n<=(int)sizeof d->iso.buf){ memcpy(d->iso.buf+d->iso.len, src, n); d->iso.len+=n; }
                enqueue_q(d,internal, extbit | ((status&0x20)?1u:0u) | txbit, ts, d->iso.buf, d->iso.len, d->iso.len);
                d->iso.len = 0;
            } else {
                int n = plen-hdr; if(n<0) n=0;
                if(d->iso.len+n < 0x1021){ if(d->iso.len+n<=(int)sizeof d->iso.buf){ memcpy(d->iso.buf+d->iso.len, payload+hdr, n); d->iso.len+=n; } }
                else d->iso.len = 0;
            }
        } else {
            int n = plen<=(int)sizeof d->iso.buf ? plen : (int)sizeof d->iso.buf;
            memcpy(d->iso.buf, payload, n); d->iso.len = n;
            enqueue_q(d,internal, txbit|extbit|2u, ts, d->iso.buf, d->iso.len, 0);
        }
    } else if(internal==5){
        int plen=dcount-4; if(plen<0)plen=0;
        unsigned rxs=((unsigned)(status>>5)&1) | (unsigned)((status&2)<<7);
        if(!(status&8)) enqueue_q(d,internal, rxs, ts, data+4, plen, plen);
    } else {
        int kslot = internal;
        if((status & 0x80) && dcount==4){
            d->kacc[kslot].len = 0; d->kacc[kslot].ts = ts;
            enqueue_q(d,internal, 2, ts, NULL, 0, 0);
        } else if((status & 0x40) && dcount==4){
            d->kacc[kslot].ts = ts;
            if(!(status & 8))
                enqueue_q(d,internal, (unsigned)(status>>5)&1, ts, d->kacc[kslot].buf, d->kacc[kslot].len, d->kacc[kslot].len);
        } else {
            if((unsigned)(dcount + d->kacc[kslot].len) > 0xFE0u){
                enqueue_q(d,internal, (unsigned)(status>>5)&1, d->kacc[kslot].ts, d->kacc[kslot].buf, d->kacc[kslot].len, d->kacc[kslot].len);
                d->kacc[kslot].len = 0;
            }
            if(dcount>0 && d->kacc[kslot].len + dcount <= (int)sizeof d->kacc[kslot].buf){
                memcpy(d->kacc[kslot].buf + d->kacc[kslot].len, data, dcount);
                d->kacc[kslot].len += dcount;
            }
        }
    }
}
static void parse_frames(Device *d){
    int i=0;
    while(i < d->fblen){

        if(d->pend_len>0 && GetTickCount()-d->pend_at>3000){
            dbg("RX: binary reply payload incomplete after 3 s (%d of %d B) - dropped", d->pend_got, d->pend_len);
            d->pend_len=0; d->pend_got=0; }
        if(d->pend_len>0){
            int avail=d->fblen-i, need=d->pend_len-d->pend_got, take=avail<need?avail:need;
            int room=(int)sizeof d->pend_buf - d->pend_got; int keep = take<room ? take : (room>0?room:0);
            if(keep>0) memcpy(d->pend_buf+d->pend_got, d->fb+i, keep);
            d->pend_got+=take; i+=take;
            if(d->pend_got>=d->pend_len){
                int kept = d->pend_len<(int)sizeof d->pend_buf ? d->pend_len : (int)sizeof d->pend_buf;
                push_reply(d,d->pend_seq,d->pend_result,(char*)d->pend_buf,kept,d->pend_type);
                d->pend_len=0; d->pend_got=0; }
            continue;
        }
        if(d->fblen-i < 3) break;
        if(d->fb[i]!='a' || d->fb[i+1]!='r'){
            int j=i+1; while(j<d->fblen && d->fb[j]!='a') j++;
            { char pv[140]; int run=j-i; fmt_bytes(d->fb+i, run<32?run:32, pv, sizeof pv); dbg("RX unknown[%d]: %s", run, pv); }
            i=j; continue;
        }
        unsigned char type=d->fb[i+2];
        if(type>=0x33 && type<=0x39){
            if(d->fblen-i < 5) break;
            int lenb=d->fb[i+3]; unsigned char status=d->fb[i+4];
            int dcount=lenb-1; if(dcount<0) dcount=0;
            if(d->fblen-i < 5+dcount) break;

            int mid = (type=='6' && (status&0xD0)==0);
            if(mid && ++d->seg_mid>2){   }
            else { char pv[140]; fmt_bytes(d->fb+i+5, dcount<32?dcount:32, pv, sizeof pv);
              if(!mid && d->seg_mid>2) dbg("RX frame proto=%d len=%d st=0x%02X: %s (+%d middle segments not logged)",
                                           type-0x30, dcount, (unsigned)status, pv, d->seg_mid-2);
              else dbg("RX frame proto=%d len=%d st=0x%02X: %s", type-0x30, dcount, (unsigned)status, pv); }
            if(!mid) d->seg_mid=0;
            enqueue_rx(d, type-0x30, status, d->fb+i+5, dcount);
            i += 5+dcount;
        } else {
            int nl=-1; for(int j=i+2;j<d->fblen;j++){ if(d->fb[j]=='\n'){nl=j;break;} }
            if(nl<0) break;
            int ll=nl-i; if(ll>0 && d->fb[nl-1]=='\r') ll--;
            if(ll>0) route_line(d,(char*)d->fb+i,ll);
            i = nl+1;
        }
    }
    if(i>0){ if(i<d->fblen) memmove(d->fb, d->fb+i, d->fblen-i); d->fblen-=i; }
}
static DWORD WINAPI reader_thread(void *arg){
    Device *d=(Device*)arg; unsigned char buf[2048];
    while(InterlockedCompareExchange(&d->run,1,1)){
        if(d->gone){ Sleep(50); continue; }
        int got = dev_read(d, buf, sizeof buf, 200);
        if(got<0){ if(d->run && !d->closing) Sleep(5); continue; }
        if(got==0) continue;
        if(d->fblen + got > (int)sizeof d->fb){
            int drop = d->fblen + got - (int)sizeof d->fb;
            if(drop>d->fblen) drop=d->fblen;
            memmove(d->fb, d->fb+drop, d->fblen-drop); d->fblen-=drop;
        }
        if(d->bprobe){
            int room=(int)sizeof d->bbuf-1-d->blen, k=got<room?got:room;
            if(k>0){ memcpy(d->bbuf+d->blen,buf,k); d->blen+=k; d->bbuf[d->blen]=0; }
            for(int z=0;z<d->blen;z++) if(!d->bbuf[z]) d->bbuf[z]=' ';
            if(strstr(d->bbuf,"bootloader")){ InterlockedExchange(&d->bfound,1); WakeAllConditionVariable(&d->rcv); }
        }
        memcpy(d->fb+d->fblen, buf, got); d->fblen+=got;
        parse_frames(d);
    }

    if(d->selfref) FreeLibraryAndExitThread(d->selfref, 0);
    return 0;
}
static HANDLE start_reader(Device *d){
    HMODULE me=NULL;
    if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,(LPCSTR)(void*)reader_thread,&me)) me=NULL;
    d->selfref=me;
    HANDLE t=CreateThread(NULL,0,reader_thread,d,0,NULL);
    if(!t && me){ FreeLibrary(me); d->selfref=NULL; }

    if(t) SetThreadPriority(t, THREAD_PRIORITY_HIGHEST);
    return t;
}

static long cmd_wait(Device *d, const char *cmd, int clen, unsigned seq, DWORD ms, Reply **out){

    if(clen>0 && !dev_write(d, cmd, clen)){ spec(8,"device disconnected"); return ERR_DEVICE_NOT_CONNECTED; }
    double t0=now_ms();
    for(;;){
        Reply *r=pop_reply(d, seq);
        if(!r){
            if(d->gone || d->used!=1){ spec(8,"device disconnected"); return ERR_DEVICE_NOT_CONNECTED; }
            double left=(double)ms-(now_ms()-t0);
            if(left<=0){ set_err("timeout waiting for device reply"); return ERR_TIMEOUT; }
            EnterCriticalSection(&d->rlock);
            int have=d->gone || d->used!=1; for(Reply *p=d->replies;p;p=p->next) if(seq==0||p->seq==seq){ have=1; break; }
            if(!have) SleepConditionVariableCS(&d->rcv,&d->rlock,(DWORD)(left+0.999));
            LeaveCriticalSection(&d->rlock);
            continue;
        }
        if(r){

            long st = (r->rtype=='e') ? r->result : 0;
            if(st){ char m[64]; _snprintf(m,sizeof m,"device error %ld",st); m[sizeof m-1]=0; set_err(m); }
            if(out)*out=r; else free(r); return st; }
    }
}

static int wu_open(Device *d);
static int open_port(Device *d){

    wchar_t wpath[512]; MultiByteToWideChar(CP_ACP,0,d->path,-1,wpath,512);

    d->h = CreateFileW(wpath, GENERIC_READ|GENERIC_WRITE, 0,
                       NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if(d->h==INVALID_HANDLE_VALUE){ d->h=NULL; return 0; }
    d->ov_r.hEvent=CreateEvent(NULL,TRUE,FALSE,NULL);
    d->ov_w.hEvent=CreateEvent(NULL,TRUE,FALSE,NULL);
    if(d->winusb && !wu_open(d)) return 0;
    return 1;
}

static int wu_open(Device *d){
    if(!wu_load()) return 0;
    if(!wu.Initialize(d->h, &d->wu0)){ dbg("WinUsb_Initialize failed (err %lu)",(unsigned long)GetLastError()); d->wu0=NULL; return 0; }
    WINUSB_INTERFACE_HANDLE cand[8]; int nc=0, pick=-1; cand[nc++]=d->wu0;
    for(UCHAR k=0; nc<8; k++){ WINUSB_INTERFACE_HANDLE a; if(!wu.GetAssociatedInterface(d->wu0,k,&a)) break; cand[nc++]=a; }
    for(int i=0;i<nc;i++){
        USB_INTERFACE_DESCRIPTOR id; UCHAR in=0,out=0; USHORT mps=0;
        if(pick>=0 || !wu.QueryInterfaceSettings(cand[i],0,&id)) continue;
        for(UCHAR p=0;p<id.bNumEndpoints;p++){ WINUSB_PIPE_INFORMATION pi;
            if(!wu.QueryPipe(cand[i],0,p,&pi) || pi.PipeType!=UsbdPipeTypeBulk) continue;
            if(USB_ENDPOINT_DIRECTION_IN(pi.PipeId)){ if(!in){ in=pi.PipeId; mps=pi.MaximumPacketSize; } } else if(!out) out=pi.PipeId; }
        if(in && out){ pick=i; d->wuio=cand[i]; d->pin=in; d->pout=out; d->mps=mps; }
    }
    for(int i=1;i<nc;i++) if(i!=pick) wu.Free(cand[i]);
    if(pick<0){ dbg("WinUSB: no bulk IN/OUT pipe pair on the cable"); return 0; }
    if(pick>0) d->wuassoc=d->wuio;
    if(d->mps<8 || d->mps>sizeof d->rs[0].b) d->mps=64;
    { UCHAR on=1; ULONG tmo=0;
      wu.SetPipePolicy(d->wuio, d->pin, AUTO_CLEAR_STALL, sizeof on, &on);
      wu.SetPipePolicy(d->wuio, d->pin, PIPE_TRANSFER_TIMEOUT, sizeof tmo, &tmo); }
    for(int i=0;i<WU_NREAD;i++){ d->rs[i].ov.hEvent=CreateEvent(NULL,TRUE,FALSE,NULL); if(!wu_issue(d,i)) return 0; }
    dbg("WinUSB transport: data pipes OUT 0x%02X / IN 0x%02X, %lu-byte packets, %d reads pending",
        d->pout, d->pin, (unsigned long)d->mps, WU_NREAD);

    { static const int cum[13]={0,0,31,59,90,120,151,181,212,243,273,304,334};
      SYSTEMTIME st; GetLocalTime(&st);
      int yday=cum[st.wMonth]+st.wDay; if(st.wYear%4==0 && st.wMonth>2) yday++;
      int tm[8]={st.wSecond,st.wMinute,st.wHour,st.wDay,st.wMonth,st.wYear,st.wDayOfWeek,yday};
      unsigned char m[44]; memcpy(m,"atp 5 34\r\n\x03\x00",12); memcpy(m+12,tm,sizeof tm);
      if(!dev_write(d,m,sizeof m)){ dbg("WinUSB: clock-set write failed"); return 0; }
      unsigned char r[2048]; int rl=0; double t0=now_ms(); int g;

      while(now_ms()-t0<500 && (g=dev_read(d,r+rl,(DWORD)(sizeof r-rl-1),100))>0){
          rl+=g; r[rl]=0; char *a=strstr((char*)r,"arp 5"); if(!a) continue;
          char *nl=strchr(a,'\n'); if(!nl) continue;
          int svc=0, plen=0, hl=(int)(nl-(char*)r)+1;
          if(sscanf(a,"arp %d %d",&svc,&plen)!=2 || rl>=hl+plen) break; }
      { char pv[220]; fmt_bytes(r,rl,pv,sizeof pv); dbg("WinUSB: clock-set reply [%d]: %s",rl,pv); } }
    return 1;
}

static int find_kernel_device(int index, char *path, int pathlen){
    HDEVINFO di = SetupDiGetClassDevs(&OP2_IFACE, NULL, NULL, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(di==INVALID_HANDLE_VALUE) return -1;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize=sizeof ifd; int found=0, k=0;
    for(DWORD i=0; SetupDiEnumDeviceInterfaces(di,NULL,&OP2_IFACE,i,&ifd); i++){
        DWORD need=0; SetupDiGetDeviceInterfaceDetailA(di,&ifd,NULL,0,&need,NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det=(PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(need);
        det->cbSize=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        SP_DEVINFO_DATA dd; dd.cbSize=sizeof dd;
        if(SetupDiGetDeviceInterfaceDetailA(di,&ifd,det,need,NULL,&dd)){

            char svc[64]={0}; DWORD t;
            int winusb = SetupDiGetDeviceRegistryPropertyA(di,&dd,SPDRP_SERVICE,&t,(BYTE*)svc,sizeof svc-1,NULL) && !_stricmp(svc,"WinUSB");
            if(!winusb){ if(k==index){ strncpy(path,det->DevicePath,pathlen-1); path[pathlen-1]=0; found=1; } k++; }
        }
        free(det);
        if(found) break;
    }
    SetupDiDestroyDeviceInfoList(di);
    return found ? 1 : -k - 1;
}

static int find_winusb_device(int index, char *path, int pathlen){
    HDEVINFO di = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT|DIGCF_ALLCLASSES);
    if(di==INVALID_HANDLE_VALUE) return 0;
    SP_DEVINFO_DATA dd; dd.cbSize=sizeof dd; int found=0, seen=0;
    for(DWORD i=0; !found && SetupDiEnumDeviceInfo(di,i,&dd); i++){
        char ids[1024]={0}, svc[64]={0}; DWORD type=0; int match=0;
        if(!SetupDiGetDeviceRegistryPropertyA(di,&dd,SPDRP_HARDWAREID,&type,(BYTE*)ids,sizeof ids-2,NULL)) continue;
        for(char *p=ids; *p && !match; p+=strlen(p)+1){ _strupr(p);
            if(strstr(p,"VID_0403&PID_CC4D") || (strstr(p,"VID_0403&PID_CC4C") && strstr(p,"&MI_00"))) match=1; }
        if(!match || !SetupDiGetDeviceRegistryPropertyA(di,&dd,SPDRP_SERVICE,&type,(BYTE*)svc,sizeof svc-1,NULL) || _stricmp(svc,"WinUSB")) continue;
        HKEY k=SetupDiOpenDevRegKey(di,&dd,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_READ); if(k==INVALID_HANDLE_VALUE) continue;
        char gs[512]={0}; DWORD gl=sizeof gs-2;
        if(RegQueryValueExA(k,"DeviceInterfaceGUIDs",NULL,&type,(BYTE*)gs,&gl)!=ERROR_SUCCESS){
            gl=sizeof gs-2; if(RegQueryValueExA(k,"DeviceInterfaceGUID",NULL,&type,(BYTE*)gs,&gl)!=ERROR_SUCCESS) gs[0]=0; }
        RegCloseKey(k);
        unsigned long g1; unsigned g2,g3,b[8]; GUID g;
        if(sscanf(gs,"{%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}",&g1,&g2,&g3,&b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6],&b[7])!=11) continue;
        g.Data1=g1; g.Data2=(USHORT)g2; g.Data3=(USHORT)g3; for(int j=0;j<8;j++) g.Data4[j]=(UCHAR)b[j];
        if(seen++!=index) continue;
        HDEVINFO d2 = SetupDiGetClassDevs(&g, NULL, NULL, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
        if(d2==INVALID_HANDLE_VALUE) continue;
        SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize=sizeof ifd;
        for(DWORD j=0; !found && SetupDiEnumDeviceInterfaces(d2,NULL,&g,j,&ifd); j++){
            DWORD need=0; SP_DEVINFO_DATA idd; idd.cbSize=sizeof idd; SetupDiGetDeviceInterfaceDetailA(d2,&ifd,NULL,0,&need,NULL);
            PSP_DEVICE_INTERFACE_DETAIL_DATA_A det=(PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(need);
            det->cbSize=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            if(SetupDiGetDeviceInterfaceDetailA(d2,&ifd,det,need,NULL,&idd) && idd.DevInst==dd.DevInst){
                strncpy(path,det->DevicePath,pathlen-1); path[pathlen-1]=0; found=1; }
            free(det);
        }
        SetupDiDestroyDeviceInfoList(d2);
    }
    SetupDiDestroyDeviceInfoList(di);
    return found;
}

static int find_device(int index, char *path, int pathlen, int *winusb){
    *winusb=0;
    int r=find_kernel_device(index, path, pathlen);
    if(r==1) return 1;
    int nk = r<0 ? -r-1 : 0;
    if(find_winusb_device(index-nk, path, pathlen)){ *winusb=1; return 1; }
    return 0;
}

static int bootloader_present(void){
    HDEVINFO di = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT|DIGCF_ALLCLASSES);
    if(di==INVALID_HANDLE_VALUE) return 0;
    SP_DEVINFO_DATA dd; dd.cbSize=sizeof dd; int found=0;
    for(DWORD i=0; !found && SetupDiEnumDeviceInfo(di,i,&dd); i++){
        char ids[1024]; DWORD type=0;
        if(!SetupDiGetDeviceRegistryPropertyA(di,&dd,SPDRP_HARDWAREID,&type,(BYTE*)ids,sizeof ids-2,NULL)) continue;
        ids[sizeof ids-2]=0; ids[sizeof ids-1]=0;
        for(char *p=ids; *p && !found; p+=strlen(p)+1){ _strupr(p); if(strstr(p,"VID_0403&PID_CC4B")) found=1; }
    }
    SetupDiDestroyDeviceInfoList(di);
    return found;
}

static void log_cable_binding(void){
    HDEVINFO di = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT|DIGCF_ALLCLASSES);
    if(di==INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dd; dd.cbSize=sizeof dd; int seen=0;
    for(DWORD i=0; SetupDiEnumDeviceInfo(di,i,&dd); i++){
        char ids[1024]={0}, svc[64]={0}; DWORD type=0;
        if(!SetupDiGetDeviceRegistryPropertyA(di,&dd,SPDRP_HARDWAREID,&type,(BYTE*)ids,sizeof ids-2,NULL)) continue;
        _strupr(ids); if(!strstr(ids,"VID_0403&PID_CC4")) continue;
        if(!SetupDiGetDeviceRegistryPropertyA(di,&dd,SPDRP_SERVICE,&type,(BYTE*)svc,sizeof svc-1,NULL)) svc[0]=0;
        dbg("Open: no usable cable; present: %s, Windows driver '%s'%s", ids, svc[0]?svc:"(none)",
            _stricmp(svc,"WinUSB") ? " - not WinUSB: install the TactriFree driver package" : ""); seen++; }
    SetupDiDestroyDeviceInfoList(di);
    if(!seen) dbg("Open: no OpenPort 2.0 cable is plugged in");
}

static int probe_bootloader(Device *d){
    d->blen=0; d->bbuf[0]=0; InterlockedExchange(&d->bfound,0); InterlockedExchange(&d->bprobe,1);
    for(int t=0;t<3 && !d->bfound && !d->gone;t++){
        if(!dev_write(d,"tbi\n",4)) break;
        double t0=now_ms();
        EnterCriticalSection(&d->rlock);
        while(!d->bfound && !d->gone){ double left=500-(now_ms()-t0); if(left<=0) break;
            SleepConditionVariableCS(&d->rcv,&d->rlock,(DWORD)(left+0.999)); }
        LeaveCriticalSection(&d->rlock);
    }
    InterlockedExchange(&d->bprobe,0);
    if(d->bfound) dbg("bootloader banner: %.120s", d->bbuf);
    return d->bfound;
}

#define API __declspec(dllexport) long __stdcall

static void teardown(Device *d){
    InterlockedExchange(&d->run,0); InterlockedExchange(&d->closing,1);
    if(d->wuio){ wu.AbortPipe(d->wuio,d->pin); wu.AbortPipe(d->wuio,d->pout); }
    if(d->h) CancelIoEx(d->h,NULL);
    if(d->thread){
        int ended = WaitForSingleObject(d->thread,2000)==WAIT_OBJECT_0;
        if(!ended){
            if(d->wuio) wu.AbortPipe(d->wuio,d->pin);
            if(d->h) CancelIoEx(d->h,NULL);
            ended = WaitForSingleObject(d->thread,3000)==WAIT_OBJECT_0; }
        CloseHandle(d->thread); d->thread=NULL;
        if(!ended){

            dbg("WARNING: reader thread did not stop within 5 s - device slot retired");
            for(int c=0;c<MAX_CHANS;c++) d->chans[c].used=0;
            InterlockedExchange(&d->gone,1); d->used=2;
            WakeAllConditionVariable(&d->qcv); WakeAllConditionVariable(&d->rcv);
            return; } }

    if(d->wuio) wu.AbortPipe(d->wuio,d->pin);
    for(int i=0;i<WU_NREAD;i++){
        DWORD g; if(d->rs[i].pend && d->rs[i].ov.hEvent && WaitForSingleObject(d->rs[i].ov.hEvent,2000)==WAIT_OBJECT_0)
            GetOverlappedResult(d->h,&d->rs[i].ov,&g,FALSE);
        d->rs[i].pend=0;
        if(d->rs[i].ov.hEvent){ CloseHandle(d->rs[i].ov.hEvent); d->rs[i].ov.hEvent=NULL; } }
    if(d->wuassoc){ wu.Free(d->wuassoc); d->wuassoc=NULL; }
    if(d->wu0){ wu.Free(d->wu0); d->wu0=NULL; }
    d->wuio=NULL;
    if(d->h){ CloseHandle(d->h); d->h=NULL; }
    if(d->ov_r.hEvent)CloseHandle(d->ov_r.hEvent); if(d->ov_w.hEvent)CloseHandle(d->ov_w.hEvent);
    { Reply*p=d->replies; while(p){Reply*n2=p->next;free(p);p=n2;} d->replies=NULL; }
    for(int q=0;q<10;q++) clear_q(d,q);
    d->used=0;
    WakeAllConditionVariable(&d->qcv); WakeAllConditionVariable(&d->rcv);
}

static volatile LONG g_next_dev = 1;

static void fw_from_identity(Device *d, const char *line){
    static const char pfx[]="main code version : ";
    const char *p=strstr(line,pfx); d->fw[0]=0; if(!p) return;
    char tmp[32]; snprintf(tmp,sizeof tmp,"%.20s\r\n",p+sizeof pfx-1);
    memcpy(d->fw,tmp,9); d->fw[9]=0;
}
static long impl_Open(const void *pName, unsigned long *pDeviceID){
    (void)pName;
    if(!g_loginit){ InitializeCriticalSection(&g_loglock); g_loginit=1; }
    if(!g_init){ InitializeCriticalSection(&g_lock); g_init=1; }
    char path[512]; int winusb=0;
    if(!find_device(0, path, sizeof path, &winusb)){
        log_cable_binding();
        if(bootloader_present()){ spec(8,"stuck in bootloader"); return ERR_DEVICE_NOT_CONNECTED; }
        spec(8,"no devices available"); return ERR_DEVICE_NOT_CONNECTED; }
    int slot=-1;
    EnterCriticalSection(&g_lock);
    for(int i=0;i<8;i++) if(!g_dev[i].used){ slot=i; memset(&g_dev[i],0,sizeof g_dev[i]); g_dev[i].used=1; break; }
    LeaveCriticalSection(&g_lock);
    if(slot<0){ spec(8,"no devices available"); return ERR_DEVICE_NOT_CONNECTED; }
    Device *d=&g_dev[slot];
    d->id=(unsigned)(InterlockedIncrement(&g_next_dev)-1);
    d->seqctr=1;
    d->progv=0xFFFFFFFFu;
    strncpy(d->path,path,sizeof d->path-1); d->winusb=winusb;
    InitializeCriticalSection(&d->rlock);
    InitializeCriticalSection(&d->qlock); InitializeConditionVariable(&d->qcv); InitializeConditionVariable(&d->rcv); InitializeCriticalSection(&d->wlock);
    if(!open_port(d)){ teardown(d); spec(8,"no devices available"); return ERR_DEVICE_NOT_CONNECTED; }
    d->run=1; d->thread=start_reader(d);

    { Reply *r=NULL; const char * id="\r\n\r\nati\r\n";
      long rc=cmd_wait(d, id, (int)strlen(id), 0x10000001u, 500, &r);
      if(!rc && r) fw_from_identity(d, r->text);
      if(r) free(r);
      if(rc && !d->gone && probe_bootloader(d)){ teardown(d); spec(8,"stuck in bootloader"); return ERR_DEVICE_NOT_CONNECTED; }
      if(rc){ teardown(d); spec(7,"device not responding"); return ERR_FAILED; } }

    { unsigned seq=next_seq(d); char cmd[32]; int n=snprintf(cmd,sizeof cmd,"ata %u\r\n",seq);
      Reply *r=NULL; long rc=cmd_wait(d, cmd, n, seq, 500, &r); if(r) free(r);
      if(rc){ teardown(d); return rc; } }

    if(pDeviceID) *pDeviceID=d->id;
    dbg("PassThruOpen OK: dev=%u path=%s transport=%s", d->id, d->path, d->winusb?"WinUSB":"vendor driver");
    return STATUS_NOERROR;
}
static Device* dev_by_id(unsigned id){ for(int i=0;i<8;i++) if(g_dev[i].used==1 && g_dev[i].id==id) return &g_dev[i]; return NULL; }
static Channel* chan_by_id(unsigned cid, Device **pd){
    for(int i=0;i<8;i++) if(g_dev[i].used==1)
        for(int c=0;c<MAX_CHANS;c++) if(g_dev[i].chans[c].used && g_dev[i].chans[c].id==cid){ if(pd)*pd=&g_dev[i]; return &g_dev[i].chans[c]; }
    return NULL;
}

static long impl_Close(unsigned long DeviceID){
    Device *d=dev_by_id(DeviceID); if(!d) return bad_dev();
    dbg("PassThruClose dev=%lu", DeviceID);

    for(int c=0;c<MAX_CHANS;c++) if(d->chans[c].used){
        unsigned s=next_seq(d); char cc[48]; int cn=snprintf(cc,sizeof cc,"atc%u %u\r\n",d->chans[c].internal,s);
        Reply*r=NULL; cmd_wait(d,cc,cn,s,500,&r); if(r)free(r); d->chans[c].used=0;
    }
    unsigned zs=next_seq(d); char cmd[32]; int n=snprintf(cmd,sizeof cmd,"atz %u\r\n", zs);
    Reply*r=NULL; long rc=cmd_wait(d,cmd,n,zs,500,&r); if(r)free(r);
    teardown(d);
    return rc;
}
static long impl_Connect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags,
                    unsigned long BaudRate, unsigned long *pChannelID){
    Device *d=dev_by_id(DeviceID); if(!d) return bad_dev();
    int slot=-1;
    EnterCriticalSection(&g_lock);
    for(int c=0;c<MAX_CHANS;c++) if(!d->chans[c].used){ slot=c; d->chans[c].id=0; d->chans[c].used=1; break; }
    LeaveCriticalSection(&g_lock);
    if(slot<0) return ERR_EXCEEDED_LIMIT;
    Channel *ch=&d->chans[slot]; ch->id=0;
    ch->protocol=ProtocolID; ch->internal=proto_internal(ProtocolID); ch->flags=Flags; ch->dev=d;
    ch->baud = BaudRate;
    unsigned seq=next_seq(d);

    unsigned long f4 = pChannelID ? *pChannelID : 0;
    char cmd[128]; int n=snprintf(cmd,sizeof cmd,"ato%u %lu %lu %lu %u\r\n", ch->internal, (unsigned long)Flags, (unsigned long)BaudRate, f4, seq);
    Reply*r=NULL; long rc=cmd_wait(d,cmd,n,seq,500,&r); if(r)free(r);
    if(rc){ ch->used=0; dbg("Connect proto=%lu -> rc=%ld", (unsigned long)ProtocolID, rc); return rc; }
    ch->id = (unsigned)(InterlockedIncrement(&g_next_chan) - 1);
    dbg("Connect proto=%lu internal=%u baud=%u flags=0x%lX -> rc=0 chan=%u", (unsigned long)ProtocolID, ch->internal, ch->baud, (unsigned long)Flags, ch->id);
    clear_q(d, (int)ch->internal);
    if(pChannelID)*pChannelID=ch->id;
    return STATUS_NOERROR;
}

static long impl_Disconnect(unsigned long ChannelID){
    Device *d=NULL; Channel *ch=chan_by_id(ChannelID,&d); if(!ch) return bad_chan();
    dbg("Disconnect chan=%lu", ChannelID);
    unsigned seq=next_seq(d); char cmd[64]; int n=snprintf(cmd,sizeof cmd,"atc%u %u\r\n",ch->internal,seq);
    Reply*r=NULL; long rc=cmd_wait(d,cmd,n,seq,500,&r); if(r)free(r);
    if(!rc) clear_q(d, (int)ch->internal);
    ch->used=0; return rc;
}

static long impl_ReadVersion(unsigned long DeviceID, char *pFirmwareVersion, char *pDllVersion, char *pApiVersion){
    Device *d=dev_by_id(DeviceID); if(!d) return bad_dev();
    if(!pFirmwareVersion||!pDllVersion||!pApiVersion) return ERR_NULL_PARAMETER;

    strcpy(pFirmwareVersion, d->fw); strcpy(pDllVersion, DLL_REPORT); strcpy(pApiVersion, API_REPORT);
    dbg("ReadVersion -> fw=%s dll=%s api=%s", d->fw, DLL_REPORT, API_REPORT);
    return STATUS_NOERROR;
}

API PassThruGetLastError(char *pErrorDescription){

    EnterCriticalSection(&g_lock); long c=g_lastcode; if(pErrorDescription) strcpy(pErrorDescription,g_lasterr); LeaveCriticalSection(&g_lock);
    return c;
}

static long do_setconfig(Device *d, unsigned internal, SCONFIG *c){
    unsigned seq=next_seq(d); char cmd[96];
    int n=snprintf(cmd,sizeof cmd,"ats%u %lu %lu %u\r\n", internal,(unsigned long)c->Parameter,(unsigned long)c->Value,seq);
    Reply*r=NULL; long rc=cmd_wait(d,cmd,n,seq,500,&r); if(r)free(r); return rc;
}

static int reply_ints(const Reply *r, long *vals, int max){
    int nv=0; const char*p=r->text;
    while(*p && nv<max){ if(*p>='0'&&*p<='9'){ vals[nv++]=atol(p); while(*p>='0'&&*p<='9')p++; } else p++; }
    return nv;
}
static unsigned reply_value(const Reply *r){
    long vals[8]; int nv = (r->rtype=='p'||r->rtype=='y') ? 0 : reply_ints(r,vals,8);
    return (nv>=2)? (unsigned)vals[nv-2] : (unsigned)r->result;
}
static long do_getconfig(Device *d, unsigned internal, SCONFIG *c){
    unsigned seq=next_seq(d); char cmd[96];
    int n=snprintf(cmd,sizeof cmd,"atg%u %lu %u\r\n", internal,(unsigned long)c->Parameter,seq);
    Reply*r=NULL; long rc=cmd_wait(d,cmd,n,seq,5000,&r);
    if(!rc && r){ c->Value=(unsigned long)reply_value(r); } if(r)free(r); return rc;
}

static long get_device_instances(void *pOutput){
    if(!pOutput) return ERR_NULL_PARAMETER;
    char path[512]; int winusb; if(!find_device(0, path, sizeof path, &winusb)) return ERR_DEVICE_NOT_CONNECTED;
    wchar_t w[600]; int n=MultiByteToWideChar(CP_ACP,0,path,-1,w,512); if(n<=0) return ERR_FAILED;
    HANDLE t=CreateFileW(w,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
    if(t==INVALID_HANDLE_VALUE) wcscat(w,L" [in use]"); else CloseHandle(t);
    wcscpy((wchar_t*)pOutput,w); return STATUS_NOERROR;
}

static long dev_ioctl(Device *d, unsigned long IoctlID, const void *pInput, void *pOutput){
    switch(IoctlID){
      case READ_VBATT: case READ_PROG_VOLTAGE: {
          unsigned pin=16;
          if(IoctlID==READ_PROG_VOLTAGE){

              if(!pInput){ t_keep=1; return (long)d->progv; }
              pin=*(const unsigned*)pInput;
          }
          if(!pOutput) return ERR_NULL_PARAMETER;
          unsigned seq=next_seq(d); char cmd[48]; int n=snprintf(cmd,sizeof cmd,"atr %d %u\r\n",(int)pin,seq);
          Reply*r=NULL; long rc=cmd_wait(d,cmd,n,seq,5000,&r);
          if(!rc && r) *(unsigned*)pOutput = reply_value(r);
          if(r)free(r); return rc; }
      case TX_IOCTL_APP_SERVICE: {

          if(!pInput || !pOutput) return ERR_FAILED;
          const unsigned *in=(const unsigned*)pInput; unsigned len=in[0], svcid=in[1];
          if(len > 0x80) return ERR_FAILED;
          unsigned seq=next_seq(d); unsigned char buf[64+0x80];
          int n=snprintf((char*)buf,64,"atp %u %u %u\r\n",svcid,len,seq);
          memcpy(buf+n,(const unsigned char*)pInput+8,len);
          Reply*r=NULL; long rc=cmd_wait(d,(char*)buf,n+(int)len,seq,5000,&r);
          if(!rc){
              unsigned *olen=(unsigned*)pOutput; unsigned char*odata=(unsigned char*)pOutput+4;
              if(r && r->rtype=='p'){ memcpy(odata,r->text,(size_t)r->tlen); *olen=(unsigned)r->tlen; }
              else *olen=0;
          }
          if(r)free(r); return rc; }
      case TX_IOCTL_SET_DEV_DEBUG_FLAGS: t_keep=1; return STATUS_NOERROR;
    }
    spec(15,"unknown ioctl ID"); return ERR_INVALID_IOCTL_ID;
}

static long chan_ioctl(Device *d, Channel *ch, unsigned long IoctlID, const void *pInput, void *pOutput){
    switch(IoctlID){

      case GET_CONFIG: { if(!pInput) return ERR_NULL_PARAMETER; SCONFIG_LIST*L=(SCONFIG_LIST*)pInput; long rc=0;
                         if(L->NumOfParams && !L->ConfigPtr) return ERR_NULL_PARAMETER;
                         for(unsigned i=0;i<L->NumOfParams;i++) rc=do_getconfig(d,ch->internal,&L->ConfigPtr[i]); return rc; }
      case SET_CONFIG: { if(!pInput) return ERR_NULL_PARAMETER; SCONFIG_LIST*L=(SCONFIG_LIST*)pInput; long rc=0;
                         if(L->NumOfParams && !L->ConfigPtr) return ERR_NULL_PARAMETER;
                         for(unsigned i=0;i<L->NumOfParams;i++) rc=do_setconfig(d,ch->internal,&L->ConfigPtr[i]); return rc; }
      case CLEAR_TX_BUFFER: return STATUS_NOERROR;
      case CLEAR_RX_BUFFER: clear_q(d,(int)ch->internal); return STATUS_NOERROR;
      case CLEAR_PERIODIC_MSGS: {
          unsigned seq=next_seq(d);char cmd[48];int n=snprintf(cmd,sizeof cmd,"atl%u %u\r\n",ch->internal,seq);
          Reply*r=NULL;long rc=cmd_wait(d,cmd,n,seq,500,&r);if(r)free(r); return rc; }
      case CLEAR_MSG_FILTERS: {

          unsigned seq=next_seq(d);char cmd[48];int n=snprintf(cmd,sizeof cmd,"atk%u %ld %u\r\n",ch->internal,-1L,seq);
          Reply*r=NULL;long rc=cmd_wait(d,cmd,n,seq,500,&r);if(r)free(r); return rc; }
      case CLEAR_FUNCT_MSG_LOOKUP_TABLE: case ADD_TO_FUNCT_MSG_LOOKUP_TABLE:
      case DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE:
          t_keep=1; return STATUS_NOERROR;
      case FIVE_BAUD_INIT: {
          SBYTE_ARRAY *in=(SBYTE_ARRAY*)pInput, *out=(SBYTE_ARRAY*)pOutput;
          if(!in || !in->BytePtr || !out || !out->BytePtr) return ERR_NULL_PARAMETER;
          if(in->NumOfBytes!=1) return ERR_INVALID_MSG;
          unsigned seq=next_seq(d); char cmd[64];
          int n=snprintf(cmd,sizeof cmd,"atw%u %d %u\r\n", ch->internal, (int)in->BytePtr[0], seq);
          Reply*r=NULL; long rc=cmd_wait(d,cmd,n,seq,5000,&r);
          if(!rc){

              long v[8]; int nv = (r && r->rtype=='w') ? reply_ints(r,v,8) : 0;
              out->BytePtr[0] = (unsigned char)(nv>=4 ? v[nv-3] : 0);
              out->BytePtr[1] = (unsigned char)(nv>=4 ? v[nv-2] : 0);
              out->NumOfBytes = 2;
          }
          if(r)free(r); return rc; }
      case FAST_INIT: {
          const PASSTHRU_MSG *im=(const PASSTHRU_MSG*)pInput; unsigned seq=next_seq(d);
          unsigned char buf[96+16]; int n;
          if(im){
              if(im->DataSize > 0x10) return ERR_INVALID_MSG;

              n=snprintf((char*)buf,96,"aty%u %lu %lu %u\r\n", ch->internal,(unsigned long)im->DataSize,(unsigned long)im->TxFlags,seq);
              memcpy(buf+n, im->Data, im->DataSize); n+=(int)im->DataSize;
          } else {
              n=snprintf((char*)buf,96,"aty%u %d %d %u\r\n", ch->internal, 0, 0, seq);
          }
          Reply*r=NULL; long rc=cmd_wait(d,(char*)buf,n,seq,5000,&r);

          if(!rc && pOutput){
              PASSTHRU_MSG *om=(PASSTHRU_MSG*)pOutput; memset(om,0,sizeof *om);
              if(r && r->rtype=='y'){
                  int len=r->tlen; if(len>PASSTHRU_MSG_DATA_SIZE) len=PASSTHRU_MSG_DATA_SIZE;
                  om->ProtocolID=proto_j2534((unsigned)r->result); om->DataSize=(unsigned)len; om->ExtraDataIndex=(unsigned)len;
                  memcpy(om->Data, r->text, len);
              }
          }
          if(r)free(r); return rc; }
    }
    spec(15,"unknown ioctl ID"); return ERR_INVALID_IOCTL_ID;
}
static void *g_statuscb;
static long impl_Ioctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput){
    dbg("Ioctl id=0x%lX arg=0x%lX", (unsigned long)IoctlID, (unsigned long)ChannelID);
    Device *d=NULL; Channel *ch=NULL;
    switch(IoctlID){
      case TX_IOCTL_SET_DLL_DEBUG_FLAGS: g_dbgflags=(DWORD)(ULONG_PTR)pInput; t_keep=1; return STATUS_NOERROR;
      case TX_IOCTL_SET_DLL_STATUS_CALLBACK: g_statuscb=(void*)pInput; t_keep=1; return STATUS_NOERROR;
      case TX_IOCTL_GET_DEVICE_INSTANCES: t_keep=1; return get_device_instances(pOutput);
      case TX_IOCTL_APP_SERVICE: case READ_VBATT: case READ_PROG_VOLTAGE: case TX_IOCTL_SET_DEV_DEBUG_FLAGS:
          if(ChannelID==0){
              for(int i=0;i<8;i++) if(g_dev[i].used==1){ t_keep=1; return STATUS_NOERROR; }
              spec(8,"no device connected"); return ERR_DEVICE_NOT_CONNECTED; }
          d=dev_by_id(ChannelID);
          if(!d){ ch=chan_by_id(ChannelID,&d); if(!ch) return bad_chan(); }
          return dev_ioctl(d,IoctlID,pInput,pOutput);
      default:
          ch=chan_by_id(ChannelID,&d); if(!ch) return bad_chan();
          return chan_ioctl(d,ch,IoctlID,pInput,pOutput);
    }
}
static long impl_SetProgrammingVoltage(unsigned long DeviceID, unsigned long PinNumber, unsigned long Voltage){
    Device *d=dev_by_id(DeviceID); if(!d) return bad_dev();
    dbg("SetProgrammingVoltage pin=%lu voltage=%lu", PinNumber, Voltage);
    d->progv=(unsigned)Voltage;

    unsigned seq=next_seq(d); char cmd[64]; int n=snprintf(cmd,sizeof cmd,"atv %ld %ld %u\r\n",(long)PinNumber,(long)Voltage,seq);
    Reply*r=NULL; long rc=cmd_wait(d,cmd,n,seq,500,&r); if(r)free(r); return rc;
}

static unsigned eff_proto(const Channel *ch, unsigned long msgproto){
    if(msgproto==ch->protocol) return ch->protocol ? ch->protocol : 0;
    if(msgproto==5 && ch->protocol==6) return 5;
    return 0;
}

static long impl_WriteMsgs(unsigned long ChannelID, const void *pMsg, unsigned long *pNumMsgs, unsigned long Timeout){
    Device *d=NULL; Channel *ch=chan_by_id(ChannelID,&d); if(!ch) return bad_chan();
    const PASSTHRU_MSG *m=(const PASSTHRU_MSG*)pMsg;
    dbg("WriteMsgs chan=%lu n=%lu tmo=%lu", ChannelID, pNumMsgs?(unsigned long)*pNumMsgs:0UL, (unsigned long)Timeout);

    if(!m || !pNumMsgs){ set_err("WriteMsgs: NULL message or count"); return ERR_NULL_PARAMETER; }

    double t0=now_ms(); long rc=0;
    for(unsigned i=0;i<*pNumMsgs;i++){
        long tmo=(long)Timeout;
        if(Timeout && i){ tmo=(long)Timeout-(long)(now_ms()-t0); if(tmo<0){ set_err("WriteMsgs timeout"); rc=ERR_TIMEOUT; break; } }
        const PASSTHRU_MSG *mm=&m[i];
        unsigned ep=eff_proto(ch, mm->ProtocolID);
        if(!ep){ set_err("WriteMsgs: msg ProtocolID mismatches channel"); rc=ERR_INVALID_PROTOCOL_ID; break; }
        unsigned internal=proto_internal(ep);

        unsigned dn = mm->DataSize;

        if(dn>4128){ set_err("WriteMsgs: DataSize above 4128"); rc=ERR_INVALID_MSG; break; }
        unsigned char buf[64+4128]; int hn; unsigned seq=next_seq(d);
        unsigned long dtmo=tx_timeout_us(ep, (unsigned long)tmo*1000UL);
        if(tmo==0) hn=snprintf((char*)buf,64,"att%u %lu %lu %lu\r\n",internal,(unsigned long)dn,(unsigned long)mm->TxFlags,dtmo);
        else       hn=snprintf((char*)buf,64,"att%u %lu %lu %lu %u\r\n",internal,(unsigned long)dn,(unsigned long)mm->TxFlags,dtmo,seq);
        if(dn) memcpy(buf+hn, mm->Data, dn);
        if(!dev_write(d,buf,hn+dn)){
            if(d->gone){ spec(8,"device disconnected"); rc=ERR_DEVICE_NOT_CONNECTED; }
            else { spec(9,"timeout writing to the device"); rc=ERR_TIMEOUT; }
            break; }
        if(tmo){ Reply*r=NULL; rc=cmd_wait(d,(char*)buf,0,seq,(DWORD)tmo,&r); if(r)free(r); if(rc) break; }
    }
    return rc;
}
static long impl_ReadMsgs(unsigned long ChannelID, void *pMsg, unsigned long *pNumMsgs, unsigned long Timeout){
    if(!pNumMsgs || *pNumMsgs==0){ spec(7,"zero message buffer count provided for read"); return ERR_FAILED; }
    Device *d=NULL; Channel *ch=chan_by_id(ChannelID,&d); if(!ch) return bad_chan();

    int qs[2]={ (int)ch->internal, 5 }; int nq = (ch->internal==6) ? 2 : 1;
    PASSTHRU_MSG *out=(PASSTHRU_MSG*)pMsg; unsigned long want=*pNumMsgs, n=0; double t0=now_ms(); int nullbuf=0;

    EnterCriticalSection(&d->qlock);
    for(;;){
        int got_any=0;
        if(!out){ for(int q=0;q<nq;q++) if(d->rxq[qs[q]].head) nullbuf=1; if(nullbuf) break; }
        for(int q=0;q<nq && n<want;q++){ RxMsg *rx=dequeue_locked(d,qs[q]); if(rx){ out[n++]=rx->msg; free(rx); got_any=1; } }
        if(n>=want) break;
        if(!got_any){
            if(Timeout==0 || d->gone || d->used!=1) break;
            double left=(double)Timeout-(now_ms()-t0); if(left<=0) break;
            SleepConditionVariableCS(&d->qcv,&d->qlock,(DWORD)(left+0.999));
        }
    }
    LeaveCriticalSection(&d->qlock);
    if(nullbuf){ set_err("ReadMsgs: NULL message buffer"); return ERR_NULL_PARAMETER; }
    *pNumMsgs=n;
    if(n) dbg("ReadMsgs chan=%lu got=%lu (first proto=%lu size=%lu)", ChannelID, n,
              (unsigned long)out[0].ProtocolID, (unsigned long)out[0].DataSize);
    if(n==0 && d->gone){ spec(8,"device disconnected"); return ERR_DEVICE_NOT_CONNECTED; }
    if(n==0){ spec(16,"no data was read"); return ERR_BUFFER_EMPTY; }
    if(n<want){ spec(9,"fewer messages were read than requested before timeout"); return ERR_TIMEOUT; }
    return STATUS_NOERROR;
}
static long impl_StartMsgFilter(unsigned long ChannelID, unsigned long FilterType, const void *pMaskMsg,
                           const void *pPatternMsg, const void *pFlowControlMsg, unsigned long *pFilterID){
    Device *d=NULL; Channel *ch=chan_by_id(ChannelID,&d); if(!ch) return bad_chan();
    const PASSTHRU_MSG *mask=(const PASSTHRU_MSG*)pMaskMsg, *pat=(const PASSTHRU_MSG*)pPatternMsg, *flow=(const PASSTHRU_MSG*)pFlowControlMsg;

    if(!mask || !pat || (FilterType==3 && !flow) ||
       pat->TxFlags!=mask->TxFlags || pat->DataSize!=mask->DataSize ||
       (flow && (flow->ProtocolID!=ch->protocol || flow->TxFlags!=mask->TxFlags || flow->DataSize!=mask->DataSize))){
        set_err("invalid filter messages"); return ERR_FAILED; }

    unsigned dlen = mask->DataSize; if(dlen>4128)dlen=4128;
    unsigned seq=next_seq(d); unsigned char buf[64+3*4128]; unsigned off;
    int n=snprintf((char*)buf,64,"atf%u %ld %ld %ld %u\r\n",ch->internal,(long)FilterType,(long)mask->TxFlags,(long)dlen,seq);
    off=(unsigned)n;
    memcpy(buf+off, mask->Data, dlen); off+=dlen;
    memcpy(buf+off, pat->Data,  dlen); off+=dlen;
    if(FilterType==3 && flow){ memcpy(buf+off, flow->Data, dlen); off+=dlen; }
    Reply*r=NULL; long rc=cmd_wait(d,(char*)buf,(int)off,seq,500,&r);
    if(!rc && pFilterID) *pFilterID = r ? reply_value(r) : 0;
    if(r)free(r); return rc;
}
static long impl_StopMsgFilter(unsigned long ChannelID, unsigned long FilterID){
    Device *d=NULL; Channel *ch=chan_by_id(ChannelID,&d); if(!ch) return bad_chan();

    unsigned seq=next_seq(d); char cmd[64]; int n=snprintf(cmd,sizeof cmd,"atk%u %ld %u\r\n",ch->internal,(long)FilterID,seq);
    Reply*r=NULL; long rc=cmd_wait(d,cmd,n,seq,500,&r); if(r)free(r); return rc;
}
static long impl_StartPeriodicMsg(unsigned long ChannelID, const void *pMsg, unsigned long *pMsgID, unsigned long TimeInterval){
    Device *d=NULL; Channel *ch=chan_by_id(ChannelID,&d); if(!ch) return bad_chan();
    const PASSTHRU_MSG *m=(const PASSTHRU_MSG*)pMsg;
    if(!m || !pMsgID){ set_err("null periodic message"); return ERR_FAILED; }
    unsigned ep=eff_proto(ch, m->ProtocolID); if(!ep) return ERR_INVALID_PROTOCOL_ID;

    long iv=(long)TimeInterval, hi=0;
    if(iv<0){ hi=(long)((TimeInterval>>16)&0x7fff)*1000; iv=(long)(TimeInterval&0xffff); }
    unsigned dn=m->DataSize;
    if(dn>4128){ set_err("periodic message DataSize above 4128"); return ERR_INVALID_MSG; }
    unsigned seq=next_seq(d); unsigned char buf[96+4128];
    int n=snprintf((char*)buf,96,"atm%u %ld %ld %lu %lu %u\r\n",proto_internal(ep),iv*1000,hi,(unsigned long)m->TxFlags,(unsigned long)dn,seq);
    if(dn) memcpy(buf+n, m->Data, dn);
    Reply*r=NULL; long rc=cmd_wait(d,(char*)buf,n+(int)dn,seq,500,&r);
    if(!rc) *pMsgID = r ? reply_value(r) : 0;
    if(r)free(r); return rc;
}
static long impl_StopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID){
    Device *d=NULL; Channel *ch=chan_by_id(ChannelID,&d); if(!ch) return bad_chan();

    unsigned seq=next_seq(d); char cmd[64]; int n=snprintf(cmd,sizeof cmd,"atn%u %ld %u\r\n",ch->internal,(long)MsgID,seq);
    Reply*r=NULL; long rc=cmd_wait(d,cmd,n,seq,500,&r); if(r)free(r); return rc;
}

#define ENTER() (t_spec=NULL, t_keep=0)
API PassThruOpen(const void *a, unsigned long *b){ ENTER(); return finish(impl_Open(a,b)); }
API PassThruClose(unsigned long a){ ENTER(); return finish(impl_Close(a)); }
API PassThruConnect(unsigned long a, unsigned long b, unsigned long c, unsigned long d, unsigned long *e){ ENTER(); return finish(impl_Connect(a,b,c,d,e)); }
API PassThruDisconnect(unsigned long a){ ENTER(); return finish(impl_Disconnect(a)); }
API PassThruReadVersion(unsigned long a, char *b, char *c, char *d){ ENTER(); long rc=impl_ReadVersion(a,b,c,d); return rc? finish(rc) : rc; }
API PassThruIoctl(unsigned long a, unsigned long b, const void *c, void *d){ ENTER(); return finish(impl_Ioctl(a,b,c,d)); }
API PassThruSetProgrammingVoltage(unsigned long a, unsigned long b, unsigned long c){ ENTER(); return finish(impl_SetProgrammingVoltage(a,b,c)); }
API PassThruWriteMsgs(unsigned long a, const void *b, unsigned long *c, unsigned long d){ ENTER(); return finish(impl_WriteMsgs(a,b,c,d)); }
API PassThruReadMsgs(unsigned long a, void *b, unsigned long *c, unsigned long d){ ENTER(); return finish(impl_ReadMsgs(a,b,c,d)); }
API PassThruStartMsgFilter(unsigned long a, unsigned long b, const void *c, const void *d, const void *e, unsigned long *f){ ENTER(); return finish(impl_StartMsgFilter(a,b,c,d,e,f)); }
API PassThruStopMsgFilter(unsigned long a, unsigned long b){ ENTER(); return finish(impl_StopMsgFilter(a,b)); }
API PassThruStartPeriodicMsg(unsigned long a, const void *b, unsigned long *c, unsigned long d){ ENTER(); return finish(impl_StartPeriodicMsg(a,b,c,d)); }
API PassThruStopPeriodicMsg(unsigned long a, unsigned long b){ ENTER(); return finish(impl_StopPeriodicMsg(a,b)); }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void *resv){ (void)h;(void)resv;
    if(reason==DLL_PROCESS_ATTACH){
        if(!g_loginit){ InitializeCriticalSection(&g_loglock); g_loginit=1; }
        if(!g_init){ InitializeCriticalSection(&g_lock); g_init=1; }
        dbg("==== %s attach; dll=%s ====", PRODUCT, DLL_REPORT);
    }
    return TRUE; }
