/* test_threads.c - several contexts running in parallel threads (run it under ThreadSanitizer).
 * SPDX-License-Identifier: MIT */
#include <pthread.h>

#include "../net_test.h"

/*
 * Threading rule: one context belongs to one thread. This test follows it and checks that
 * independent contexts do not interfere (no shared mutable state in the library): each thread
 * owns a server and a client context and runs ping/pong traffic on its own loopback port.
 * With -fsanitize=thread (cmake --preset tsan) any data race is reported as a failure.
 */
#define THREADS 4
#define ROUNDS 2000

typedef struct {
    int ok;
} result;

static void* worker(void* arg)
{
    result* r = arg;
    side S = {0}, C = {0};
    S.ctx = dx_create(NULL);
    C.ctx = dx_create(NULL);
    if (!S.ctx || !C.ctx) return NULL;
    S.reply = 1;
    dx_listen(S.ctx, "127.0.0.1", 0);
    uint32_t conn;
    dx_connect(C.ctx, "127.0.0.1", (uint16_t)dx_listen_port(S.ctx), &conn);
    WAIT_UNTIL(S, C, S.connected == 1 && C.connected == 1);

    for (uint32_t i = 0; i < ROUNDS; i++) {
        uint8_t b[4] = {(uint8_t)i, (uint8_t)(i >> 8), 0, 0};
        while (dx_send(C.ctx, conn, DX_PKT_PING, b, 4) == DX_ERR_WOULD_BLOCK) {
            pump_side(&S, 0);
            pump_side(&C, 0);
        }
        pump_side(&S, 0);
        pump_side(&C, 0);
    }
    WAIT_UNTIL(S, C, C.pongs == ROUNDS);
    r->ok = C.pongs == ROUNDS && S.bad == 0 && S.disconnected == 0;
    dx_destroy(S.ctx);
    dx_destroy(C.ctx);
    return NULL;
}

int main(void)
{
    pthread_t t[THREADS];
    result res[THREADS] = {{0}};
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, worker, &res[i]);
    for (int i = 0; i < THREADS; i++) {
        pthread_join(t[i], NULL);
        CHECK(res[i].ok);
    }
    return dx_test_done();
}
