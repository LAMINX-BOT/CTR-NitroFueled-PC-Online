/* Online play on a RyuNET server: the system services a signed-in PS4 offers, from
 * Ryu7w7/OCTR-NitroFueled-PC (GPL-2.0, its src/runtime_services.c): NetCtl, Net, Http and Ssl
 * (runtime_net.c, runtime_http.c), NpManager, NpAuth and NpWebApi (runtime_np_online.c and the
 * sessions, friends and invitations below), session invitations accepted from the launcher or a
 * friend (sceSystemServiceGetStatus/ReceiveEvent), the Invite button and the UserService event
 * callback NpToolkit2 waits for.
 *
 * Off unless BB_ONLINE=1 (run.py setup_online): runtime_net_resolve then answers nothing and
 * runtime_services.c's offline contracts stay (cable unplugged, signed out of PSN). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#else
#include <arpa/inet.h>
#endif

#define USER_ID 1
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_EINVAL ((int32_t)0x80410116)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=0x10000; /* apart from runtime_services.c's ids */
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }

/* OCTR's private-room game mode (Normal / Speed / cups, its runtime_room_mode.c) is not part of
 * this edition: every room races in the game's own mode. */
static void runtime_room_mode_joined(const char *session) { (void)session; }
static void runtime_room_mode_hosting(void) {}
static void runtime_room_mode_left(void) {}

/* ---- Session invitations (RyuNET, accepted in Discord) ----
 * When the player accepts a Discord invite, the launcher writes ryunet_join.txt next to
 * bbport.ini: "session=<PSN session id>\nhost=<nickname>\ntime=<unix seconds>\n". The game
 * hears about it the way a PS4 tells it about an invitation accepted in the system UI:
 * sceSystemServiceGetStatus counts a pending event and sceSystemServiceReceiveEvent hands it
 * SESSION_INVITATION (0x10000002), whose param is SceNpSessionInvitationEventParam
 * (shadPS4 invitation_dialog.h, static_asserted offsets/sizes there):
 *   +0x00 sessionId OrbisNpSessionId (char[45]+term+pad = 48)
 *   +0x30 invitationId OrbisNpInvitationId (char[60]+term = 61)
 *   +0x70 flag u32 (1 = invitation)
 *   +0x74 padding[4]
 *   +0x78 onlineId OrbisNpOnlineId (char[16]+term+dummy = 20) of the local user who accepted
 *   +0x8C userId s32 of the local user who accepted (USER_ID, single-user system)
 *   +0x90 referralOnlineId (24) of the inviter
 *   +0xA8 referralAccountId u64 of the inviter
 * (176 bytes total: the game copies exactly 0xB0, eboot 0xb89d44.) The userId
 * MUST be the accepting local user: with 0 the game reports "not the active
 * user" and retries the join forever. The game drops an invitation that comes
 * while it is busy (0xa99af0), so it is offered again every few seconds until the game joins
 * that session (POST /v1/sessions/<id>/members, see invite_session_joined) or it expires. */
const char *runtime_np_get_online_name(void);
uint64_t runtime_np_calculate_account_id(const char *name);
#define SYSTEM_EVENT_SESSION_INVITATION 0x10000002
/* taken: the game asked for the invited session (GET /v1/sessions/<id>), so it acted on the
 * invitation; it is not offered again (each offer it acts on shows its own message). */
static struct { char session[64]; char host[32]; time_t expires, next_offer; int active, taken; } s_invite;
static pthread_mutex_t s_invite_lock=PTHREAD_MUTEX_INITIALIZER;

static int invite_text_ok(const char *s, size_t max) {
    size_t n=strlen(s);
    if (!n || n>max) return 0;
    for (size_t i=0;i<n;++i) {
        const char c=s[i];
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return 0;
    }
    return 1;
}

/* Picks up ryunet_join.txt (at most once a second) and removes it. */
static void poll_invite_file(void) {
    static time_t last_check;
    const time_t now=time(NULL);
    if (now==last_check) return;
    last_check=now;
    char path[600];
    const char *cfg=getenv("BB_CONFIG");
    const char *slash=cfg ? strrchr(cfg,'/') : NULL;
    const char *bslash=cfg ? strrchr(cfg,'\\') : NULL;
    if (bslash>slash) slash=bslash;
    if (slash) snprintf(path,sizeof path,"%.*s/ryunet_join.txt",(int)(slash-cfg),cfg);
    else snprintf(path,sizeof path,"ryunet_join.txt");
    FILE *f=fopen(path,"r");
    if (!f) return;
    char line[256], session[64]="", host[32]="";
    long long written=0;
    while (fgets(line,sizeof line,f)) {
        line[strcspn(line,"\r\n")]=0;
        if (!strncmp(line,"session=",8)) snprintf(session,sizeof session,"%s",line+8);
        else if (!strncmp(line,"host=",5)) snprintf(host,sizeof host,"%s",line+5);
        else if (!strncmp(line,"time=",5)) written=atoll(line+5);
    }
    fclose(f);
    remove(path);
    /* A stale file (a game that never started) must not join a match hours later. */
    if (!invite_text_ok(session,45) || !invite_text_ok(host,16) || written<(long long)now-300 || written>(long long)now+60) {
        printf("Runtime: ignored an invitation file (session='%s' host='%s' age=%llds)\n",session,host,(long long)now-written);
        return;
    }
    pthread_mutex_lock(&s_invite_lock);
    snprintf(s_invite.session,sizeof s_invite.session,"%s",session);
    snprintf(s_invite.host,sizeof s_invite.host,"%s",host);
    s_invite.expires=now+240;  /* time for a game started by the invite to reach the menus */
    s_invite.next_offer=now;
    s_invite.active=1;
    s_invite.taken=0;
    pthread_mutex_unlock(&s_invite_lock);
    printf("Runtime: invitation from %s accepted in Discord: joining session %s\n",host,session);
}

/* An invitation accepted in the menu's Friends tab (runtime_friends.c): offered to the game
 * like one accepted in Discord. */
void runtime_invite_join(const char *host) {
    if (!invite_text_ok(host,16)) return;
    const time_t now=time(NULL);
    pthread_mutex_lock(&s_invite_lock);
    snprintf(s_invite.session,sizeof s_invite.session,"cusa13795-session-%s",host);
    snprintf(s_invite.host,sizeof s_invite.host,"%s",host);
    s_invite.expires=now+240;
    s_invite.next_offer=now;
    s_invite.active=1;
    s_invite.taken=0;
    pthread_mutex_unlock(&s_invite_lock);
    printf("Runtime: invitation from %s accepted in the Friends tab: joining their session\n",host);
}

static int invite_due(void) {
    poll_invite_file();
    const time_t now=time(NULL);
    pthread_mutex_lock(&s_invite_lock);
    if (s_invite.active && now>=s_invite.expires) {
        s_invite.active=0;
        printf("Runtime: invitation from %s expired: the game never joined it\n",s_invite.host);
    }
    const int due=s_invite.active && !s_invite.taken && now>=s_invite.next_offer;
    pthread_mutex_unlock(&s_invite_lock);
    return due;
}

/* The game asked for this session: it acts on the invitation, which is not offered again. */
static void invite_session_taken(const char *session) {
    pthread_mutex_lock(&s_invite_lock);
    if (s_invite.active && !s_invite.taken && session && !strcmp(session,s_invite.session)) {
        s_invite.taken=1;
        printf("Runtime: the game took the invitation from %s (reading its session)\n",s_invite.host);
    }
    pthread_mutex_unlock(&s_invite_lock);
}

/* The game joined a session: an invitation to it is done. */
static void invite_session_joined(const char *path) {
    pthread_mutex_lock(&s_invite_lock);
    if (s_invite.active && path && strstr(path,s_invite.session)) {
        s_invite.active=0;
        printf("Runtime: joined %s's session from the Discord invitation\n",s_invite.host);
    }
    pthread_mutex_unlock(&s_invite_lock);
}

static ABI int32_t system_status(unsigned char *status) {
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* event_num=0, no overlay, foreground, normal CPU mode */
    if (invite_due()) *(int32_t *)status=1;
    return 0;
}
static ABI int32_t system_event(void *event) {
    if (!event) return SYSTEM_PARAMETER;
    const time_t now=time(NULL);
    pthread_mutex_lock(&s_invite_lock);
    if (!s_invite.active || s_invite.taken || now<s_invite.next_offer) { pthread_mutex_unlock(&s_invite_lock); return SYSTEM_NO_EVENT; }
    s_invite.next_offer=now+4;
    unsigned char *e=(unsigned char *)event;
    memset(e,0,4+0xB0);
    *(int32_t *)e=SYSTEM_EVENT_SESSION_INVITATION;
    unsigned char *param=e+4;
    snprintf((char *)param+0x00,45+1,"%s",s_invite.session);
    snprintf((char *)param+0x30,60+1,"inv-%s",s_invite.host);
    *(uint32_t *)(param+0x70)=1; /* SCE_NP_SESSION_INVITATION_EVENT_FLAG_INVITATION */
    const char *me=runtime_np_get_online_name();
    snprintf((char *)param+0x78,16+1,"%s",me ? me : "");
    *(int32_t *)(param+0x8C)=(int32_t)USER_ID; /* accepting local user: 0 reads as "not the active user" */
    snprintf((char *)param+0x90,16+1,"%s",s_invite.host); /* referral = inviter */
    *(uint64_t *)(param+0xA8)=runtime_np_calculate_account_id(s_invite.host);
    printf("Runtime: SESSION_INVITATION offered to the game (session %s, from %s)\n",s_invite.session,s_invite.host);
    pthread_mutex_unlock(&s_invite_lock);
    return 0;
}

#include "runtime_net.h"
#include "runtime_http.h"
#include "runtime_np_online.h"
#include "runtime_friends.h"

/* ---- NetCtl / Net / Http / Ssl ---- */
static ABI int32_t net_init(void) { return runtime_net_init(); }
static ABI int32_t net_term(void) { return runtime_net_term(); }
static ABI int32_t *net_errno_loc(void) { return runtime_net_errno_loc(); }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_socket(const char *name,int domain,int type,int protocol) { return runtime_net_socket(name,domain,type,protocol); }
static ABI int32_t net_connect(int s,const void *addr,uint32_t addrlen) { return runtime_net_connect(s,(const OrbisNetSockaddr*)addr,addrlen); }
static ABI int32_t net_bind(int s,const void *addr,uint32_t addrlen) { return runtime_net_bind(s,(const OrbisNetSockaddr*)addr,addrlen); }
static ABI int32_t net_listen(int s,int backlog) { return runtime_net_listen(s,backlog); }
static ABI int32_t net_accept(int s,void *addr,uint32_t *addrlen) { return runtime_net_accept(s,(OrbisNetSockaddr*)addr,addrlen); }
static ABI int32_t net_send(int s,const void *msg,size_t len,int flags) { return runtime_net_send(s,msg,len,flags); }
static ABI int32_t net_sendto(int s,const void *msg,size_t len,int flags,const void *to,uint32_t tolen) { return runtime_net_sendto(s,msg,len,flags,(const OrbisNetSockaddr*)to,tolen); }
static ABI int32_t net_recv(int s,void *buf,size_t len,int flags) { return runtime_net_recv(s,buf,len,flags); }
static ABI int32_t net_recvfrom(int s,void *buf,size_t len,int flags,void *from,uint32_t *fromlen) { return runtime_net_recvfrom(s,buf,len,flags,(OrbisNetSockaddr*)from,fromlen); }
static ABI int32_t net_setsockopt(int s,int level,int optname,const void *optval,uint32_t optlen) { return runtime_net_setsockopt(s,level,optname,optval,optlen); }
static ABI int32_t net_getsockopt(int s,int level,int optname,void *optval,uint32_t *optlen) { return runtime_net_getsockopt(s,level,optname,optval,optlen); }
static ABI int32_t net_getsockname(int s,void *name,uint32_t *namelen) { return runtime_net_getsockname(s,(OrbisNetSockaddr*)name,namelen); }
static ABI int32_t net_shutdown(int s,int how) { return runtime_net_shutdown(s,how); }
static ABI int32_t net_close(int s) { return runtime_net_socket_close(s); }
static ABI int32_t net_abort(int s,int flags) { return runtime_net_socket_abort(s,flags); }

static ABI int32_t net_epoll_create(const char *name,int flags) { return runtime_net_epoll_create(name,flags); }
static ABI int32_t net_epoll_destroy(int id) { return runtime_net_epoll_destroy(id); }
static ABI int32_t net_epoll_control(int eid,int op,int s,void *event) { return runtime_net_epoll_control(eid,op,s,(OrbisNetEpollEvent*)event); }
static ABI int32_t net_epoll_wait(int eid,void *events,int maxevents,int timeout) { return runtime_net_epoll_wait(eid,(OrbisNetEpollEvent*)events,maxevents,timeout); }
static ABI int32_t net_epoll_abort(int eid,int flags) { return runtime_net_epoll_abort(eid,flags); }

static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { return runtime_net_resolver_create(name,pool,flags); }
static ABI int32_t net_resolver_destroy(int id) { return runtime_net_resolver_destroy(id); }
static ABI int32_t net_resolver_start_ntoa(int rid,const char *hostname,void *addr,int timeout,int retry,int flags) { return runtime_net_resolver_start_ntoa(rid,hostname,(struct in_addr*)addr,timeout,retry,flags); }
static ABI int32_t net_resolver_start_aton(int rid,struct in_addr addr,char *hostname,size_t hostlen,int timeout,int retry,int flags) { return runtime_net_resolver_start_aton(rid,addr,hostname,hostlen,timeout,retry,flags); }
static ABI int32_t net_resolver_abort(int rid,int flags) { return runtime_net_resolver_abort(rid,flags); }

static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}

static ABI int32_t netctl_init(void) { return runtime_netctl_init(); }
static ABI int32_t netctl_term(void) { return runtime_netctl_term(); }
static ABI int32_t netctl_state(int32_t *state) { return runtime_netctl_state(state); }
static ABI int32_t netctl_info(int code,void *info) { return runtime_netctl_info(code,info); }
static ABI int32_t netctl_nat(uint32_t *info) { return runtime_netctl_nat((OrbisNetCtlNatInfo*)info); }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { return runtime_netctl_register(cb,arg,cid); }
static ABI int32_t netctl_unregister(int cid) { return runtime_netctl_unregister(cid); }
static ABI int32_t netctl_check(void) { return runtime_netctl_check(); }

static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t ok_void(void) { return 0; }

/* NpWebApi push events: none are sent. Logged (each filter's first data type, the string its
 * parameters start with). NpToolkit2 listens only for presence ("np:service:presence:onlineStatus"
 * and, for "inGamePresence", "np:service:presence:gameTitleInfo"), not for friend list changes:
 * the game reads its friend list once, at sign-in. */
static ABI int32_t np_push_filter(int32_t lib, int32_t handle, const char *service, uint32_t label,
                                  const char *params, size_t count) {
    const int32_t id=new_id();
    printf("Runtime: NpWebApi push filter %d (lib %d, handle %d, service '%s', label %u, %zu type(s))\n",id,lib,handle,
           service ? service : "",label,count);
    if (params && count) printf("  first type '%.64s'\n",params);
    return id;
}
static ABI int32_t np_push_callback(int32_t user, int32_t filter, void *callback, void *arg) {
    const int32_t id=new_id();
    printf("Runtime: NpWebApi push callback %d (user ctx %d, filter %d, function %p, arg %p)\n",id,user,filter,callback,arg);
    return id;
}

/* HTTP */
static ABI int32_t http_init_abi(void) { return runtime_http_init(); }
static ABI int32_t http_term_abi(void) { return runtime_http_term(); }
static ABI int32_t http_template(int32_t http_ctx,const char *name,int32_t version,int32_t auto_redirect) { return runtime_http_create_template(http_ctx,name,version,auto_redirect); }
static ABI int32_t http_del_template(int32_t tmpl) { return runtime_http_delete_template(tmpl); }
static ABI int32_t http_conn(int32_t tmpl,const char *url,int32_t keep_alive) { return runtime_http_create_connection(tmpl,url,keep_alive); }
static ABI int32_t http_del_conn(int32_t conn) { return runtime_http_delete_connection(conn); }
static ABI int32_t http_req(int32_t conn,int32_t method,const char *path,uint64_t len) { return runtime_http_create_request(conn,method,path,len); }
static ABI int32_t http_req2(int32_t conn,const char *method,const char *path,uint64_t len) { return runtime_http_create_request2(conn,method,path,len); }
static ABI int32_t http_del_req(int32_t req) { return runtime_http_delete_request(req); }
static ABI int32_t http_set_req_len(int32_t req,uint64_t len) { return runtime_http_set_request_content_length(req,len); }
static ABI int32_t http_add_header(int32_t req,const char *name,const char *value,int32_t mode) { return runtime_http_add_request_header(req,name,value,mode); }
static ABI int32_t http_send(int32_t req,const void *data,size_t size) { return runtime_http_send_request(req,data,size); }
static ABI int32_t http_abort(int32_t req) { return runtime_http_abort_request(req); }
static ABI int32_t http_status_code(int32_t req,int32_t *code) { return runtime_http_get_status_code(req,code); }
static ABI int32_t http_resp_len(int32_t req,int32_t *result,uint64_t *len) { return runtime_http_get_response_content_length(req,result,len); }
static ABI int32_t http_read_data(int32_t req,void *data,uint64_t size) { return runtime_http_read_data(req,data,size); }
static ABI int32_t http_all_headers(int32_t req,char **headers,uint64_t *length) { return runtime_http_get_all_response_headers(req,headers,length); }
/* PS4-faithful blob parser: (blob, blob_len, field, &value, &value_len). */
static ABI int32_t http_parse_header(const char *blob,uint64_t blob_len,const char *name,const char **value,uint64_t *length) { return runtime_http_parse_response_header(blob,blob_len,name,value,length); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) { return runtime_http_create_epoll(ctx,handle); }
static ABI int32_t http_destroy_epoll(void *handle) { return runtime_http_destroy_epoll(handle); }
static ABI int32_t http_set_epoll(void *handle,int32_t req,void *user_data) { return runtime_http_set_epoll(handle,req,user_data); }
static ABI int32_t http_unset_epoll(void *handle,int32_t req) { return runtime_http_unset_epoll(handle,req); }
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) { return runtime_http_wait_request(handle,events,max,timeout); }

/* NP */
static ABI int32_t np_state(int32_t user,int32_t *state) { return runtime_np_get_state(user,state); }
static ABI int32_t np_online_id(int32_t user,void *id) { return runtime_np_get_online_id(user,id); }
static ABI int32_t np_np_id(int32_t user,void *id) { return runtime_np_get_np_id(user,id); }
static ABI int32_t np_account_id(int32_t user,uint64_t *id) { return runtime_np_get_account_id(user,id); }
static ABI int32_t np_user_id_by_acc_id(uint64_t acc, int32_t *user_id) { return runtime_np_get_user_id_by_account_id(acc, user_id); }
static ABI int32_t np_check_plus(int32_t req, const void *param, void *result) { return runtime_np_check_plus(req, param, result); }
static ABI int32_t np_check_avail(void) { return runtime_np_check_availability(); }
static ABI int32_t np_parental_control(int32_t req, void *online_or_user, int8_t *age, void *info) { return runtime_np_parental_control(req, online_or_user, age, info); }
static ABI int32_t np_auth_id_token(int32_t req,const void *param,void *token) { return runtime_np_auth_get_id_token(req,param,token); }
static ABI int32_t np_auth_code(int32_t req,const void *param,void *code) { return runtime_np_auth_get_auth_code(req,param,code,NULL); }
static ABI int32_t np_reg_state_cb(void *cb,void *arg) { return runtime_np_register_state_cb(cb,arg); }
static ABI int32_t np_reg_state_cb_toolkit(void *cb,void *arg) { return runtime_np_register_state_cb_toolkit(cb,arg); }
static ABI int32_t np_unreg_state_cb(void *cb,void *arg) { return runtime_np_unregister_state_cb(cb,arg); }
static ABI int32_t np_unreg_state_cb_toolkit(void *cb,void *arg) { return runtime_np_unregister_state_cb_toolkit(cb,arg); }
static ABI int32_t np_check_cb(void) { return runtime_np_check_callback(); }
static ABI int32_t np_presence_status(const void *id, int32_t *status) { (void)id; if (status) *status = runtime_net_is_online() ? 1 : 0; return 0; }

static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { return runtime_np_poll(request, result); }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* WebApi */
typedef struct {
    int32_t httpStatus;
    char *pErrorObject;
    uint64_t errorObjectSize;
    uint64_t responseDataSize;
} NpWebApiResponseInfo;

typedef struct {
    int64_t id;
    int in_use;
    int32_t method;
    char group[64];
    char path[256];
    char *response;
    size_t response_len;
    size_t read_pos;
} WebApiReq;

#define MAX_WEBAPI_REQS 32
static WebApiReq s_webapi_reqs[MAX_WEBAPI_REQS];
static pthread_mutex_t s_webapi_lock = PTHREAD_MUTEX_INITIALIZER;

static void get_sessions_dir(char *out, size_t out_len) {
    const char *sess_env = getenv("BB_SESSIONS_DIR");
    if (sess_env && *sess_env) {
        snprintf(out, out_len, "%s", sess_env);
    } else {
        snprintf(out, out_len, "sessions");
    }
#ifdef _WIN32
    _mkdir(out);
#else
    mkdir(out, 0777);
#endif
}

/* ---- Session members (RyuNET) ----
 * POST /v1/sessions/<id>/members joins that session as the local user. The
 * game then reads the session back and requires the local user to be listed
 * as a member, otherwise it reports "not the active user" and retries the
 * join forever (the repeated AVISO dialog). So every join is recorded.
 * Members live next to the session files, one `onlineId|accountId` per line
 * in sessions/members-<sid>.txt, so two local instances share them. Remote
 * players need server-side membership; only users seen joining on this
 * machine are ever recorded here. At most 8 members per session. */
static pthread_mutex_t s_members_lock = PTHREAD_MUTEX_INITIALIZER;
/* The WebAPI invitation session can remain after leaving a private room. It
 * identifies the room, while recent game traffic with a peer distinguishes a
 * multiplayer race from Arcade/Adventure. */
static int s_private_room;
extern int runtime_net_private_lobby_active(void); /* runtime_net.c */
int runtime_private_room_active(void) {
    return __atomic_load_n(&s_private_room, __ATOMIC_ACQUIRE) &&
           runtime_net_private_lobby_active();
}
static void private_room_active(int active) {
    __atomic_store_n(&s_private_room, active, __ATOMIC_RELEASE);
}

static int session_members_path(const char *sid, char *out, size_t out_size) {
    if (!sid || !*sid) return 0;
    size_t n = strlen(sid);
    if (n == 0 || n > 90) return 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = sid[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    snprintf(out, out_size, "%s/members-%s.txt", dir, sid);
    return 1;
}

/* Records the local user as a member of sid. Returns total members. */
static int record_session_member(const char *sid) {
    const char *me = runtime_np_get_online_name();
    if (!me || !*me) return 0;
    char path[512];
    if (!session_members_path(sid, path, sizeof(path))) return 0;
    pthread_mutex_lock(&s_members_lock);
    char seen[8][64];
    int count = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[160];
        while (count < 8 && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *bar = strchr(line, '|');
            if (!line[0] || !bar) continue;
            *bar = 0;
            int dup = 0;
            for (int i = 0; i < count; ++i) dup = dup || strcmp(seen[i], line) == 0;
            if (!dup) snprintf(seen[count++], sizeof(seen[0]), "%s", line);
        }
        fclose(f);
    }
    int present = 0;
    for (int i = 0; i < count; ++i) present = present || strcmp(seen[i], me) == 0;
    if (!present && count < 8) {
        f = fopen(path, "a");
        if (f) {
            fprintf(f, "%s|%llu\n", me, (unsigned long long)runtime_np_calculate_account_id(me));
            fclose(f);
            snprintf(seen[count++], sizeof(seen[0]), "%s", me);
        }
    }
    pthread_mutex_unlock(&s_members_lock);
    runtime_net_trace("webapi: %s joined session %s (%d member(s))", me, sid, count);
    return count;
}

/* Fills names/accs with the recorded members of sid (up to max). Returns count. */
static int read_session_members(const char *sid, char names[][64], uint64_t *accs, int max) {
    char path[512];
    int count = 0;
    if (max <= 0 || !session_members_path(sid, path, sizeof(path))) return 0;
    pthread_mutex_lock(&s_members_lock);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[160];
        while (count < max && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *bar = strchr(line, '|');
            if (!line[0] || !bar) continue;
            *bar = 0;
            snprintf(names[count], 64, "%s", line);
            accs[count] = strtoull(bar + 1, NULL, 10);
            if (accs[count] == 0) accs[count] = runtime_np_calculate_account_id(names[count]);
            count++;
        }
        fclose(f);
    }
    pthread_mutex_unlock(&s_members_lock);
    return count;
}

/* Members recorded for sid excluding host_name (the host is counted separately). */
static int session_join_count(const char *sid, const char *host_name) {
    char names[8][64];
    uint64_t accs[8];
    int n = read_session_members(sid, names, accs, 8);
    int joins = 0;
    for (int i = 0; i < n; ++i) {
        if (!host_name || strcmp(names[i], host_name) != 0) joins++;
    }
    return joins;
}

static void write_active_session(const char *session_id) {
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    const char *my_name = runtime_np_get_online_name();
    if (!my_name || !*my_name) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.session", dir, my_name);
    FILE *f = fopen(path, "w");
    if (f) {
        uint64_t acc_id = runtime_np_calculate_account_id(my_name);
        fprintf(f, "sessionId=%s\naccountId=%llu\nonlineId=%s\ntime=%llu\n",
                session_id, (unsigned long long)acc_id, my_name, (unsigned long long)time(NULL));
        fclose(f);
        runtime_net_trace("write_active_session: wrote session '%s' to '%s'", session_id, path);
    }
}

static void remove_active_session(void) {
    private_room_active(0);
    runtime_room_mode_left(); /* stop following the host's room mode */
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    const char *my_name = runtime_np_get_online_name();
    if (!my_name || !*my_name) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.session", dir, my_name);
    remove(path);
    /* Joins recorded for our own session end with it (no stale members). */
    {
        char sid[128], mpath[512];
        snprintf(sid, sizeof(sid), "cusa13795-session-%s", my_name);
        if (session_members_path(sid, mpath, sizeof(mpath))) remove(mpath);
    }
    runtime_net_trace("remove_active_session: removed '%s'", path);
}

/* The running game's title id (param.sfo TITLE_ID): friends are shown in this game (the European
 * edition is CUSA14876, the Americas' CUSA13795). */
static char np_title[16] = "CUSA13795";
void runtime_np_set_title(const char *title_id) {
    if (title_id && *title_id) snprintf(np_title, sizeof(np_title), "%s", title_id);
}

static char *generate_friends_json(void) {
    const char *csv = runtime_np_get_friends_csv();
    char buf[16384];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"friendList\":[");

    int count = 0;
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 1024 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token) {
                uint64_t acc_id = runtime_np_calculate_account_id(token);
                if (count > 0) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                }
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{"
                    "\"avatarUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
                    "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
                    "\"region\":\"US\","
                    "\"isOfficiallyVerified\":false,"
                    "\"onlineId\":\"%s\","
                    "\"accountId\":\"%llu\","
                    "\"user\":{\"accountId\":\"%llu\",\"onlineId\":\"%s\"},"
                    "\"personalDetail\":{"
                        "\"displayName\":\"%s\","
                        "\"firstName\":\"%s\","
                        "\"middleName\":\"\","
                        "\"lastName\":\"\","
                        "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\""
                    "},"
                    "\"presence\":{"
                        "\"onlineStatus\":\"online\","
                        "\"platform\":\"PS4\","
                        "\"incontextInfoList\":[{"
                            "\"platform\":\"PS4\","
                            "\"onlineStatus\":\"online\","
                            "\"gameStatus\":\"In Game\","
                            "\"gameData\":\"\","
                            "\"gameTitleInfo\":{"
                                "\"npTitleId\":\"%s_00\","
                                "\"titleName\":\"Crash Team Racing Nitro-Fueled\""
                            "}"
                        "}]"
                    "}"
                    "}",
                    token, (unsigned long long)acc_id, (unsigned long long)acc_id, token, token, token, np_title);
                count++;
            }
            token = strtok(NULL, ",");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"totalResults\":%d,\"size\":%d,\"offset\":0,\"limit\":500,\"start\":0}", count, count);
    runtime_net_trace("generate_friends_json: returning %d friend(s)", count);
    return strdup(buf);
}

static char *generate_profiles_json(const char *path) {
    (void)path;
    const char *csv = runtime_np_get_friends_csv();
    const char *my_name = runtime_np_get_online_name();

    char buf[16384];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"profiles\":[");

    int count = 0;

    /* Include local player */
    if (my_name && *my_name) {
        uint64_t my_acc = runtime_np_calculate_account_id(my_name);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{"
            "\"accountId\":\"%llu\","
            "\"onlineId\":\"%s\","
            "\"region\":\"US\","
            "\"isOfficiallyVerified\":false,"
            "\"user\":{\"accountId\":\"%llu\",\"onlineId\":\"%s\"},"
            "\"relation\":\"me\","
            "\"aboutMe\":\"Crash Team Racing Nitro-Fueled\","
            "\"languagesUsed\":[\"en\"],"
            "\"avatarUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
            "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
            "\"personalDetail\":{"
                "\"displayName\":\"%s\","
                "\"firstName\":\"%s\","
                "\"middleName\":\"\","
                "\"lastName\":\"\","
                "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\""
            "}"
            "}",
            (unsigned long long)my_acc, my_name, (unsigned long long)my_acc, my_name, my_name, my_name);
        count++;
    }

    /* Include friends */
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 1024 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token && (!my_name || strcmp(token, my_name) != 0)) {
                uint64_t acc_id = runtime_np_calculate_account_id(token);
                if (count > 0) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                }
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{"
                    "\"accountId\":\"%llu\","
                    "\"onlineId\":\"%s\","
                    "\"region\":\"US\","
                    "\"isOfficiallyVerified\":false,"
                    "\"user\":{\"accountId\":\"%llu\",\"onlineId\":\"%s\"},"
                    "\"relation\":\"friend\","
                    "\"aboutMe\":\"Crash Team Racing Nitro-Fueled\","
                    "\"languagesUsed\":[\"en\"],"
                    "\"avatarUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
                    "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
                    "\"personalDetail\":{"
                        "\"displayName\":\"%s\","
                        "\"firstName\":\"%s\","
                        "\"middleName\":\"\","
                        "\"lastName\":\"\","
                        "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\""
                    "}"
                    "}",
                    (unsigned long long)acc_id, token, (unsigned long long)acc_id, token, token, token);
                count++;
            }
            token = strtok(NULL, ",");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"size\":%d,\"totalResults\":%d}", count, count);
    runtime_net_trace("generate_profiles_json: returning %d profile(s)", count);
    return strdup(buf);
}

static char *generate_friend_online_id_lists_json(void) {
    const char *csv = runtime_np_get_friends_csv();
    const char *my_name = runtime_np_get_online_name();
    uint64_t my_acc = runtime_np_calculate_account_id(my_name);

    char buf[8192];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"friendOnlineIdLists\":[{\"user\":{\"accountId\":\"%llu\",\"onlineId\":\"%s\"},\"friendOnlineIdList\":[",
        (unsigned long long)my_acc, (my_name && *my_name) ? my_name : "Player");

    int count = 0;
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 128 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token) {
                if (count > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "\"%s\"", token);
                count++;
            }
            token = strtok(NULL, ",");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}]}");
    return strdup(buf);
}

static char *invitation_json(const char *host);

static char *generate_invitations_json(void) {
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    const char *csv = runtime_np_get_friends_csv();
    const char *my_name = runtime_np_get_online_name();

    char buf[8192];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"invitations\":[");

    int count = 0;
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 1024 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token && (!my_name || strcmp(token, my_name) != 0)) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s.session", dir, token);
                FILE *f = fopen(path, "r");
                if (f) {
                    char sid[128] = {0};
                    uint64_t acc_id = 0;
                    char line[256];
                    while (fgets(line, sizeof(line), f)) {
                        if (strncmp(line, "sessionId=", 10) == 0) {
                            char *nl = line + 10;
                            while (*nl && *nl != '\r' && *nl != '\n') nl++;
                            *nl = '\0';
                            strncpy(sid, line + 10, sizeof(sid) - 1);
                        } else if (strncmp(line, "accountId=", 10) == 0) {
                            acc_id = strtoull(line + 10, NULL, 10);
                        }
                    }
                    fclose(f);

                    if (sid[0] == '\0') {
                        snprintf(sid, sizeof(sid), "cusa13795-session-%s", token);
                    }
                    if (acc_id == 0) {
                        acc_id = runtime_np_calculate_account_id(token);
                    }

                    if (count > 0) {
                        pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                    }
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{"
                        "\"invitationId\":\"inv-%s\","
                        "\"sessionId\":\"%s\","
                        "\"receivedDate\":\"2026-10-14T00:00:00.000Z\","
                        "\"updateDate\":\"2026-10-14T00:00:00.000Z\","
                        "\"expired\":false,"
                        "\"usedFlag\":false,"
                        "\"fromUser\":{"
                            "\"onlineId\":\"%s\","
                            "\"accountId\":\"%llu\""
                        "},"
                        "\"session\":{"
                            "\"sessionId\":\"%s\","
                            "\"sessionName\":\"CTR Private Match\","
                            "\"sessionLockFlag\":false"
                        "}"
                        "}",
                        token, sid, token, (unsigned long long)acc_id, sid);
                    count++;
                }
            }
            token = strtok(NULL, ",");
        }
    }

    /* The invitation accepted in Discord: its host's session file lives on another PC. */
    pthread_mutex_lock(&s_invite_lock);
    char pending_host[32] = "";
    if (s_invite.active) snprintf(pending_host, sizeof(pending_host), "%s", s_invite.host);
    pthread_mutex_unlock(&s_invite_lock);
    char pending_marker[48];
    snprintf(pending_marker, sizeof(pending_marker), "\"inv-%s\"", pending_host);
    if (pending_host[0] && !strstr(buf, pending_marker) && pos + 1024 < sizeof(buf)) {
        char *one = invitation_json(pending_host);
        if (one) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s", count > 0 ? "," : "", one);
            free(one);
            count++;
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"size\":%d,\"totalResults\":%d}", count, count);
    runtime_net_trace("generate_invitations_json: found %d invitation(s)", count);
    return strdup(buf);
}


static char *generate_friend_sessions_json(void) {
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    const char *csv = runtime_np_get_friends_csv();
    const char *my_name = runtime_np_get_online_name();

    char buf[8192];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"sessions\":[");

    int count = 0;
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 1024 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token && (!my_name || strcmp(token, my_name) != 0)) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s.session", dir, token);
                FILE *f = fopen(path, "r");
                if (f) {
                    char sid[128] = {0};
                    uint64_t acc_id = 0;
                    char line[256];
                    while (fgets(line, sizeof(line), f)) {
                        if (strncmp(line, "sessionId=", 10) == 0) {
                            char *nl = line + 10;
                            while (*nl && *nl != '\r' && *nl != '\n') nl++;
                            *nl = '\0';
                            strncpy(sid, line + 10, sizeof(sid) - 1);
                        } else if (strncmp(line, "accountId=", 10) == 0) {
                            acc_id = strtoull(line + 10, NULL, 10);
                        }
                    }
                    fclose(f);

                    if (sid[0] == '\0') {
                        snprintf(sid, sizeof(sid), "cusa13795-session-%s", token);
                    }
                    if (acc_id == 0) {
                        acc_id = runtime_np_calculate_account_id(token);
                    }

                    if (count > 0) {
                        pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                    }
                    int total = 1 + session_join_count(sid, token);
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{"
                        "\"sessionId\":\"%s\","
                        "\"sessionName\":\"CTR Private Match\","
                        "\"sessionCreateTimestamp\":\"2026-10-14T00:00:00.000Z\","
                        "\"availablePlatforms\":[\"PS4\"],"
                        "\"memberCount\":%d,"
                        "\"sessionLockFlag\":false,"
                        "\"friends\":[{"
                            "\"onlineId\":\"%s\","
                            "\"accountId\":\"%llu\","
                            "\"platform\":\"PS4\""
                        "}]"
                        "}",
                        sid, total, token, (unsigned long long)acc_id);
                    count++;
                }
            }
            token = strtok(NULL, ",");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"start\":0,\"size\":%d,\"totalResults\":%d}", count, count);
    runtime_net_trace("generate_friend_sessions_json: found %d active session(s)", count);
    return strdup(buf);
}

static char *generate_multiuser_sessions_json(void) {
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    const char *csv = runtime_np_get_friends_csv();
    const char *my_name = runtime_np_get_online_name();

    char buf[8192];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"users\":[");

    int count = 0;
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 1024 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token && (!my_name || strcmp(token, my_name) != 0)) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s.session", dir, token);
                FILE *f = fopen(path, "r");
                if (f) {
                    char sid[128] = {0};
                    uint64_t acc_id = 0;
                    char line[256];
                    while (fgets(line, sizeof(line), f)) {
                        if (strncmp(line, "sessionId=", 10) == 0) {
                            char *nl = line + 10;
                            while (*nl && *nl != '\r' && *nl != '\n') nl++;
                            *nl = '\0';
                            strncpy(sid, line + 10, sizeof(sid) - 1);
                        } else if (strncmp(line, "accountId=", 10) == 0) {
                            acc_id = strtoull(line + 10, NULL, 10);
                        }
                    }
                    fclose(f);

                    if (sid[0] == '\0') {
                        snprintf(sid, sizeof(sid), "cusa13795-session-%s", token);
                    }
                    if (acc_id == 0) {
                        acc_id = runtime_np_calculate_account_id(token);
                    }

                    if (count > 0) {
                        pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                    }
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{"
                        "\"accountId\":\"%llu\","
                        "\"sessions\":[{"
                            "\"sessionId\":\"%s\","
                            "\"sessionName\":\"CTR Private Match\","
                            "\"availablePlatforms\":[\"PS4\"],"
                            "\"memberCount\":1,"
                            "\"sessionLockFlag\":false,"
                            "\"priority\":0,"
                            "\"index\":0"
                        "}]"
                        "}",
                        (unsigned long long)acc_id, sid);
                    count++;
                }
            }
            token = strtok(NULL, ",");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return strdup(buf);
}

static char *generate_session_info_json(const char *session_id) {
    char host_name[64] = "Player";
    uint64_t acc_id = 0;

    if (session_id && strncmp(session_id, "cusa13795-session-", 18) == 0) {
        strncpy(host_name, session_id + 18, sizeof(host_name) - 1);
        host_name[sizeof(host_name) - 1] = '\0';
    } else {
        const char *my_name = runtime_np_get_online_name();
        if (my_name && *my_name) {
            strncpy(host_name, my_name, sizeof(host_name) - 1);
            host_name[sizeof(host_name) - 1] = '\0';
        }
    }

    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.session", dir, host_name);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "accountId=", 10) == 0) {
                acc_id = strtoull(line + 10, NULL, 10);
            }
        }
        fclose(f);
    }
    if (acc_id == 0) {
        acc_id = runtime_np_calculate_account_id(host_name);
    }

    /* members: the host plus everyone recorded by POST .../members. The game
     * requires the local user here, else "not the active user". */
    char mnames[8][64];
    uint64_t maccs[8];
    int nmem = read_session_members(session_id ? session_id : "", mnames, maccs, 8);
    char buf[2048];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{"
        "\"sessionId\":\"%s\","
        "\"sessionName\":\"CTR Private Match\","
        "\"sessionPrivacy\":\"public\","
        "\"sessionType\":\"owner-bind\","
        "\"sessionMaxUser\":8,"
        "\"sessionStatus\":\"in_lobby\","
        "\"sessionLockFlag\":false,"
        "\"availablePlatforms\":[\"PS4\"],"
        /* sessionCreator: an invitation is joined through the session's creator (eboot
         * 0xac4576 reads its accountId from the toolkit's Session; 0 there is "failed to
         * retrieve friend information", 0xac4584), whose Demonware lobby is then looked up. */
        "\"sessionCreator\":{"
            "\"accountId\":\"%llu\","
            "\"onlineId\":\"%s\","
            "\"platform\":\"PS4\""
        "},"
        "\"members\":[{"
            "\"accountId\":\"%llu\","
            "\"onlineId\":\"%s\","
            "\"platform\":\"PS4\""
        "}",
        session_id ? session_id : "cusa13795-session",
        (unsigned long long)acc_id, host_name, (unsigned long long)acc_id, host_name);
    for (int i = 0; i < nmem && pos + 160 < sizeof(buf); ++i) {
        if (strcmp(mnames[i], host_name) == 0) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",{"
            "\"accountId\":\"%llu\","
            "\"onlineId\":\"%s\","
            "\"platform\":\"PS4\""
            "}",
            (unsigned long long)maccs[i], mnames[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return strdup(buf);
}

static ABI int32_t np_webapi_create_ctx(int32_t lib_ctx, void *online_id) { (void)lib_ctx; (void)online_id; return new_id(); }
static ABI int32_t np_webapi_create_ctx_a(int32_t lib_ctx, int32_t user_id) { (void)lib_ctx; (void)user_id; return new_id(); }
static ABI int32_t np_webapi_create_req(int32_t user_ctx, const char *group, const char *path,
                                        int32_t method, const void *param, int64_t *req_id) {
    (void)user_ctx; (void)param;
    int id = new_id();
    pthread_mutex_lock(&s_webapi_lock);
    for (int i = 0; i < MAX_WEBAPI_REQS; i++) {
        if (!s_webapi_reqs[i].in_use) {
            s_webapi_reqs[i].in_use = 1;
            s_webapi_reqs[i].id = id;
            s_webapi_reqs[i].method = method;
            strncpy(s_webapi_reqs[i].group, group ? group : "", sizeof(s_webapi_reqs[i].group) - 1);
            s_webapi_reqs[i].group[sizeof(s_webapi_reqs[i].group) - 1] = '\0';
            strncpy(s_webapi_reqs[i].path, path ? path : "", sizeof(s_webapi_reqs[i].path) - 1);
            s_webapi_reqs[i].path[sizeof(s_webapi_reqs[i].path) - 1] = '\0';
            s_webapi_reqs[i].response = NULL;
            s_webapi_reqs[i].response_len = 0;
            s_webapi_reqs[i].read_pos = 0;
            break;
        }
    }
    pthread_mutex_unlock(&s_webapi_lock);
    if (req_id) *req_id = (int64_t)id;
    runtime_net_trace("sceNpWebApiCreateRequest: group='%s', path='%s', method=%d, req_id=%d",
                      group ? group : "", path ? path : "", method, id);
    return 0;
}

/* One invitation from host to their session "cusa13795-session-<host>", the shape the list
 * (generate_invitations_json) uses. Sessions are named after their host, so another PC's
 * invitation needs no shared file: the Discord invite named the host. */
static char *invitation_json(const char *host) {
    char sid[96], buf[1024];
    snprintf(sid, sizeof(sid), "cusa13795-session-%s", host);
    snprintf(buf, sizeof(buf),
        "{\"invitationId\":\"inv-%s\",\"sessionId\":\"%s\","
        "\"receivedDate\":\"2026-10-14T00:00:00.000Z\",\"updateDate\":\"2026-10-14T00:00:00.000Z\","
        "\"expired\":false,\"usedFlag\":false,"
        "\"fromUser\":{\"onlineId\":\"%s\",\"accountId\":\"%llu\"},"
        "\"session\":{\"sessionId\":\"%s\",\"sessionName\":\"CTR Private Match\",\"sessionLockFlag\":false}}",
        host, sid, host, (unsigned long long)runtime_np_calculate_account_id(host), sid);
    return strdup(buf);
}

/* The <id> in ".../<marker><id>[/?...]", or "" (at most out_size-1 characters). */
static void path_segment_after(const char *path, const char *marker, char *out, size_t out_size) {
    out[0] = '\0';
    const char *p = path ? strstr(path, marker) : NULL;
    if (!p) return;
    p += strlen(marker);
    size_t n = 0;
    while (p[n] && p[n] != '?' && p[n] != '/' && n < out_size - 1) n++;
    memcpy(out, p, n);
    out[n] = '\0';
}

static char *get_webapi_response_for_req(const WebApiReq *req) {
    runtime_net_trace("get_webapi_response_for_req: path='%s', method=%d", req->path, req->method);

    /* GET /v1/users/me/invitations/inv-<host>: one invitation, as after a SESSION_INVITATION event. */
    if (req->method == 0 && strstr(req->path, "/invitations/inv-")) {
        char host[64];
        path_segment_after(req->path, "/invitations/inv-", host, sizeof(host));
        if (host[0]) return invitation_json(host);
    }
    /* PUT on an invitation (e.g. marking it used): nothing to keep. */
    if (req->method == 2 && strstr(req->path, "/invitations/")) {
        return strdup("{}");
    }
    /* POST /v1/sessions/<id>/members -> join that session. Checked before "create", which also
     * POSTs under /v1/sessions and used to answer a join with the joiner's own session. */
    if (req->method == 1 && strstr(req->path, "/v1/sessions/") && strstr(req->path, "/members")) {
        char sid[96];
        path_segment_after(req->path, "/v1/sessions/", sid, sizeof(sid));
        if (sid[0]) {
            record_session_member(sid);
            private_room_active(1);
            runtime_room_mode_joined(sid); /* the host's mode (Speed) from RyuNET */
        }
        invite_session_joined(req->path);
        char resp[160];
        snprintf(resp, sizeof(resp), "{\"sessionId\":\"%s\"}", sid[0] ? sid : "cusa13795-session");
        runtime_net_trace("webapi: joined session -> %s", resp);
        return strdup(resp);
    }

    if (strstr(req->path, "friendList")) {
        return generate_friends_json();
    }
    if (strstr(req->path, "friendOnlineIdLists")) {
        return generate_friend_online_id_lists_json();
    }
    if (strstr(req->path, "profile") || strstr(req->path, "profiles")) {
        return generate_profiles_json(req->path);
    }
    if (strstr(req->path, "friends/sessions")) {
        return generate_friend_sessions_json();
    }
    if (strstr(req->path, "@users/sessions")) {
        return generate_multiuser_sessions_json();
    }
    if (strstr(req->path, "entitlements")) {
        return strdup("{\"entitlements\":[],\"totalResults\":0,\"start\":0,\"size\":0}");
    }
    if (strstr(req->path, "invitations") && req->method == 1) {
        return strdup("{\"invitationId\":\"inv-sent\"}");
    }
    if (strstr(req->path, "invitations")) {
        return generate_invitations_json();
    }
    /* POST /v1/sessions -> create session (method 1 is POST in OrbisNpWebApiHttpMethod) */
    if (req->method == 1 && strstr(req->path, "/v1/sessions")) {
        const char *my_name = runtime_np_get_online_name();
        char sid[128];
        snprintf(sid, sizeof(sid), "cusa13795-session-%s", (my_name && *my_name) ? my_name : "host");
        write_active_session(sid);
        private_room_active(1);
        runtime_room_mode_hosting(); /* the room's mode to RyuNET for the players who join */
        char resp[256];
        snprintf(resp, sizeof(resp), "{\"sessionId\":\"%s\"}", sid);
        runtime_net_trace("webapi: created session -> %s", resp);
        return strdup(resp);
    }
    /* DELETE /v1/sessions -> delete session (method 3 is DELETE) */
    if (req->method == 3 && strstr(req->path, "/v1/sessions")) {
        remove_active_session();
        return strdup("{}");
    }
    /* POST /v1/sessions/<id>/members -> join session */
    if (strstr(req->path, "/v1/sessions/") && strstr(req->path, "/members")) {
        char sid[96];
        path_segment_after(req->path, "/v1/sessions/", sid, sizeof(sid));
        if (sid[0]) {
            record_session_member(sid);
            private_room_active(1);
            runtime_room_mode_joined(sid);
        }
        invite_session_joined(req->path);
        return strdup("{\"sessionId\":\"cusa13795-session\"}");
    }
    /* GET /v1/sessions/<id> */
    if (strstr(req->path, "/v1/sessions/")) {
        const char *p = strstr(req->path, "/v1/sessions/") + 13;
        char sid[128] = {0};
        size_t si = 0;
        while (p[si] && p[si] != '?' && p[si] != '/' && si < sizeof(sid) - 1) {
            sid[si] = p[si];
            si++;
        }
        sid[si] = '\0';
        invite_session_taken(sid);
        return generate_session_info_json(sid);
    }
    /* generic /v1/sessions query */
    if (strstr(req->path, "/v1/sessions")) {
        return generate_friend_sessions_json();
    }
    if (strstr(req->path, "friends")) {
        return generate_friends_json();
    }
    return strdup("{}");
}

static ABI int32_t np_webapi_send_req(int64_t req_id, const void *data, uint64_t size) {
    (void)data; (void)size;
    pthread_mutex_lock(&s_webapi_lock);
    for (int i = 0; i < MAX_WEBAPI_REQS; i++) {
        if (s_webapi_reqs[i].in_use && s_webapi_reqs[i].id == req_id) {
            if (!s_webapi_reqs[i].response) {
                s_webapi_reqs[i].response = get_webapi_response_for_req(&s_webapi_reqs[i]);
                s_webapi_reqs[i].response_len = strlen(s_webapi_reqs[i].response);
                s_webapi_reqs[i].read_pos = 0;
            }
            runtime_net_trace("sceNpWebApiSendRequest: req_id=%lld -> prepared %zu bytes response",
                              (long long)req_id, s_webapi_reqs[i].response_len);
            break;
        }
    }
    pthread_mutex_unlock(&s_webapi_lock);
    return 0;
}

static ABI int32_t np_webapi_send_req2(int64_t req_id, const void *data, uint64_t size, NpWebApiResponseInfo *resp) {
    (void)data; (void)size;
    pthread_mutex_lock(&s_webapi_lock);
    for (int i = 0; i < MAX_WEBAPI_REQS; i++) {
        if (s_webapi_reqs[i].in_use && s_webapi_reqs[i].id == req_id) {
            if (!s_webapi_reqs[i].response) {
                s_webapi_reqs[i].response = get_webapi_response_for_req(&s_webapi_reqs[i]);
                s_webapi_reqs[i].response_len = strlen(s_webapi_reqs[i].response);
                s_webapi_reqs[i].read_pos = 0;
            }
            if (resp) {
                resp->httpStatus = 200;
                resp->responseDataSize = s_webapi_reqs[i].response_len;
            }
            runtime_net_trace("sceNpWebApiSendRequest2: req_id=%lld -> status 200, size=%zu",
                              (long long)req_id, s_webapi_reqs[i].response_len);
            pthread_mutex_unlock(&s_webapi_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&s_webapi_lock);
    if (resp) { resp->httpStatus = 200; resp->responseDataSize = 0; }
    return 0;
}

static ABI int32_t np_webapi_add_header(int64_t req_id, const char *name, const char *val) {
    (void)req_id; (void)name; (void)val;
    return 0;
}

static ABI int32_t np_webapi_read_data(int64_t req_id, void *data, uint64_t size) {
    pthread_mutex_lock(&s_webapi_lock);
    for (int i = 0; i < MAX_WEBAPI_REQS; i++) {
        if (s_webapi_reqs[i].in_use && s_webapi_reqs[i].id == req_id) {
            if (s_webapi_reqs[i].response && data && size > 0) {
                size_t avail = s_webapi_reqs[i].response_len - s_webapi_reqs[i].read_pos;
                size_t to_copy = size < avail ? (size_t)size : avail;
                if (to_copy > 0) {
                    memcpy(data, s_webapi_reqs[i].response + s_webapi_reqs[i].read_pos, to_copy);
                    s_webapi_reqs[i].read_pos += to_copy;
                }
                runtime_net_trace("sceNpWebApiReadData: req_id=%lld, read %zu bytes (pos=%zu/%zu)",
                                  (long long)req_id, to_copy, s_webapi_reqs[i].read_pos, s_webapi_reqs[i].response_len);
                pthread_mutex_unlock(&s_webapi_lock);
                return (int32_t)to_copy;
            }
            pthread_mutex_unlock(&s_webapi_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&s_webapi_lock);
    return 0;
}

static ABI int32_t np_webapi_delete_req(int64_t req_id) {
    pthread_mutex_lock(&s_webapi_lock);
    for (int i = 0; i < MAX_WEBAPI_REQS; i++) {
        if (s_webapi_reqs[i].in_use && s_webapi_reqs[i].id == req_id) {
            if (s_webapi_reqs[i].response) {
                free(s_webapi_reqs[i].response);
                s_webapi_reqs[i].response = NULL;
            }
            s_webapi_reqs[i].in_use = 0;
            break;
        }
    }
    pthread_mutex_unlock(&s_webapi_lock);
    return 0;
}

static ABI int32_t np_webapi_get_status(int64_t req_id, int32_t *code) {
    (void)req_id;
    if (code) *code = 200;
    return 0;
}

static ABI int32_t np_webapi_get_header(int64_t req_id, const char *name, char *val, uint64_t size) {
    (void)req_id; (void)name;
    if (val && size > 0) val[0] = '\0';
    return 0;
}

static ABI int32_t np_webapi_get_header_len(int64_t req_id, const char *name, uint64_t *len) {
    (void)req_id; (void)name;
    if (len) *len = 0;
    return 0;
}

static ABI int32_t np_webapi_pool_stats(int32_t lib_ctx, void *stats) {
    (void)lib_ctx;
    if (stats) memset(stats, 0, 32);
    return 0;
}

static ABI int32_t np_webapi_parse_npid(const char *json, void *npid) {
    if (!npid) return NP_INVALID_ARGUMENT;
    memset(npid, 0, 32);
    if (!json) return 0;
    const char *p = strstr(json, "\"onlineId\":");
    if (!p) p = strstr(json, "\"onlineId\" :");
    if (!p) p = strstr(json, "\"npId\":");
    if (!p) p = strstr(json, "\"npId\" :");
    if (p) {
        const char *quote1 = strchr(p, ':');
        if (quote1) {
            quote1 = strchr(quote1, '"');
            if (quote1) {
                quote1++;
                const char *quote2 = strchr(quote1, '"');
                if (quote2) {
                    size_t len = (size_t)(quote2 - quote1);
                    if (len > 15) len = 15;
                    memcpy(npid, quote1, len);
                    ((char *)npid)[len] = '\0';
                }
            }
        }
    }
    runtime_net_trace("sceNpWebApiUtilityParseNpId -> '%s'", (char *)npid);
    return 0;
}

static ABI int32_t http_uri_escape(char *out, uint64_t *require, uint64_t prepare, const char *in) {
    if (!in) return -1;
    static const char hex_chars[] = "0123456789ABCDEF";
    uint64_t needed = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            needed++;
        } else {
            needed += 3;
        }
    }
    needed++;
    if (require) *require = needed;
    if (!out) return 0;
    if (prepare < needed) return -1;
    char *dst = out;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            *dst++ = *p;
        } else {
            *dst++ = '%';
            *dst++ = hex_chars[(*p >> 4) & 0x0F];
            *dst++ = hex_chars[*p & 0x0F];
        }
    }
    *dst = '\0';
    return 0;
}

/* (ctxId, stats*): sceSslGetMemoryPoolStats, sceHttpGetMemoryPoolStats, sceNetGetMemoryPoolStats,
 * sceNpInGameMessageGetMemoryPoolStatistics. Stats = 3 size_t + int32 (32 bytes). */
static ABI int32_t ctx_pool_stats(int32_t ctx, void *stats) {
    (void)ctx;
    if (stats) memset(stats, 0, 32);
    return 0;
}


/* The game's Invite button opens the PS4 invitation dialog to pick friends (its parameter names
 * only the game's own session, "cusa13795-session-<nickname>", in user-select mode). With RyuNET
 * friends, every friend online is invited at once and a notice says so (runtime_friends.c); the
 * dialog then closes. */
static struct { int initialized, status; } invitation;
static ABI int32_t invitation_init(void) {
    if (invitation.initialized) return (int32_t)0x80B80004;
    invitation.initialized=1; invitation.status=1; return 0;
}
static ABI int32_t invitation_open(const void *p) {
    (void)p;
    if (!invitation.initialized) return DIALOG_NOT_INITIALIZED;
    if (runtime_friends_invite_online()) printf("Runtime: InvitationDialog: friends online invited\n");
    invitation.status=3; return 0;
}
static ABI int32_t invitation_status(void) { return invitation.status; }
static ABI int32_t invitation_term(void) {
    if (!invitation.initialized) return DIALOG_NOT_INITIALIZED;
    invitation.initialized=0; invitation.status=0; return 0;
}
static ABI int32_t invitation_result(void *result) { if (result) memset(result,0,16); return 0; }
static ABI int32_t http_ok(void) { return 0; }

/* UserService event callbacks (ForNpToolkit): registered and notify login of local user 1. */
typedef struct SceUserServiceEvent {
    int32_t eventType; /* 0 = SCE_USER_SERVICE_EVENT_TYPE_LOGIN */
    int32_t userId;    /* 1 */
} SceUserServiceEvent;
typedef void (ABI *SceUserServiceEventCallback)(const SceUserServiceEvent *event, void *userData);
static SceUserServiceEventCallback s_user_event_cb = NULL;
static void *s_user_event_arg = NULL;
static ABI int32_t user_event_register(void *cb, void *arg) {
    s_user_event_cb = (SceUserServiceEventCallback)cb;
    s_user_event_arg = arg;
    runtime_net_trace("sceUserServiceRegisterEventCallback: registered cb=%p, arg=%p", cb, arg);
    if (s_user_event_cb) {
        SceUserServiceEvent ev;
        ev.eventType = 0; /* SCE_USER_SERVICE_EVENT_TYPE_LOGIN */
        ev.userId = 1;
        runtime_net_trace("sceUserServiceRegisterEventCallback -> dispatching initial LOGIN for user 1");
        s_user_event_cb(&ev, s_user_event_arg);
    }
    return 0;
}
static ABI int32_t user_event_unregister(void) {
    s_user_event_cb = NULL;
    s_user_event_arg = NULL;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceHttpSetChunkedTransferEnabled",http_ok}, {"sceHttpsSetSslCallback",http_ok},
    {"sceSystemServiceGetStatus",system_status}, {"sceSystemServiceReceiveEvent",system_event},
    {"sceNetInit",net_init}, {"sceNetTerm",net_term},
    {"sceNetErrnoLoc",net_errno_loc}, {"sceNetPoolCreate",net_pool_create},
    {"sceNetPoolDestroy",net_pool_destroy}, {"sceNetEpollCreate",net_epoll_create},
    {"sceNetEpollDestroy",net_epoll_destroy}, {"sceNetResolverCreate",net_resolver_create},
    {"sceNetResolverDestroy",net_resolver_destroy}, {"sceNetHtons",net_htons},
    {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl},
    {"sceNetNtohl",net_ntohl}, {"sceNetInetPton",net_pton},
    {"sceNetInetNtop",net_ntop}, {"sceNetSocket",net_socket},
    {"sceNetConnect",net_connect}, {"sceNetBind",net_bind},
    {"sceNetListen",net_listen}, {"sceNetAccept",net_accept},
    {"sceNetSend",net_send}, {"sceNetSendto",net_sendto},
    {"sceNetRecv",net_recv}, {"sceNetRecvfrom",net_recvfrom},
    {"sceNetSetsockopt",net_setsockopt}, {"sceNetGetsockopt",net_getsockopt},
    {"sceNetGetsockname",net_getsockname}, {"sceNetShutdown",net_shutdown},
    {"sceNetSocketClose",net_close}, {"sceNetSocketAbort",net_abort},
    {"sceNetEpollControl",net_epoll_control}, {"sceNetEpollWait",net_epoll_wait},
    {"sceNetEpollAbort",net_epoll_abort}, {"sceNetResolverStartNtoa",net_resolver_start_ntoa},
    {"sceNetResolverStartAton",net_resolver_start_aton}, {"sceNetCtlGetState",netctl_state},
    {"sceNetCtlGetInfo",netctl_info}, {"sceNetCtlRegisterCallback",netctl_register},
    {"sceNetCtlCheckCallback",netctl_check}, {"sceNetCtlUnregisterCallback",netctl_unregister},
    {"sceNetCtlGetNatInfo",netctl_nat}, {"sceSslInit",lib_init_id},
    {"sceSslTerm",ok_void}, {"sceHttpInit",http_init_abi},
    {"sceHttpTerm",http_term_abi}, {"sceHttpCreateTemplate",http_template},
    {"sceHttpDeleteTemplate",http_del_template}, {"sceHttpCreateConnectionWithURL",http_conn},
    {"sceHttpCreateRequestWithURL",http_req}, {"sceHttpSendRequest",http_send},
    {"sceHttpCreateEpoll",http_epoll}, {"sceHttpSetNonblock",ok_void},
    {"sceHttpSetConnectTimeOut",ok_void}, {"sceHttpsEnableOption",ok_void},
    {"sceHttpsDisableOption",ok_void}, {"sceHttpAddRequestHeader",http_add_header},
    {"sceHttpSetRequestContentLength",http_set_req_len}, {"sceHttpDeleteConnection",http_del_conn},
    {"sceHttpDeleteRequest",http_del_req}, {"sceHttpAbortWaitRequest",http_abort},
    {"sceHttpDestroyEpoll",http_destroy_epoll}, {"sceHttpSetEpoll",http_set_epoll},
    {"sceHttpUnsetEpoll",http_unset_epoll}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_status_code}, {"sceHttpGetResponseContentLength",http_resp_len},
    {"sceHttpReadData",http_read_data}, {"sceNpGetState",np_state},
    {"sceNpGetOnlineId",np_online_id}, {"sceNpGetNpId",np_np_id},
    {"sceNpRegisterStateCallback",np_reg_state_cb}, {"sceNpUnregisterStateCallback",np_unreg_state_cb},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",ok_void}, {"sceNpCheckCallback",np_check_cb},
    {"sceNpSetNpTitleId",ok_void}, {"sceNpNotifyPlusFeature",ok_void},
    {"sceNpSetContentRestriction",ok_void}, {"sceNpCreateAsyncRequest",np_request},
    {"sceNpDeleteRequest",ok_void}, {"sceNpAbortRequest",ok_void},
    {"sceNpPollAsync",np_poll}, {"sceNpCheckNpAvailability",np_check_avail},
    {"sceNpGetParentalControlInfo",np_parental_control}, {"sceNpCheckPlus",np_check_plus},
    {"sceNpGetGamePresenceStatus",np_presence_status}, {"sceNpCmpNpId",np_compare},
    {"sceNpCmpOnlineId",np_compare}, {"sceNpAuthCreateRequest",np_request},
    {"sceNpAuthCreateAsyncRequest",np_request}, {"sceNpAuthDeleteRequest",ok_void},
    {"sceNpAuthAbortRequest",ok_void}, {"sceNpAuthPollAsync",np_poll},
    {"sceNpAuthGetAuthorizationCode",np_auth_code}, {"sceNpLookupCreateTitleCtx",np_request},
    {"sceNpLookupDeleteTitleCtx",ok_void}, {"sceNpLookupCreateAsyncRequest",np_request_ctx},
    {"sceNpLookupDeleteRequest",ok_void}, {"sceNpLookupAbortRequest",ok_void},
    {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",ok_void},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",ok_void},
    {"sceNpScoreAbortRequest",ok_void}, {"sceNpWebApiInitialize",lib_init_id},
    {"sceNpWebApiTerminate",ok_void}, {"sceNpWebApiCreateContext",np_webapi_create_ctx},
    {"sceNpWebApiCreateContextA",np_webapi_create_ctx_a}, {"sceNpWebApiCreateHandle",lib_init_id},
    {"sceNpWebApiDeleteHandle",ok_void}, {"sceNpWebApiCreatePushEventFilter",lib_init_id},
    {"sceNpWebApiDeletePushEventFilter",ok_void}, {"sceNpWebApiCreateExtdPushEventFilter",np_push_filter},
    {"sceNpWebApiDeleteExtdPushEventFilter",ok_void}, {"sceNpWebApiRegisterPushEventCallback",lib_init_id},
    {"sceNpWebApiUnregisterPushEventCallback",ok_void}, {"sceNpWebApiRegisterExtdPushEventCallback",lib_init_id},
    {"sceNpWebApiRegisterExtdPushEventCallbackA",np_push_callback}, {"sceNpWebApiUnregisterExtdPushEventCallback",ok_void},
    {"sceNpWebApiCreateRequest",np_webapi_create_req}, {"sceNpWebApiSendRequest",np_webapi_send_req},
    {"sceNpWebApiSendRequest2",np_webapi_send_req2}, {"sceNpWebApiAddHttpRequestHeader",np_webapi_add_header},
    {"sceNpWebApiDeleteRequest",np_webapi_delete_req}, {"sceNpWebApiAbortRequest",ok_void},
    {"sceNpWebApiDeleteContext",ok_void}, {"sceNpWebApiReadData",np_webapi_read_data},
    {"sceNpWebApiGetHttpStatusCode",np_webapi_get_status}, {"sceNpWebApiGetHttpResponseHeaderValue",np_webapi_get_header},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",np_webapi_get_header_len}, {"sceNpWebApiGetMemoryPoolStats",np_webapi_pool_stats},
    {"sceNpWebApiUtilityParseNpId",np_webapi_parse_npid}, {"sceNpMatching2ContextStart",ok_void},
    {"sceNpMatching2ContextStop",ok_void}, {"sceNpMatching2DestroyContext",ok_void},
    {"sceNpMatching2RegisterContextCallback",ok_void}, {"sceNpMatching2RegisterLobbyEventCallback",ok_void},
    {"sceNpMatching2RegisterRoomEventCallback",ok_void}, {"sceNpMatching2RegisterSignalingCallback",ok_void},
    {"sceNpMatching2RegisterRoomMessageCallback",ok_void}, {"sceNpMatching2SetDefaultRequestOptParam",ok_void},
    {"sceNpMatching2SetSignalingOptParam",ok_void}, {"sceNpMatching2GetSignalingOptParamLocal",np_signed_out},
    {"sceNpMatching2SetUserInfo",ok_void}, {"sceNpMatching2GetUserInfoListA",np_signed_out},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2CreateJoinRoomA",np_signed_out},
    {"sceNpMatching2JoinRoom",np_signed_out}, {"sceNpMatching2JoinRoomA",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out}, {"sceNpMatching2SetRoomDataExternal",np_signed_out},
    {"sceNpMatching2GetRoomDataExternalList",np_signed_out}, {"sceNpMatching2SetRoomDataInternal",np_signed_out},
    {"sceNpMatching2GetRoomDataInternal",np_signed_out}, {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2GetRoomMemberDataInternal",np_signed_out}, {"sceNpMatching2GetRoomMemberIdListLocal",np_signed_out},
    {"sceNpMatching2GetRoomPasswordLocal",np_signed_out}, {"sceNpMatching2SendRoomChatMessage",np_signed_out},
    {"sceNpMatching2SendRoomMessage",np_signed_out}, {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out},
    {"sceNpMatching2SignalingGetPingInfo",np_signed_out}, {"sceNpMatching2SignalingGetConnectionInfoA",np_signed_out},
    {"sceNpMatching2GetMemoryInfo",ok_void}, {"sceNpMatching2GetSslMemoryInfo",ok_void},
    {"sceNpSignalingDeleteContext",ok_void}, {"sceNpSignalingActivateConnection",np_signed_out},
    {"sceNpSignalingDeactivateConnection",ok_void}, {"sceNpSignalingGetConnectionStatus",np_signed_out},
    {"sceNpScoreCreateNpTitleCtxA",lib_init_id}, {"sceNpScoreGetFriendsRankingA",np_signed_out},
    {"sceNpScoreGetFriendsRankingForCrossSave",np_signed_out}, {"sceNpScoreGetGameDataByAccountId",np_signed_out},
    {"sceNpScoreGetRankingByAccountId",np_signed_out}, {"sceNpScoreGetRankingByAccountIdForCrossSave",np_signed_out},
    {"sceNpScoreGetRankingByAccountIdPcId",np_signed_out}, {"sceNpScoreGetRankingByAccountIdPcIdForCrossSave",np_signed_out},
    {"sceNpScoreGetRankingByRangeA",np_signed_out}, {"sceNpScoreGetRankingByRangeForCrossSave",np_signed_out},
    {"sceNpScoreCensorComment",np_signed_out}, {"sceNpScoreSanitizeComment",np_signed_out},
    {"sceNpScoreGetBoardInfo",np_signed_out}, {"sceNpScoreGetGameData",np_signed_out},
    {"sceNpScoreGetRankingByNpIdPcId",np_signed_out}, {"sceNpScoreGetRankingByRange",np_signed_out},
    {"sceNpScoreRecordGameData",np_signed_out}, {"sceNpScoreRecordScore",np_signed_out},
    {"sceNpScoreSetPlayerCharacterId",ok_void}, {"sceVoiceDeletePort",ok_void},
    {"sceVoiceConnectIPortToOPort",ok_void}, {"sceVoiceDisconnectIPortFromOPort",ok_void},
    {"sceVoiceStart",ok_void}, {"sceVoiceStop",ok_void},
    {"sceNpMatching2Initialize",ok_void}, {"sceNpMatching2Terminate",ok_void},
    {"sceNpMatching2CreateContext",lib_init_id}, {"sceNpMatching2CreateContextA",lib_init_id},
    {"sceNpSignalingInitialize",ok_void}, {"sceNpSignalingTerminate",ok_void},
    {"sceNpSignalingCreateContext",lib_init_id}, {"sceNpSignalingCreateContextA",lib_init_id},
    {"sceNpCreateRequest",np_request}, {"sceNpInGameMessageInitialize",ok_void},
    {"sceNpInGameMessageTerminate",ok_void}, {"sceNpInGameMessageCreateHandle",lib_init_id},
    {"sceNpInGameMessageDeleteHandle",ok_void}, {"sceNpInGameMessagePrepareA",ok_void},
    {"sceNpInGameMessageSendDataA",ok_void}, {"sceNpBandwidthTestInitStart",lib_init_id},
    {"sceNpBandwidthTestGetStatus",ok_void}, {"sceNpBandwidthTestAbort",ok_void},
    {"sceNpBandwidthTestShutdown",ok_void}, {"sceNpWordFilterCreateTitleCtxA",lib_init_id},
    {"sceNpWordFilterCreateRequest",lib_init_id}, {"sceNpWordFilterDeleteTitleCtx",ok_void},
    {"sceNpWordFilterDeleteRequest",ok_void}, {"sceNpWordFilterAbortRequest",ok_void},
    {"sceNpWordFilterSanitizeComment",ok_void}, {"sceNpTssCreateNpTitleCtxA",lib_init_id},
    {"sceNpTssGetData",ok_void}, {"sceNpTusCreateNpTitleCtxA",lib_init_id},
    {"sceNpTusDeleteNpTitleCtx",ok_void}, {"sceNpTusCreateRequest",lib_init_id},
    {"sceNpTusDeleteRequest",ok_void}, {"sceNpTusAbortRequest",ok_void},
    {"sceNpTusGetDataA",ok_void}, {"sceNpTusGetDataAVUser",ok_void},
    {"sceNpTusGetDataForCrossSave",ok_void}, {"sceNpTusGetDataForCrossSaveVUser",ok_void},
    {"sceNpTusSetDataA",ok_void}, {"sceNpTusSetDataAVUser",ok_void},
    {"sceNpTusAddAndGetVariableA",ok_void}, {"sceNpTusAddAndGetVariableAVUser",ok_void},
    {"sceNpTusAddAndGetVariableForCrossSave",ok_void}, {"sceNpTusAddAndGetVariableForCrossSaveVUser",ok_void},
    {"sceNpTusSetMultiSlotVariableA",ok_void}, {"sceNpTusSetMultiSlotVariableVUser",ok_void},
    {"sceNpTusTryAndSetVariableA",ok_void}, {"sceNpTusTryAndSetVariableAVUser",ok_void},
    {"sceNpTusTryAndSetVariableForCrossSave",ok_void}, {"sceNpTusTryAndSetVariableForCrossSaveVUser",ok_void},
    {"sceNpTusGetFriendsDataStatusA",ok_void}, {"sceNpTusGetFriendsDataStatusForCrossSave",ok_void},
    {"sceNpTusGetFriendsVariableA",ok_void}, {"sceNpTusGetFriendsVariableForCrossSave",ok_void},
    {"sceNpTusGetMultiSlotVariableA",ok_void}, {"sceNpTusGetMultiSlotVariableAVUser",ok_void},
    {"sceNpTusGetMultiSlotVariableForCrossSave",ok_void}, {"sceNpTusGetMultiSlotVariableForCrossSaveVUser",ok_void},
    {"sceNpTusGetMultiUserDataStatusA",ok_void}, {"sceNpTusGetMultiUserDataStatusAVUser",ok_void},
    {"sceNpTusGetMultiUserDataStatusForCrossSave",ok_void}, {"sceNpTusGetMultiUserDataStatusForCrossSaveVUser",ok_void},
    {"sceNpTusGetMultiUserVariableA",ok_void}, {"sceNpTusGetMultiUserVariableAVUser",ok_void},
    {"sceNpTusGetMultiUserVariableForCrossSave",ok_void}, {"sceNpTusGetMultiUserVariableForCrossSaveVUser",ok_void},
    {"sceNpTusDeleteMultiSlotDataA",ok_void}, {"sceNpTusDeleteMultiSlotDataVUser",ok_void},
    {"sceNpSnsFacebookCreateRequest",lib_init_id}, {"sceNpSnsFacebookDeleteRequest",ok_void},
    {"sceNpSnsFacebookGetAccessToken",np_signed_out}, {"sceNpSnsFacebookDialogInitialize",ok_void},
    {"sceNpSnsFacebookDialogOpen",ok_void}, {"sceNpSnsFacebookDialogUpdateStatus",ok_void},
    {"sceNpSnsFacebookDialogTerminate",ok_void}, {"sceNpSnsFacebookDialogGetResult",ok_void},
    {"sceNpTrophyAbortHandle",ok_void}, {"sceNpTrophyCaptureScreenshot",ok_void},
    {"sceNpTrophyGetGameIcon",ok_void}, {"sceNpTrophyGetGroupIcon",ok_void},
    {"sceNpTrophyGetGroupInfo",ok_void}, {"sceNpTrophyGetTrophyIcon",ok_void},
    {"sceNpTrophyGetTrophyUnlockState",ok_void}, {"sceNpTrophyShowTrophyList",ok_void},
    {"sceSslGetMemoryPoolStats",ctx_pool_stats}, {"sceHttpUriEscape",http_uri_escape},
    {"sceNetCtlInit",netctl_init}, {"sceNetCtlTerm",netctl_term},
    {"sceNetResolverAbort",net_resolver_abort}, {"sceNpCheckNpAvailabilityA",np_check_avail},
    {"sceNpGetAccountIdA",np_account_id}, {"sceNpGetParentalControlInfoA",np_parental_control},
    {"sceNpGetUserIdByAccountId",np_user_id_by_acc_id}, {"sceNpAuthGetAuthorizationCodeA",np_auth_code},
    {"sceNpAuthGetIdTokenA",np_auth_id_token}, {"sceHttpAbortRequest",http_abort},
    {"sceHttpCreateRequestWithURL2",http_req2}, {"sceHttpGetAllResponseHeaders",http_all_headers},
    {"sceHttpParseResponseHeader",http_parse_header}, {"sceHttpGetMemoryPoolStats",ctx_pool_stats},
    {"sceNetGetMemoryPoolStats",ctx_pool_stats}, {"sceNpInGameMessageGetMemoryPoolStatistics",ctx_pool_stats},
    {"sceInvitationDialogInitialize",invitation_init}, {"sceInvitationDialogOpenA",invitation_open},
    {"sceInvitationDialogUpdateStatus",invitation_status}, {"sceInvitationDialogTerminate",invitation_term},
    {"sceInvitationDialogGetResultA",invitation_result}, {"sceNetCtlRegisterCallbackForNpToolkit",netctl_register},
    {"sceNetCtlCheckCallbackForNpToolkit",netctl_check}, {"sceNetCtlUnregisterCallbackForNpToolkit",netctl_unregister},
    {"sceNpRegisterStateCallbackForToolkit",np_reg_state_cb_toolkit}, {"sceNpUnregisterStateCallbackForToolkit",np_unreg_state_cb_toolkit},
    {"sceNpCheckCallbackForLib",np_check_cb}, {"sceUserServiceRegisterEventCallback",user_event_register},
    {"sceUserServiceUnregisterEventCallback",user_event_unregister}, {"sceMouseInit",ok_void},
    {"sceVoiceInit",ok_void}, {"sceVoiceEnd",ok_void},
};

uintptr_t runtime_net_resolve(const char *name) {
    return runtime_net_is_online() ? RUNTIME_LOOKUP(exports,name) : 0;
}
