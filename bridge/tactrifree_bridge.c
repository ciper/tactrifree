/* tactrifree 1.0 beta3 - SPDX-License-Identifier: GPL-3.0-or-later */
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <string.h>
#include <intrin.h>
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"advapi32.lib")
#ifdef AS_GLU32
#include "glu32_stubs.h"
#endif

typedef LONG NTSTATUS;
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UNI;
typedef struct { ULONG Length; HANDLE Root; UNI *ObjectName; ULONG Attributes; PVOID SD, SQOS; } OBJATTR;
typedef struct { union { NTSTATUS Status; PVOID Pointer; }; ULONG_PTR Information; } IOSB;
#define ST_SUCCESS 0
#define ST_PENDING 0x103
#define ST_CANCELLED ((NTSTATUS)0xC0000120L)
#define ST_NOT_CONNECTED ((NTSTATUS)0xC000009DL)

static char g_fakefw[16]="1.17.4869"; static int g_block=1;
static int g_blocknet=1; static volatile LONG g_netblocked;     static char g_testblock[16]="";
static wchar_t g_testpath[64];

static CRITICAL_SECTION g_loglk; static FILE *g_log; static char g_logpath[MAX_PATH]; static long g_logmax=8L<<20;
static LARGE_INTEGER g_qpf;
static void log_open(void){
    char t[MAX_PATH]; GetTempPathA(MAX_PATH,t); _snprintf(g_logpath,MAX_PATH,"%stactrifree-bridge.log",t); g_logpath[MAX_PATH-1]=0;
    char e[16]; DWORD n=GetEnvironmentVariableA("TACTRIFREE_LOG_MB",e,sizeof e); if(n>0&&n<sizeof e){ long mb=atol(e); if(mb>=1&&mb<=1024) g_logmax=mb<<20; }
    g_log=fopen(g_logpath,"ab"); }
static void logf_(const char *fmt, ...){
    char b[1200]; va_list ap; va_start(ap,fmt); _vsnprintf(b,sizeof b-1,fmt,ap); va_end(ap); b[sizeof b-1]=0;
    SYSTEMTIME s; GetLocalTime(&s);
    EnterCriticalSection(&g_loglk);
    if(g_log){

        static DWORD rotfail;
        if(ftell(g_log)>g_logmax && (!rotfail || GetTickCount()-rotfail>60000)){ char old[MAX_PATH+8]; fclose(g_log); _snprintf(old,sizeof old,"%s.old",g_logpath); old[sizeof old-1]=0;
            DeleteFileA(old); rotfail = MoveFileA(g_logpath,old) ? 0 : (GetTickCount()|1); g_log=fopen(g_logpath,"ab"); }
        if(g_log){ fprintf(g_log,"%02d:%02d:%02d.%03d [%lu] %s\n",s.wHour,s.wMinute,s.wSecond,s.wMilliseconds,GetCurrentProcessId(),b); fflush(g_log); }
    }
    LeaveCriticalSection(&g_loglk); }
static void fmt_bytes(const unsigned char *d,int n,char *o,int cap){
    int k=0, m=n<32?n:32; for(int i=0;i<m && k<cap-8;i++){ unsigned char c=d[i];
        k+= (c>=32&&c<127&&c!='\\') ? _snprintf(o+k,cap-k,"%c",c) : c=='\r'?_snprintf(o+k,cap-k,"\\r"):c=='\n'?_snprintf(o+k,cap-k,"\\n"):_snprintf(o+k,cap-k,"\\x%02X",c); }
    if(n>32 && k<cap-12) k+=_snprintf(o+k,cap-k," ..(%d B)",n); o[k<cap?k:cap-1]=0; }

static DWORD g_tls=64;
static int guarded(void){ return g_tls<64 && __readfsdword(0xE10+g_tls*4); }
static void guard(int on){ if(g_tls<64) __writefsdword(0xE10+g_tls*4,on); }

typedef NTSTATUS (NTAPI *tCreate)(PHANDLE,ACCESS_MASK,OBJATTR*,IOSB*,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
typedef NTSTATUS (NTAPI *tRW)(HANDLE,HANDLE,PVOID,PVOID,IOSB*,PVOID,ULONG,PLARGE_INTEGER,PULONG);
typedef NTSTATUS (NTAPI *tCancel)(HANDLE,IOSB*);
typedef NTSTATUS (NTAPI *tCancelEx)(HANDLE,IOSB*,IOSB*);
typedef NTSTATUS (NTAPI *tClose)(HANDLE);
static tCreate oCreate; static tRW oRead, oWrite; static tCancel oCancel; static tCancelEx oCancelEx; static tClose oClose;

static struct { HMODULE m;
    BOOL (WINAPI *Initialize)(HANDLE,PWINUSB_INTERFACE_HANDLE); BOOL (WINAPI *Free)(WINUSB_INTERFACE_HANDLE);
    BOOL (WINAPI *GetAssociatedInterface)(WINUSB_INTERFACE_HANDLE,UCHAR,PWINUSB_INTERFACE_HANDLE);
    BOOL (WINAPI *QueryInterfaceSettings)(WINUSB_INTERFACE_HANDLE,UCHAR,PUSB_INTERFACE_DESCRIPTOR);
    BOOL (WINAPI *QueryPipe)(WINUSB_INTERFACE_HANDLE,UCHAR,UCHAR,PWINUSB_PIPE_INFORMATION);
    BOOL (WINAPI *SetPipePolicy)(WINUSB_INTERFACE_HANDLE,UCHAR,ULONG,ULONG,PVOID);
    BOOL (WINAPI *ReadPipe)(WINUSB_INTERFACE_HANDLE,UCHAR,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
    BOOL (WINAPI *WritePipe)(WINUSB_INTERFACE_HANDLE,UCHAR,PUCHAR,ULONG,PULONG,LPOVERLAPPED);
    BOOL (WINAPI *AbortPipe)(WINUSB_INTERFACE_HANDLE,UCHAR); BOOL (WINAPI *ResetPipe)(WINUSB_INTERFACE_HANDLE,UCHAR); } wu;
static int wu_load(void){
    if(wu.m) return 1; HMODULE m=LoadLibraryExA("winusb.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32); if(!m) return 0;
#define F(x) if(!(*(FARPROC*)&wu.x=GetProcAddress(m,"WinUsb_" #x))) return 0;
    F(Initialize) F(Free) F(GetAssociatedInterface) F(QueryInterfaceSettings) F(QueryPipe) F(SetPipePolicy) F(ReadPipe) F(WritePipe) F(AbortPipe)
#undef F
    *(FARPROC*)&wu.ResetPipe=GetProcAddress(m,"WinUsb_ResetPipe");
    wu.m=m; return 1; }

typedef struct Pend { IOSB *iosb; HANDLE ev; unsigned char *buf; ULONG len; DWORD tid; struct Pend *next; } Pend;
#define RING 16384
#define NRD 10
typedef struct Br {
    HANDLE h; int winusb, boot; volatile LONG used, stop, gone, inited;
    volatile LONG busy; CRITICAL_SECTION wcs;
    CRITICAL_SECTION ics;
    WINUSB_INTERFACE_HANDLE wu0, wuassoc, wuio; UCHAR pin, pout; ULONG mps;
    struct { OVERLAPPED ov; unsigned char b[512]; int pend; } rs[NRD]; int rnext;
    OVERLAPPED ovw;
    OVERLAPPED ovr; IOSB riosb; unsigned char rbuf[8192];
    unsigned char ring[RING]; int rh, rt, rc;
    Pend *ph, *pt;
    CRITICAL_SECTION cs; HANDLE thread;
    int armed; DWORD armed_at; unsigned char line[160]; int linelen;
    unsigned long long rxbytes, txbytes; unsigned long nreads, nwrites, nblocked;
    unsigned long att_left;
} Br;
static Br g_br[8]; static CRITICAL_SECTION g_brlk;
static volatile LONG g_nbr;
static Br *find_br(HANDLE h){ if(!h) return NULL; for(int i=0;i<8;i++) if(g_br[i].used && g_br[i].h==h) return &g_br[i]; return NULL; }
static volatile LONG g_clockset_done;

static Br *enter_br(HANDLE h){
    EnterCriticalSection(&g_brlk); Br *b=find_br(h); if(b && !b->stop) InterlockedIncrement(&b->busy); else b=NULL; LeaveCriticalSection(&g_brlk);
    return b; }
static void leave_br(Br *b){ if(b) InterlockedDecrement(&b->busy); }

static int thread_alive(DWORD tid){
    if(tid==GetCurrentThreadId()) return 1;
    HANDLE t=OpenThread(SYNCHRONIZE,FALSE,tid); if(!t) return 0;
    int alive = WaitForSingleObject(t,0)==WAIT_TIMEOUT; CloseHandle(t); return alive; }
static void complete(Pend *p, NTSTATUS st, ULONG n){
    __try { p->iosb->Information=n; MemoryBarrier(); p->iosb->Status=st; } __except(EXCEPTION_EXECUTE_HANDLER){ return; }
    if(p->ev) SetEvent(p->ev); }

static void serve(Br *b){
    while(b->ph && b->rc>0){
        Pend *p=b->ph; b->ph=p->next; if(!b->ph) b->pt=NULL;
        if(!thread_alive(p->tid)){ logf_("parked read of exited thread %lu dropped",(unsigned long)p->tid); HeapFree(GetProcessHeap(),0,p); continue; }
        ULONG n=(ULONG)b->rc<p->len?(ULONG)b->rc:p->len; int bad=0; int rt=b->rt;
        __try { for(ULONG i=0;i<n;i++){ p->buf[i]=b->ring[rt]; rt=(rt+1)%RING; } } __except(EXCEPTION_EXECUTE_HANDLER){ bad=1; }
        if(bad){ logf_("parked read buffer not writable - read dropped"); HeapFree(GetProcessHeap(),0,p); continue; }
        b->rt=rt; b->rc-=(int)n;
        complete(p,ST_SUCCESS,n); HeapFree(GetProcessHeap(),0,p); b->nreads++;
    } }

static void fail_some(Br *b, NTSTATUS st, DWORD tid, IOSB *req, int drop){
    Pend **pp=&b->ph, *last=NULL;
    while(*pp){ Pend *p=*pp;
        if((tid && p->tid==tid) || (req && p->iosb==req)){ *pp=p->next; if(!drop && thread_alive(p->tid)) complete(p,st,0); HeapFree(GetProcessHeap(),0,p); }
        else { last=p; pp=&p->next; } }
    b->pt=last; }
static void fail_all(Br *b, NTSTATUS st){
    while(b->ph){ Pend *p=b->ph; b->ph=p->next; if(thread_alive(p->tid)) complete(p,st,0); HeapFree(GetProcessHeap(),0,p); } b->pt=NULL; }
static void ring_put(Br *b, const unsigned char *d, int n){
    for(int i=0;i<n;i++){ if(b->rc==RING){ logf_("RING FULL: %d byte(s) dropped",n-i); break; }
        b->ring[b->rh]=d[i]; b->rh=(b->rh+1)%RING; b->rc++; } }

static void feed(Br *b, const unsigned char *d, int n){
    b->rxbytes+=n;
    for(int i=0;i<n;i++){
        if(!b->armed){ ring_put(b,d+i,n-i); break; }
        if(GetTickCount()-b->armed_at>2000 || b->linelen>=(int)sizeof b->line-1){
            ring_put(b,b->line,b->linelen); b->linelen=0; b->armed=0; ring_put(b,d+i,n-i); break; }
        b->line[b->linelen++]=d[i];
        if(b->linelen>=2 && b->line[b->linelen-2]=='\r' && b->line[b->linelen-1]=='\n'){
            b->line[b->linelen]=0; char *v=strstr((char*)b->line,"main code version : ");
            if(v){ v+=20; char real[64]; int k=0; while(v[k] && v[k]!='\r' && k<63){ real[k]=v[k]; k++; } real[k]=0;
                char out[200]; int pre=(int)(v-(char*)b->line);
                _snprintf(out,sizeof out,"%.*s%s\r\n",pre,(char*)b->line,g_fakefw); out[sizeof out-1]=0;
                logf_("FIRMWARE real='%s' -> reported to EcuFlash '%s'",real,g_fakefw);
                ring_put(b,(unsigned char*)out,(int)strlen(out)); b->armed=0; }
            else ring_put(b,b->line,b->linelen);
            b->linelen=0;
        } } }

static void release_stale(Br *b){
    EnterCriticalSection(&b->cs);
    if(b->armed && GetTickCount()-b->armed_at>2000){ ring_put(b,b->line,b->linelen); b->linelen=0; b->armed=0; serve(b); }
    LeaveCriticalSection(&b->cs); }

static int wu_issue(Br *b,int i){
    b->rs[i].pend=0; ResetEvent(b->rs[i].ov.hEvent); if(b->gone||b->stop) return 0;
    if(!wu.ReadPipe(b->wuio,b->pin,b->rs[i].b,b->mps,NULL,&b->rs[i].ov) && GetLastError()!=ERROR_IO_PENDING){
        DWORD e=GetLastError(); logf_("WinUSB read failed (error %lu): cable removed?",e); InterlockedExchange(&b->gone,1); return 0; }
    b->rs[i].pend=1; return 1; }
static DWORD WINAPI reader(void *arg){
    Br *b=(Br*)arg; guard(1); int errs=0;
    if(b->winusb){
        for(int i=0;i<NRD;i++) wu_issue(b,i);
        while(!b->stop && !b->gone){
            int i=b->rnext; if(!b->rs[i].pend && !wu_issue(b,i)) break;
            if(WaitForSingleObject(b->rs[i].ov.hEvent,200)!=WAIT_OBJECT_0){ release_stale(b); continue; }
            DWORD got=0; BOOL ok=GetOverlappedResult(b->h,&b->rs[i].ov,&got,FALSE); b->rs[i].pend=0;
            if(!ok){ DWORD e=GetLastError(); if(b->stop) break;
                if(e!=ERROR_OPERATION_ABORTED){
                    int gone = e==ERROR_DEVICE_NOT_CONNECTED || e==ERROR_BAD_COMMAND || e==ERROR_GEN_FAILURE || e==ERROR_INVALID_HANDLE ||
                               e==ERROR_NO_SUCH_DEVICE || e==ERROR_FILE_NOT_FOUND || ++errs>3;
                    if(gone){ logf_("WinUSB read error %lu: cable removed",e); InterlockedExchange(&b->gone,1); break; }
                    logf_("WinUSB read error %lu (%d in a row): pipe reset",e,errs); if(wu.ResetPipe) wu.ResetPipe(b->wuio,b->pin); }
                wu_issue(b,i); b->rnext=(i+1)%NRD; continue; }
            errs=0;
            EnterCriticalSection(&b->cs); feed(b,b->rs[i].b,(int)got); serve(b); LeaveCriticalSection(&b->cs);
            wu_issue(b,i); b->rnext=(i+1)%NRD;
        }
    } else {
        while(!b->stop && !b->gone){
            ResetEvent(b->ovr.hEvent); b->riosb.Status=ST_PENDING; b->riosb.Information=0;
            LARGE_INTEGER off; off.QuadPart=0;
            NTSTATUS st=oRead(b->h,b->ovr.hEvent,NULL,NULL,&b->riosb,b->rbuf,sizeof b->rbuf-1,&off,NULL);
            if(st==ST_PENDING){ while(!b->stop && WaitForSingleObject(b->ovr.hEvent,200)!=WAIT_OBJECT_0) release_stale(b); if(b->stop) break; st=b->riosb.Status; }
            if(st<0){ if(b->stop) break; logf_("read status 0x%08lX on the vendor driver: cable removed?",(unsigned long)st); InterlockedExchange(&b->gone,1); break; }
            EnterCriticalSection(&b->cs); feed(b,b->rbuf,(int)b->riosb.Information); serve(b); LeaveCriticalSection(&b->cs);
        }
    }
    EnterCriticalSection(&b->cs); if(b->gone) fail_all(b,ST_NOT_CONNECTED); LeaveCriticalSection(&b->cs);
    return 0; }

static int wu_send(Br *b,const void *d,ULONG n){
    ResetEvent(b->ovw.hEvent); DWORD w=0;
    if(!wu.WritePipe(b->wuio,b->pout,(PUCHAR)d,n,NULL,&b->ovw)){
        if(GetLastError()!=ERROR_IO_PENDING){ DWORD e=GetLastError(); logf_("WinUSB write failed (error %lu)",e); return -1; }
        if(WaitForSingleObject(b->ovw.hEvent,2000+n*10*1000/1200)!=WAIT_OBJECT_0){     wu.AbortPipe(b->wuio,b->pout); GetOverlappedResult(b->h,&b->ovw,&w,TRUE); logf_("WinUSB write timeout"); return -2; } }
    if(!GetOverlappedResult(b->h,&b->ovw,&w,FALSE)){ DWORD e=GetLastError(); logf_("WinUSB write error %lu",e); return -1; }
    return (int)w; }
static int init_session_once(Br *b);

static int init_session(Br *b){
    if(b->inited>=2) return b->inited==2;
    EnterCriticalSection(&b->ics);
    if(!b->inited){ int ok=init_session_once(b); InterlockedExchange(&b->inited,ok?2:3); }
    LeaveCriticalSection(&b->ics);
    return b->inited==2; }
static int init_session_once(Br *b){
    guard(1);
    b->ovw.hEvent=CreateEvent(NULL,TRUE,FALSE,NULL); b->ovr.hEvent=CreateEvent(NULL,TRUE,FALSE,NULL);
    for(int i=0;i<NRD;i++) b->rs[i].ov.hEvent=CreateEvent(NULL,TRUE,FALSE,NULL);
    if(b->winusb){
        int ok=wu_load() && wu.Initialize(b->h,&b->wu0);
        if(ok){ WINUSB_INTERFACE_HANDLE c[8]; int nc=0,pick=-1; c[nc++]=b->wu0;
            for(UCHAR k=0;nc<8;k++){ WINUSB_INTERFACE_HANDLE a; if(!wu.GetAssociatedInterface(b->wu0,k,&a)) break; c[nc++]=a; }
            for(int i=0;i<nc && pick<0;i++){ USB_INTERFACE_DESCRIPTOR id; UCHAR in=0,out=0; USHORT mps=0;
                if(!wu.QueryInterfaceSettings(c[i],0,&id)) continue;
                for(UCHAR p=0;p<id.bNumEndpoints;p++){ WINUSB_PIPE_INFORMATION pi; if(!wu.QueryPipe(c[i],0,p,&pi)||pi.PipeType!=UsbdPipeTypeBulk) continue;
                    if(pi.PipeId&0x80){ if(!in){ in=pi.PipeId; mps=pi.MaximumPacketSize; } } else if(!out) out=pi.PipeId; }
                if(in&&out){ pick=i; b->wuio=c[i]; b->pin=in; b->pout=out; b->mps=(mps>=8&&mps<=512)?mps:64; } }
            for(int i=1;i<nc;i++) if(i!=pick) wu.Free(c[i]);
            if(pick>0) b->wuassoc=b->wuio; ok = pick>=0; }
        if(!ok){ logf_("WinUSB setup FAILED (error %lu) - the cable cannot be used",GetLastError()); guard(0); return 0; }
        { UCHAR on=1; ULONG t=0; wu.SetPipePolicy(b->wuio,b->pin,AUTO_CLEAR_STALL,1,&on); wu.SetPipePolicy(b->wuio,b->pin,PIPE_TRANSFER_TIMEOUT,4,&t); }
        logf_("WinUSB session: OUT 0x%02X IN 0x%02X, %lu-byte packets, %d reads pending",b->pout,b->pin,b->mps,NRD);
        if(!InterlockedExchange(&g_clockset_done,1)){
            static const int cum[13]={0,0,31,59,90,120,151,181,212,243,273,304,334}; SYSTEMTIME st; GetLocalTime(&st);
            int yday=cum[st.wMonth]+st.wDay; if(st.wYear%4==0 && st.wMonth>2) yday++;
            int tm[8]={st.wSecond,st.wMinute,st.wHour,st.wDay,st.wMonth,st.wYear,st.wDayOfWeek,yday};
            unsigned char m[44]; memcpy(m,"atp 5 34\r\n\x03\x00",12); memcpy(m+12,tm,32);
            int w=wu_send(b,m,44); unsigned char r[256]; DWORD got=0; int rl=0;
            if(w==44){ for(int t=0;t<12;t++){ ResetEvent(b->rs[0].ov.hEvent);
                    if(!wu.ReadPipe(b->wuio,b->pin,r+rl,b->mps<=(ULONG)(sizeof r-rl)?b->mps:(ULONG)(sizeof r-rl),NULL,&b->rs[0].ov) && GetLastError()!=ERROR_IO_PENDING) break;
                    if(WaitForSingleObject(b->rs[0].ov.hEvent,100)!=WAIT_OBJECT_0){ wu.AbortPipe(b->wuio,b->pin); GetOverlappedResult(b->h,&b->rs[0].ov,&got,TRUE); rl+=(int)got; break; }
                    GetOverlappedResult(b->h,&b->rs[0].ov,&got,FALSE); rl+=(int)got;

                    r[rl<(int)sizeof r?rl:(int)sizeof r-1]=0; { char *a=strstr((char*)r,"arp 5"); if(a && strchr(a,'\n')) break; }
                    if(rl>=(int)sizeof r-70){ memmove(r,r+rl-8,8); rl=8; } } }
            char pv[160]; fmt_bytes(r,rl,pv,sizeof pv); logf_("clock-set (as the vendor driver does at plug-in): write %d B, reply [%d] %s",w,rl,pv); }
    } else logf_("vendor-driver session: bridge reads/writes on the vendor driver handle (filter + version only)");
    b->thread=CreateThread(NULL,0,reader,b,0,NULL);

    if(b->thread) SetThreadPriority(b->thread,THREAD_PRIORITY_HIGHEST);
    guard(0); return 1; }

static int cable_is_winusb(void){
    static DWORD at; static int val=-1; if(val>=0 && GetTickCount()-at<2000) return val;
    HDEVINFO di=SetupDiGetClassDevsA(NULL,"USB",NULL,DIGCF_PRESENT|DIGCF_ALLCLASSES); int r=0;
    if(di!=INVALID_HANDLE_VALUE){ SP_DEVINFO_DATA dd; dd.cbSize=sizeof dd;
        for(DWORD i=0;SetupDiEnumDeviceInfo(di,i,&dd);i++){ char ids[1024]={0},svc[64]={0}; DWORD t;
            if(!SetupDiGetDeviceRegistryPropertyA(di,&dd,SPDRP_HARDWAREID,&t,(BYTE*)ids,sizeof ids-2,NULL)) continue;
            int m=0; for(char *p=ids;*p;p+=strlen(p)+1){ _strupr(p); if(strstr(p,"VID_0403&PID_CC4D") || (strstr(p,"VID_0403&PID_CC4C") && strstr(p,"&MI_00"))) m=1; }
            if(m && SetupDiGetDeviceRegistryPropertyA(di,&dd,SPDRP_SERVICE,&t,(BYTE*)svc,sizeof svc-1,NULL) && !_stricmp(svc,"WinUSB")){ r=1; break; } }
        SetupDiDestroyDeviceInfoList(di); }
    val=r; at=GetTickCount(); return r; }

static int cable_path(OBJATTR *oa){
    if(!oa||!oa->ObjectName||!oa->ObjectName->Buffer) return 0; wchar_t b[401]; int n=oa->ObjectName->Length/2; if(n>400) n=400;
    for(int i=0;i<n;i++) b[i]=towlower(oa->ObjectName->Buffer[i]); b[n]=0;
    if(g_testpath[0] && wcsstr(b,g_testpath)) return 3;
    if(wcsstr(b,L"vid_0403&pid_cc4d") || (wcsstr(b,L"vid_0403&pid_cc4c") && wcsstr(b,L"&mi_00"))) return 1;
    return wcsstr(b,L"vid_0403&pid_cc4b")?2 : 0; }

static int afd_path(OBJATTR *oa){
    if(!oa||!oa->ObjectName||!oa->ObjectName->Buffer) return 0; int n=oa->ObjectName->Length/2; const wchar_t *w=oa->ObjectName->Buffer;
    static const wchar_t pre[]=L"\\device\\afd"; int k=(int)(sizeof pre/sizeof pre[0])-1; if(n<k) return 0;
    for(int i=0;i<k;i++) if(towlower(w[i])!=pre[i]) return 0;
    return n==k || w[k]==L'\\'; }
static NTSTATUS NTAPI hCreate(PHANDLE ph,ACCESS_MASK a,OBJATTR *oa,IOSB *io,PLARGE_INTEGER al,ULONG fa,ULONG sh,ULONG cd,ULONG co,PVOID ea,ULONG el){
    if(g_blocknet && !guarded() && afd_path(oa)){
        LONG k=InterlockedIncrement(&g_netblocked);
        if(k<=3 || k%100==0){ guard(1); logf_("*** INTERNET BLOCKED: network socket #%ld refused (BlockInternet=1)",k); guard(0); }
        if(io){ io->Information=0; io->Status=(NTSTATUS)0xC0000022L; }
        return (NTSTATUS)0xC0000022L; }
    NTSTATUS st=oCreate(ph,a,oa,io,al,fa,sh,cd,co,ea,el);
    int kind;
    if(guarded() || !(kind=cable_path(oa))) return st;
    guard(1);
    int sync=(co&0x30)!=0; int boot=kind==2;
    if(st<0) logf_("open cable (%s, share %lu) -> status 0x%08lX",sync?"probe":"session",sh,(unsigned long)st);
    else if(!sync){
        int win=(kind==1)?cable_is_winusb():0;
        EnterCriticalSection(&g_brlk); Br *b=NULL; for(int i=0;i<8;i++) if(!g_br[i].used){ b=&g_br[i]; break; }
        if(b){ memset(b,0,sizeof *b); b->h=*ph; b->boot=boot; b->winusb=win && !boot; InitializeCriticalSection(&b->cs); InitializeCriticalSection(&b->wcs);
               InitializeCriticalSection(&b->ics); b->used=1; InterlockedIncrement(&g_nbr); }
        LeaveCriticalSection(&g_brlk);
        if(b) logf_("open cable h=%p%s via %s",*ph,boot?" (BOOTLOADER, PID CC4B)":kind==3?" (TEST device)":"",b->winusb?"WinUSB (bridged)":"vendor driver (filtered)"); else logf_("open cable: no free bridge slot");
    }
    guard(0); return st; }
static NTSTATUS NTAPI hRead(HANDLE h,HANDLE ev,PVOID apc,PVOID ctx,IOSB *io,PVOID buf,ULONG len,PLARGE_INTEGER off,PULONG key){
    Br *b=guarded()?NULL:enter_br(h); if(!b) return oRead(h,ev,apc,ctx,io,buf,len,off,key);
    ev=(HANDLE)((ULONG_PTR)ev & ~(ULONG_PTR)1);
    if(!init_session(b) || b->gone){ io->Information=0; io->Status=ST_NOT_CONNECTED; if(ev) SetEvent(ev); leave_br(b); return ST_NOT_CONNECTED; }
    if(apc) logf_("NOTE: read with an APC routine (unexpected)");
    EnterCriticalSection(&b->cs);
    if(b->gone){ LeaveCriticalSection(&b->cs); io->Information=0; io->Status=ST_NOT_CONNECTED; if(ev) SetEvent(ev); leave_br(b); return ST_NOT_CONNECTED; }
    if(b->rc>0 && !b->ph){ ULONG n=(ULONG)b->rc<len?(ULONG)b->rc:len; unsigned char *d=(unsigned char*)buf;
        for(ULONG i=0;i<n;i++){ d[i]=b->ring[b->rt]; b->rt=(b->rt+1)%RING; } b->rc-=(int)n; b->nreads++;
        LeaveCriticalSection(&b->cs); io->Information=n; io->Status=ST_SUCCESS; if(ev) SetEvent(ev); leave_br(b); return ST_SUCCESS; }
    Pend *p=(Pend*)HeapAlloc(GetProcessHeap(),0,sizeof *p);
    if(!p){ LeaveCriticalSection(&b->cs); leave_br(b); return (NTSTATUS)0xC000009AL; }
    p->iosb=io; p->ev=ev; p->buf=(unsigned char*)buf; p->len=len; p->tid=GetCurrentThreadId(); p->next=NULL;
    io->Information=0; io->Status=ST_PENDING; if(ev) ResetEvent(ev);
    if(b->pt) b->pt->next=p; else b->ph=p; b->pt=p;
    LeaveCriticalSection(&b->cs); leave_br(b); return ST_PENDING; }
static int blocked_one(const char *c, char *why){
    if(g_testblock[0] && !strncmp(c,g_testblock,strlen(g_testblock))){ strcpy(why,"TestBlock"); return 1; }
    if(!g_block) return 0;
    if(!strncmp(c,"atx",3)){ strcpy(why,"enter bootloader (firmware update)"); return 1; }
    if(!strncmp(c,"atfw",4)){ strcpy(why,"firmware write"); return 1; }
    if(c[0]=='t' && c[1]=='b' && c[2]!='i' && c[2]!='r' && c[2]!='x'){ strcpy(why,"bootloader program/erase"); return 1; }
    return 0; }

static unsigned long att_short(const unsigned char *d, ULONG n){
    ULONG i=0; while(i<n && (d[i]=='\r'||d[i]=='\n')) i++;
    if(n-i<5 || d[i]!='a' || d[i+1]!='t' || d[i+2]!='t') return 0;
    ULONG e=i; while(e<n && e<i+64 && d[e]!='\n') e++;
    if(e>=n || d[e]!='\n') return 0;
    ULONG k=i+3; while(k<e && d[k]>='0' && d[k]<='9') k++; while(k<e && d[k]==' ') k++;
    unsigned long ds=0; int dig=0; while(k<e && d[k]>='0' && d[k]<='9' && dig<6){ ds=ds*10+(unsigned long)(d[k]-'0'); k++; dig++; }
    if(!dig || ds>4128) return 0;
    ULONG carried=n-(e+1); return ds>carried ? ds-carried : 0; }

static int scan_write(const unsigned char *d, ULONG n, char *why, unsigned long *att_left){
    ULONG i=0; *att_left=0;
    for(int line=0; line<64; line++){
        while(i<n && (d[i]=='\r'||d[i]=='\n')) i++;
        if(i>=n) return 0;
        char c[8]={0}; for(int k=0;k<6 && i+k<n;k++) c[k]=(char)tolower(d[i+k]);
        if(blocked_one(c,why)) return 1;
        if(c[0]=='a' && c[1]=='t'){
            if(c[2]=='t'){ *att_left=att_short(d+i,n-i); return 0; }
            if(c[2]=='f'||c[2]=='m'||c[2]=='y'||c[2]=='p') return 0; }
        else if(!(c[0]=='t' && c[1]=='b')) return 0;
        while(i<n && d[i]!='\n') i++; }
    return 0; }
static NTSTATUS NTAPI hWrite(HANDLE h,HANDLE ev,PVOID apc,PVOID ctx,IOSB *io,PVOID buf,ULONG len,PLARGE_INTEGER off,PULONG key){
    Br *b=guarded()?NULL:enter_br(h); if(!b) return oWrite(h,ev,apc,ctx,io,buf,len,off,key);
    ev=(HANDLE)((ULONG_PTR)ev & ~(ULONG_PTR)1);
    const unsigned char *d=(const unsigned char*)buf; char why[64]; char pv[200];
    ULONG cont=0;
    EnterCriticalSection(&b->wcs);
    if(b->att_left){ cont = len<b->att_left ? len : (ULONG)b->att_left; b->att_left-=cont;
        logf_("write continues att data: %lu B (%lu still to come)", (unsigned long)cont, b->att_left); }
    int blk = cont<len && scan_write(d+cont,len-cont,why,&b->att_left);
    LeaveCriticalSection(&b->wcs);
    if(blk){ fmt_bytes(d,(int)len,pv,sizeof pv); b->nblocked++;
        logf_("*** BLOCKED (%s)%s: %s - not sent to the cable; reported as written",why,b->boot?" [cable in bootloader]":"",pv);
        io->Information=len; io->Status=ST_SUCCESS; if(ev) SetEvent(ev); leave_br(b); return ST_SUCCESS; }
    if(!init_session(b) || b->gone){ io->Information=0; io->Status=ST_NOT_CONNECTED; if(ev) SetEvent(ev); leave_br(b); return ST_NOT_CONNECTED; }
    for(ULONG i=cont;i+3<=len && i<cont+12;i++) if(d[i]=='a'&&d[i+1]=='t'&&d[i+2]=='i'&&(i+3==len||d[i+3]=='\r'||d[i+3]=='\n')){
        EnterCriticalSection(&b->cs); b->armed=1; b->armed_at=GetTickCount(); b->linelen=0; LeaveCriticalSection(&b->cs); break; }
    b->nwrites++; b->txbytes+=len;
    if(!b->winusb){ NTSTATUS st=oWrite(h,ev,apc,ctx,io,buf,len,off,key); leave_br(b); return st; }
    guard(1); EnterCriticalSection(&b->wcs); int w=wu_send(b,buf,len); LeaveCriticalSection(&b->wcs); guard(0);
    if(w<0){ if(w==-1) InterlockedExchange(&b->gone,1); io->Information=0; io->Status=w==-2?(NTSTATUS)0xC00000B5L:ST_NOT_CONNECTED; if(ev) SetEvent(ev); leave_br(b); return io->Status; }
    io->Information=(ULONG)w; io->Status=ST_SUCCESS; if(ev) SetEvent(ev); leave_br(b); return ST_SUCCESS; }
static void cancel_reads(Br *b){ EnterCriticalSection(&b->cs); fail_all(b,ST_CANCELLED); LeaveCriticalSection(&b->cs); }
static NTSTATUS NTAPI hCancel(HANDLE h,IOSB *io){
    Br *b=guarded()?NULL:enter_br(h); if(!b) return oCancel(h,io);
    EnterCriticalSection(&b->cs); fail_some(b,ST_CANCELLED,GetCurrentThreadId(),NULL,0); LeaveCriticalSection(&b->cs);
    if(io){ io->Information=0; io->Status=ST_SUCCESS; } leave_br(b); return ST_SUCCESS; }
static NTSTATUS NTAPI hCancelEx(HANDLE h,IOSB *req,IOSB *io){
    Br *b=guarded()?NULL:enter_br(h); if(!b) return oCancelEx(h,req,io);
    EnterCriticalSection(&b->cs); if(req) fail_some(b,ST_CANCELLED,0,req,0); else fail_all(b,ST_CANCELLED); LeaveCriticalSection(&b->cs);
    if(io){ io->Information=0; io->Status=ST_SUCCESS; } leave_br(b); return ST_SUCCESS; }
static NTSTATUS NTAPI hClose(HANDLE h){
    Br *b=NULL; if(!guarded()){ EnterCriticalSection(&g_brlk); b=find_br(h); if(b && InterlockedExchange(&b->stop,1)) b=NULL; LeaveCriticalSection(&g_brlk); }
    if(!b) return oClose(h);
    guard(1);
    cancel_reads(b);

    for(int i=0; b->busy && i<5000; i++){ cancel_reads(b); if(i==100 && b->wuio) wu.AbortPipe(b->wuio,b->pout); Sleep(1); }
    int ended = !b->busy;
    if(b->wuio){ wu.AbortPipe(b->wuio,b->pin); wu.AbortPipe(b->wuio,b->pout); }
    if(b->thread){ CancelIoEx(b->h,NULL); if(WaitForSingleObject(b->thread,5000)!=WAIT_OBJECT_0) ended=0; CloseHandle(b->thread); }
    if(b->inited>=2) logf_("close cable h=%p: %lu writes / %llu B, %lu reads served, %llu B received, %lu blocked%s",
                        h,b->nwrites,b->txbytes,b->nreads,b->rxbytes,b->nblocked,b->gone?", cable was removed":"");
    if(b->gone) InterlockedExchange(&g_clockset_done,0);
    if(!ended){

        logf_("WARNING: the cable session did not stop within 5 s; its slot is retired (small leak) to stay safe");
        EnterCriticalSection(&g_brlk); b->h=NULL; LeaveCriticalSection(&g_brlk);
        guard(0); return oClose(h); }

    if(b->wuio) wu.AbortPipe(b->wuio,b->pin);
    for(int i=0;i<NRD;i++){ DWORD g;
        if(b->rs[i].pend && b->rs[i].ov.hEvent && WaitForSingleObject(b->rs[i].ov.hEvent,2000)==WAIT_OBJECT_0) GetOverlappedResult(b->h,&b->rs[i].ov,&g,FALSE);
        if(b->rs[i].ov.hEvent) CloseHandle(b->rs[i].ov.hEvent); }
    if(b->wuassoc) wu.Free(b->wuassoc); if(b->wu0) wu.Free(b->wu0);
    if(b->ovw.hEvent) CloseHandle(b->ovw.hEvent); if(b->ovr.hEvent) CloseHandle(b->ovr.hEvent);
    EnterCriticalSection(&g_brlk); b->used=0; InterlockedDecrement(&g_nbr); LeaveCriticalSection(&g_brlk);
    DeleteCriticalSection(&b->cs); DeleteCriticalSection(&b->wcs); DeleteCriticalSection(&b->ics);
    guard(0);
    return oClose(h); }

#define MAXSTUB 1024
static struct { char name[48]; unsigned char *stub; unsigned nargs; volatile LONG n; } g_s[MAXSTUB]; static int g_ns;
void __cdecl other_pre(DWORD *a, DWORD idx){
    if(!g_nbr || guarded()) return; unsigned na=g_s[idx].nargs>12?12:g_s[idx].nargs; Br *b=NULL; int at=-1;
    for(unsigned k=0;k<na && !b;k++){ b=find_br((HANDLE)a[k]); at=(int)k; }
    if(!b) return;
    DWORD le=__readfsdword(0x34); guard(1);
    if(InterlockedIncrement(&g_s[idx].n)<=20) logf_("UNEXPECTED %s on the cable handle (arg #%d) - passed through unchanged",g_s[idx].name,at);
    guard(0); __writefsdword(0x34,le); }
static unsigned char *g_pool;
static void *thunk(int idx, unsigned char *stub){
    unsigned char *t=g_pool+idx*48, *p=t;
    *p++=0x60; *p++=0x68; *(DWORD*)p=(DWORD)idx; p+=4; *p++=0x8D; *p++=0x44; *p++=0x24; *p++=0x28; *p++=0x50;
    *p++=0xB8; *(DWORD*)p=(DWORD)other_pre; p+=4; *p++=0xFF; *p++=0xD0; *p++=0x83; *p++=0xC4; *p++=0x08; *p++=0x61;
    memcpy(p,stub+5,5); p+=5; *p++=0xE9; *(LONG*)p=(LONG)((stub+10)-(p+4)); return t; }
static void *tramp(unsigned char *stub){
    unsigned char *t=(unsigned char*)VirtualAlloc(NULL,32,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE); if(!t) return NULL;
    memcpy(t,stub,10); t[10]=0xE9; *(LONG*)(t+11)=(LONG)((stub+10)-(t+15)); return t; }
static int install(void){
    unsigned char *base=(unsigned char*)GetModuleHandleA("ntdll.dll");
    IMAGE_NT_HEADERS *nh=(IMAGE_NT_HEADERS*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_EXPORT_DIRECTORY *ed=(IMAGE_EXPORT_DIRECTORY*)(base+nh->OptionalHeader.DataDirectory[0].VirtualAddress);
    DWORD *names=(DWORD*)(base+ed->AddressOfNames); WORD *ords=(WORD*)(base+ed->AddressOfNameOrdinals); DWORD *funcs=(DWORD*)(base+ed->AddressOfFunctions);
    for(DWORD i=0;i<ed->NumberOfNames && g_ns<MAXSTUB;i++){
        const char *nm=(const char*)(base+names[i]); if(nm[0]!='N'||nm[1]!='t') continue; unsigned char *p=base+funcs[ords[i]];
        if(!(p[0]==0xB8 && p[5]==0xBA && p[10]==0xFF && p[11]==0xD2 && (p[12]==0xC2||p[12]==0xC3))) continue;
        if(!strcmp(nm,"NtContinue")||!strcmp(nm,"NtCallbackReturn")||!strcmp(nm,"NtRaiseException")||!strcmp(nm,"NtTestAlert")) continue;
        strncpy(g_s[g_ns].name,nm,47); g_s[g_ns].stub=p; g_s[g_ns].nargs=p[12]==0xC2?(*(WORD*)(p+13))/4:0; g_ns++; }
    g_pool=(unsigned char*)VirtualAlloc(NULL,MAXSTUB*48,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE); if(!g_pool) return 0;
    static void *target[MAXSTUB]; int special=0;
    for(int i=0;i<g_ns;i++){ const char *n=g_s[i].name; void **o=NULL; void *hk=NULL;
        if(!strcmp(n,"NtCreateFile")){ o=(void**)&oCreate; hk=hCreate; } else if(!strcmp(n,"NtReadFile")){ o=(void**)&oRead; hk=hRead; }
        else if(!strcmp(n,"NtWriteFile")){ o=(void**)&oWrite; hk=hWrite; } else if(!strcmp(n,"NtCancelIoFile")){ o=(void**)&oCancel; hk=hCancel; }
        else if(!strcmp(n,"NtCancelIoFileEx")){ o=(void**)&oCancelEx; hk=hCancelEx; } else if(!strcmp(n,"NtClose")){ o=(void**)&oClose; hk=hClose; }
        if(o){ *o=tramp(g_s[i].stub); target[i]=*o?hk:NULL; special++; } else target[i]=thunk(i,g_s[i].stub); }
    if(special!=6 || !oCreate||!oRead||!oWrite||!oCancel||!oCancelEx||!oClose) return 0;
    int ok=0; for(int i=0;i<g_ns;i++){ if(!target[i]) continue; unsigned char *p=g_s[i].stub; DWORD old;
        if(!VirtualProtect(p+5,5,PAGE_EXECUTE_READWRITE,&old)) continue;
        unsigned char j[5]; j[0]=0xE9; *(LONG*)(j+1)=(LONG)((unsigned char*)target[i]-(p+10)); memcpy(p+5,j,5);
        VirtualProtect(p+5,5,old,&old); ok++; }
    FlushInstructionCache(GetCurrentProcess(),NULL,0);
    return ok; }

static void read_ini(HINSTANCE hi){
    char p[MAX_PATH]; GetModuleFileNameA(hi,p,MAX_PATH); char *s=strrchr(p,'\\'); if(!s) return; strcpy(s+1,"tactrifree-bridge.ini");
    char v[32]; GetPrivateProfileStringA("bridge","FirmwareVersion",g_fakefw,v,sizeof v,p);
    if(strlen(v)>=5 && strlen(v)<15) strcpy(g_fakefw,v);
    g_block=GetPrivateProfileIntA("bridge","BlockFirmware",1,p)!=0;
    g_blocknet=GetPrivateProfileIntA("bridge","BlockInternet",1,p)!=0;
    GetPrivateProfileStringA("bridge","TestBlock","",g_testblock,sizeof g_testblock,p); _strlwr(g_testblock);
    { char tp[64]; GetPrivateProfileStringA("bridge","TestPath","",tp,sizeof tp,p); _strlwr(tp);
      MultiByteToWideChar(CP_ACP,0,tp,-1,g_testpath,64); } }

#define BRIDGE_VERSION "TactriFree EcuFlash bridge 1.0 beta3 (0.12, 2026-11-09)"
__declspec(dllexport) const char * __cdecl TactriFreeBridgeVersion(void){ return BRIDGE_VERSION; }

BOOL WINAPI DllMain(HINSTANCE hi, DWORD r, LPVOID rs){ (void)rs;
    if(r==DLL_THREAD_DETACH){
        if(g_tls<64){ DWORD me=GetCurrentThreadId();
            for(int i=0;i<8;i++){ Br *b=NULL;
                EnterCriticalSection(&g_brlk); if(g_br[i].used && !g_br[i].stop && g_br[i].ph){ b=&g_br[i]; InterlockedIncrement(&b->busy); } LeaveCriticalSection(&g_brlk);
                if(!b) continue;
                EnterCriticalSection(&b->cs); fail_some(b,ST_CANCELLED,me,NULL,1); LeaveCriticalSection(&b->cs); leave_br(b); } }
        return TRUE; }
    if(r==DLL_PROCESS_ATTACH){

        { HMODULE pin; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,(LPCSTR)(void*)DllMain,&pin); }
#ifdef AS_GLU32
        { wchar_t sys[MAX_PATH]; UINT n=GetSystemDirectoryW(sys,MAX_PATH); wcscpy(sys+n,L"\\glu32.dll");
          HMODULE g=LoadLibraryW(sys); for(int i=0;i<GLU_N;i++) g_glu[i]=g?(void*)GetProcAddress(g,g_glu_names[i]):NULL; }
#endif
        InitializeCriticalSection(&g_loglk); InitializeCriticalSection(&g_brlk); QueryPerformanceFrequency(&g_qpf);
        g_tls=TlsAlloc(); read_ini(hi); log_open();
        char host[MAX_PATH]; GetModuleFileNameA(NULL,host,MAX_PATH);
        if(g_tls>=64){ logf_("TactriFree bridge: TLS slot unavailable - bridge NOT active in %s",host); return TRUE; }
        int n=install();
        logf_("==== %s attach: %s | hooks %d | firmware reported '%s' | firmware block %s | internet block %s%s%s ====",
              BRIDGE_VERSION,host,n,g_fakefw,g_block?"ON":"OFF",g_blocknet?"ON":"OFF",g_testblock[0]?" | TestBlock=":"",g_testblock);
        if(n<=0) logf_("hooks NOT installed - EcuFlash runs unbridged");
    }
    return TRUE; }
