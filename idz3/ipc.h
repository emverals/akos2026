#ifndef TANK_IPC_H
#define TANK_IPC_H
#include <stddef.h>
#include <sys/types.h>

#define IPC_MAX_WORKERS 14
#define IPC_MAX_MESSAGE 8192

typedef struct Ipc Ipc;
enum IpcMode { IPC_SHM_UNNAMED, IPC_SHM_NAMED, IPC_MQ };
Ipc *ipc_create(enum IpcMode mode, const char *session, int nworkers);
Ipc *ipc_attach(enum IpcMode mode, const char *session, int worker_id);
int ipc_send(Ipc *ipc, int worker_id, const void *data, size_t length);
ssize_t ipc_recv(Ipc *ipc, int worker_id, void *data, size_t capacity, int timeout_ms);
void ipc_close(Ipc *ipc);
void ipc_unlink(Ipc *ipc);
const char *ipc_error(void);
#endif
