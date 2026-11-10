#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* Собственные имена с PID: тест не удаляет ресурсы других запусков. */
int main(void)
{
    char session[100], name[150], buffer[16];
    for (int mode = 0; mode < 3; mode++) {
        snprintf(session, sizeof session, "contract_%ld_%d", (long)getpid(), mode);
        Ipc *owner = ipc_create(mode, session, 1);
        assert(owner);
        errno = 0;
        assert(!ipc_create(mode, session, 1) && errno == EEXIST);
        Ipc *worker = ipc_attach(mode, session, 0);
        assert(worker);
        errno = 0;
        assert(!ipc_attach(mode, session, 0) && errno == EBUSY);
        errno = 0;
        assert(!ipc_attach((mode + 1) % 3, session, 0) && errno == EINVAL);
        errno = 0;
        assert(ipc_recv(worker, 0, buffer, sizeof buffer, 10) == -1 && errno == ETIMEDOUT);
        assert(ipc_send(owner, 0, "ping", 4) == 0);
        /* Маленький буфер отклоняется; в SHM сообщение доступно повторно. */
        errno = 0;
        assert(ipc_recv(worker, 0, buffer, 1, 100) == -1 && errno == EMSGSIZE);
        if (mode != IPC_MQ) {
            assert(ipc_recv(worker, 0, buffer, sizeof buffer, 100) == 4);
            assert(memcmp(buffer, "ping", 4) == 0);
        }
        assert(ipc_send(worker, 0, "pong", 4) == 0);
        assert(ipc_recv(owner, 0, buffer, sizeof buffer, 100) == 4);
        assert(memcmp(buffer, "pong", 4) == 0);
        /* Унаследованный дескриптор не даёт потомку права unlink владельца. */
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            ipc_unlink(owner);
            ipc_close(owner);
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert(ipc_send(owner, 0, "a", 1) == 0);
        assert(ipc_recv(worker, 0, buffer, sizeof buffer, 100) == 1 && buffer[0] == 'a');
        ipc_close(worker);
        ipc_unlink(owner);
        ipc_unlink(owner);
        ipc_close(owner);
        errno = 0;
        assert(!ipc_attach(mode, session, 0) && errno == ENOENT);
    }
    /* Частичная ошибка создания должна сохранить уже существовавший семафор. */
    snprintf(session, sizeof session, "collision_%ld", (long)getpid());
    snprintf(name, sizeof name, "/tank3_%s_s0_1", session);
    sem_t *foreign = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    assert(foreign != SEM_FAILED);
    errno = 0;
    assert(!ipc_create(IPC_SHM_NAMED, session, 1) && errno == EEXIST);
    sem_t *still = sem_open(name, 0);
    assert(still != SEM_FAILED);
    sem_close(still);
    snprintf(name, sizeof name, "/tank3_%s_s0_0", session);
    errno = 0;
    assert(sem_open(name, 0) == SEM_FAILED && errno == ENOENT);
    snprintf(name, sizeof name, "/tank3_%s_meta", session);
    errno = 0;
    assert(shm_open(name, O_RDWR, 0) == -1 && errno == ENOENT);
    snprintf(name, sizeof name, "/tank3_%s_s0_1", session);
    sem_close(foreign);
    assert(sem_unlink(name) == 0);
    return 0;
}
