#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x544b3331u
#define SESSION_MAX 100
#define NAME_MAXLEN 160
#define SEMS 4

typedef struct { size_t length; unsigned char bytes[IPC_MAX_MESSAGE]; } Message;
typedef struct { sem_t sem[SEMS]; Message msg[2]; } Slot;
typedef struct {
    uint32_t magic;
    int mode, nworkers, ready;
    pid_t owner, registered[IPC_MAX_WORKERS];
    Slot slots[];
} Shared;
struct Ipc {
    enum IpcMode mode;
    char session[SESSION_MAX + 1];
    int nworkers, worker, fd, owner, unlinked;
    pid_t owner_pid;
    size_t map_size;
    Shared *shared;
    sem_t *sem[IPC_MAX_WORKERS][SEMS];
    mqd_t mq[IPC_MAX_WORKERS][2];
    unsigned char sem_created[IPC_MAX_WORKERS][SEMS];
    unsigned char mq_created[IPC_MAX_WORKERS][2];
    int shm_created, sem_initialized;
};
static _Thread_local char error_text[256];
static void fail(const char *what) { int e=errno; snprintf(error_text,sizeof error_text,"%s: %s",what,strerror(e)); errno=e; }
const char *ipc_error(void) { return error_text; }
static int valid(enum IpcMode mode, const char *s) {
    if (mode < IPC_SHM_UNNAMED || mode > IPC_MQ || !s || !*s || strlen(s)>SESSION_MAX) return 0;
    for (; *s; ++s) if (!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-')) return 0;
    return 1;
}
static void name(char *out, const Ipc *p, char kind, int worker, int item) {
    if (kind=='m') snprintf(out, NAME_MAXLEN, "/tank3_%s_meta", p->session);
    else snprintf(out, NAME_MAXLEN, "/tank3_%s_%c%d_%d",p->session,kind,worker,item);
}
static size_t mapsize(enum IpcMode mode, int n) { return sizeof(Shared)+(mode==IPC_MQ?0:(size_t)n*sizeof(Slot)); }
static Ipc *alloc_ipc(enum IpcMode mode, const char *session) {
    Ipc *p=calloc(1,sizeof *p); if (!p) return NULL;
    p->mode=mode; strcpy(p->session,session); p->fd=-1; p->worker=-1; p->shared=MAP_FAILED;
    for (int i=0;i<IPC_MAX_WORKERS;i++) { for(int j=0;j<SEMS;j++) p->sem[i][j]=SEM_FAILED; for(int j=0;j<2;j++) p->mq[i][j]=(mqd_t)-1; }
    return p;
}
static int deadline(struct timespec *ts, int ms) {
    if (clock_gettime(CLOCK_REALTIME,ts)) return -1;
    ts->tv_sec+=ms/1000; ts->tv_nsec+=(long)(ms%1000)*1000000L;
    if(ts->tv_nsec>=1000000000L){ts->tv_sec++;ts->tv_nsec-=1000000000L;} return 0;
}
static int lock_metadata(int fd) {
    struct timespec end, now;
    if (clock_gettime(CLOCK_MONOTONIC, &end)) return -1;
    end.tv_sec += 5;
    for (;;) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) return 0;
        if (errno != EWOULDBLOCK && errno != EAGAIN) return -1;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        if (now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) {
            errno = ETIMEDOUT; return -1;
        }
        struct timespec pause = {0, 10000000L};
        if (now.tv_sec == end.tv_sec && end.tv_nsec - now.tv_nsec < pause.tv_nsec)
            pause.tv_nsec = end.tv_nsec - now.tv_nsec;
        if (nanosleep(&pause, NULL)) return -1;
    }
}
static int wait_sem(sem_t *s, const struct timespec *end) { return end ? sem_timedwait(s,end) : sem_wait(s); }
static int open_objects(Ipc *p, int create) {
    char nm[NAME_MAXLEN];
    for(int i=0;i<p->nworkers;i++) {
        if (!create && p->worker!=i) continue;
        if(p->mode!=IPC_SHM_UNNAMED) {
            int count=p->mode==IPC_MQ?2:SEMS;
            for(int j=0;j<count;j++) {
                name(nm,p,'s',i,j);
                p->sem[i][j]=create?sem_open(nm,O_CREAT|O_EXCL,0600,(p->mode==IPC_SHM_NAMED && j%2==0)?1:0):sem_open(nm,0);
                if(p->sem[i][j]==SEM_FAILED) return -1;
                if(create) p->sem_created[i][j]=1;
            }
        } else for(int j=0;j<SEMS;j++) p->sem[i][j]=&p->shared->slots[i].sem[j];
        if(p->mode==IPC_MQ) for(int j=0;j<2;j++) {
            struct mq_attr a={0}; a.mq_maxmsg=2; a.mq_msgsize=IPC_MAX_MESSAGE;
            name(nm,p,'q',i,j);
            p->mq[i][j]=create?mq_open(nm,O_CREAT|O_EXCL|O_RDWR,0600,&a):mq_open(nm,O_RDWR);
            if(p->mq[i][j]==(mqd_t)-1) return -1;
            if(create) p->mq_created[i][j]=1;
            if(mq_getattr(p->mq[i][j],&a)) return -1;
            if(a.mq_msgsize!=IPC_MAX_MESSAGE || a.mq_maxmsg!=2) { errno=EINVAL; return -1; }
        }
    }
    return 0;
}
Ipc *ipc_create(enum IpcMode mode, const char *session, int nworkers) {
    if(!valid(mode,session)||nworkers<1||nworkers>IPC_MAX_WORKERS){errno=EINVAL;fail("ipc_create");return NULL;}
    Ipc *p=alloc_ipc(mode,session); if(!p){fail("allocate");return NULL;}
    p->owner=1;p->owner_pid=getpid();p->nworkers=nworkers;p->map_size=mapsize(mode,nworkers);
    char nm[NAME_MAXLEN]; name(nm,p,'m',0,0);
    p->fd=shm_open(nm,O_CREAT|O_EXCL|O_RDWR,0600); if(p->fd<0) goto error;
    p->shm_created=1;
    if(lock_metadata(p->fd)||ftruncate(p->fd,(off_t)p->map_size)) goto error;
    p->shared=mmap(NULL,p->map_size,PROT_READ|PROT_WRITE,MAP_SHARED,p->fd,0); if(p->shared==MAP_FAILED) goto error;
    memset(p->shared,0,p->map_size);
    p->shared->magic=MAGIC;p->shared->mode=mode;p->shared->nworkers=nworkers;p->shared->owner=p->owner_pid;
    if(mode==IPC_SHM_UNNAMED) for(int i=0;i<nworkers;i++) for(int j=0;j<SEMS;j++) {
        if(sem_init(&p->shared->slots[i].sem[j],1,j%2==0?1:0)) goto error;
        p->sem_initialized++;
    }
    if(open_objects(p,1)) goto error;
    p->shared->ready=1;
    if(flock(p->fd,LOCK_UN)) goto error;
    return p;
error: { int e=errno; fail("ipc_create"); ipc_unlink(p); ipc_close(p); errno=e; return NULL; }
}
Ipc *ipc_attach(enum IpcMode mode, const char *session, int worker_id) {
    if(!valid(mode,session)||worker_id<0||worker_id>=IPC_MAX_WORKERS){errno=EINVAL;fail("ipc_attach");return NULL;}
    Ipc *p=alloc_ipc(mode,session); if(!p){fail("allocate");return NULL;}
    char nm[NAME_MAXLEN];name(nm,p,'m',0,0);
    p->fd=shm_open(nm,O_RDWR,0); if(p->fd<0) goto error;
    if(lock_metadata(p->fd)) goto error;
    struct stat st; if(fstat(p->fd,&st)) goto error;
    if(st.st_size<(off_t)sizeof(Shared)){errno=EINVAL;goto error;}
    p->map_size=(size_t)st.st_size;
    if(p->map_size>mapsize(IPC_SHM_UNNAMED,IPC_MAX_WORKERS)){errno=EINVAL;goto error;}
    p->shared=mmap(NULL,p->map_size,PROT_READ|PROT_WRITE,MAP_SHARED,p->fd,0); if(p->shared==MAP_FAILED) goto error;
    if(p->shared->magic!=MAGIC||!p->shared->ready||p->shared->mode!=(int)mode||p->shared->nworkers<1||p->shared->nworkers>IPC_MAX_WORKERS||worker_id>=p->shared->nworkers||p->map_size!=mapsize(mode,p->shared->nworkers)) {errno=EINVAL;goto error;}
    p->nworkers=p->shared->nworkers;
    if(p->shared->registered[worker_id]) {errno=EBUSY;goto error;}
    p->worker=worker_id;
    if(open_objects(p,0)) {p->worker=-1;goto error;}
    p->shared->registered[worker_id]=getpid();
    if(flock(p->fd,LOCK_UN)) goto error;
    return p;
error: {int e=errno;fail("ipc_attach");ipc_close(p);errno=e;return NULL;}
}
static int check(Ipc *p,int id,const void *data) {
    if(!p||!data||id<0||id>=p->nworkers||(!p->owner&&id!=p->worker)||(p->owner&&p->owner_pid!=getpid())) {errno=EINVAL;return -1;} return 0;
}
int ipc_send(Ipc *p,int id,const void *data,size_t len) {
    if(check(p,id,data) || len>IPC_MAX_MESSAGE){errno=EINVAL;fail("ipc_send");return -1;}
    struct timespec end; if(deadline(&end,5000)) goto error;
    int dir=p->owner?0:1;
    if(p->mode==IPC_MQ) {
        if(mq_timedsend(p->mq[id][dir],data,len,0,&end)) goto error;
        if(sem_post(p->sem[id][dir])) goto error;
    } else {
        if(wait_sem(p->sem[id][dir*2],&end)) goto error;
        Message *m=&p->shared->slots[id].msg[dir];m->length=len;memcpy(m->bytes,data,len);
        if(sem_post(p->sem[id][dir*2+1])) goto error;
    }
    return 0;
error: fail("ipc_send");return -1;
}
ssize_t ipc_recv(Ipc *p,int id,void *data,size_t capacity,int timeout_ms) {
    if(check(p,id,data)||timeout_ms < -1){errno=EINVAL;fail("ipc_recv");return -1;}
    struct timespec end,*limit=NULL; if(timeout_ms>=0){if(deadline(&end,timeout_ms)) goto error;limit=&end;}
    int dir=p->owner?1:0;
    if(p->mode==IPC_MQ) {
        if(wait_sem(p->sem[id][dir],limit)) goto error;
        char buf[IPC_MAX_MESSAGE]; ssize_t n=limit?mq_timedreceive(p->mq[id][dir],buf,sizeof buf,NULL,limit):mq_receive(p->mq[id][dir],buf,sizeof buf,NULL);
        if(n<0){int e=errno;sem_post(p->sem[id][dir]);errno=e;goto error;}
        if((size_t)n>capacity){errno=EMSGSIZE;goto error;} memcpy(data,buf,(size_t)n);return n;
    }
    if(wait_sem(p->sem[id][dir*2+1],limit)) goto error;
    Message *m=&p->shared->slots[id].msg[dir];size_t n=m->length;
    if(n>capacity||n>IPC_MAX_MESSAGE){sem_post(p->sem[id][dir*2+1]);errno=EMSGSIZE;goto error;}
    memcpy(data,m->bytes,n);if(sem_post(p->sem[id][dir*2])) goto error;return (ssize_t)n;
error: fail("ipc_recv");return -1;
}
void ipc_unlink(Ipc *p) {
    if(!p||!p->owner||p->owner_pid!=getpid()||p->unlinked) return;
    p->unlinked=1;char nm[NAME_MAXLEN];
    if(p->mode==IPC_SHM_UNNAMED && p->shared!=MAP_FAILED)
        for(int k=0;k<p->sem_initialized;k++) sem_destroy(&p->shared->slots[k/SEMS].sem[k%SEMS]);
    for(int i=0;i<p->nworkers;i++) {
        for(int j=0;j<SEMS;j++) if(p->sem_created[i][j]) {name(nm,p,'s',i,j);sem_unlink(nm);}
        for(int j=0;j<2;j++) if(p->mq_created[i][j]) {name(nm,p,'q',i,j);mq_unlink(nm);}
    }
    if(p->shm_created){name(nm,p,'m',0,0);shm_unlink(nm);}
}
void ipc_close(Ipc *p) {
    if(!p)return;
    if(!p->owner && p->worker>=0 && p->shared!=MAP_FAILED && p->fd>=0 && lock_metadata(p->fd)==0) {
        if(p->shared->registered[p->worker]==getpid())p->shared->registered[p->worker]=0;
        flock(p->fd,LOCK_UN);
    }
    for(int i=0;i<IPC_MAX_WORKERS;i++) {
        if(p->mode!=IPC_SHM_UNNAMED) for(int j=0;j<SEMS;j++) if(p->sem[i][j]!=SEM_FAILED) sem_close(p->sem[i][j]);
        for(int j=0;j<2;j++) if(p->mq[i][j]!=(mqd_t)-1)mq_close(p->mq[i][j]);
    }
    if(p->shared!=MAP_FAILED)munmap(p->shared,p->map_size);
    if(p->fd>=0)close(p->fd);
    free(p);
}
