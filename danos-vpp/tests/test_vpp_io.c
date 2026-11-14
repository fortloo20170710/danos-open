#include "../src/api/vpp_wire.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

static void *slow_frame(void *arg)
{
    int fd = *(int *)arg;
    uint8_t frame[26] = {0};
    frame[11] = 10; frame[17] = 1;
    for (unsigned i = 0; i < sizeof(frame); i++) {
        if (send(fd, frame+i, 1, MSG_NOSIGNAL) != 1) break;
        usleep(15000);
    }
    return NULL;
}
int main(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, slow_frame, &pair[1]) == 0);
    uint8_t reply[128];
    uint64_t start = vpp_wire_now_ms();
    assert(vpp_wire_recv_until(pair[0], reply, sizeof(reply), start+100) < 0);
    assert(vpp_wire_now_ms() - start < 1000);
    close(pair[0]);
    pthread_join(thread, NULL); close(pair[1]);
    puts("[PASS] slow partial frame cannot renew receive deadline");

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int size = 4096;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
    uint8_t *payload = calloc(1, 1024*1024);
    assert(payload);
    start = vpp_wire_now_ms();
    assert(vpp_wire_send_until(pair[0], 1, payload, 1024*1024, start+100) < 0);
    assert(vpp_wire_now_ms() - start < 1000);
    free(payload); close(pair[0]); close(pair[1]);
    puts("[PASS] non-reading peer cannot block sender indefinitely");
    return 0;
}
