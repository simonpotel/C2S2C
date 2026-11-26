/* binding.c - Node.js native addon (N-API): the thin layer between JavaScript and libduplex.
 * SPDX-License-Identifier: MIT */
#include <node_api.h>
#include <stdlib.h>
#include <string.h>

#include "Duplex/duplex.h"

/*
 * This file exposes the C functions to JavaScript with the smallest possible
 * glue. Every function returns the library's result code (negative = error); the friendly
 * API, error classes, handlers and hooks live in index.ts.
 *
 * A poll crosses into JavaScript once per batch: the packet descriptions come as one Uint32Array
 * and all payloads as one Buffer (a single copy), never one call per packet.
 */

#define CALL(env, expr)                                                                            \
    do {                                                                                           \
        if ((expr) != napi_ok) {                                                                   \
            napi_throw_error((env), NULL, "N-API call failed: " #expr);                            \
            return NULL;                                                                           \
        }                                                                                          \
    } while (0)

static napi_value make_int(napi_env env, int value)
{
    napi_value v;
    napi_create_int32(env, value, &v);
    return v;
}

static int arg_int(napi_env env, napi_value v, int32_t* out)
{
    return napi_get_value_int32(env, v, out) == napi_ok;
}

static dx_ctx* arg_ctx(napi_env env, napi_value v)
{
    void* ptr = NULL;
    if (napi_get_value_external(env, v, &ptr) != napi_ok) return NULL;
    return ptr;
}

/* Reads an optional numeric property of the options object (0 = keep the default). */
static uint32_t opt_u32(napi_env env, napi_value obj, const char* name)
{
    napi_value v;
    uint32_t out = 0;
    bool has = false;
    napi_has_named_property(env, obj, name, &has);
    if (has && napi_get_named_property(env, obj, name, &v) == napi_ok)
        napi_get_value_uint32(env, v, &out);
    return out;
}

/* create(options) -> external context, or an Error with the library's code. */
static napi_value js_create(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1];
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    dx_config cfg;
    dx_config_default(&cfg);
    if (argc > 0) {
        cfg.max_conns =
            opt_u32(env, argv[0], "maxConns") ? opt_u32(env, argv[0], "maxConns") : cfg.max_conns;
        uint32_t payload = opt_u32(env, argv[0], "maxPayload");
        if (payload) {
            cfg.max_payload = payload;
            cfg.tx_bytes = 4 * (payload + DX_HEADER_SIZE);
        }
        if (opt_u32(env, argv[0], "rxQueue")) cfg.rx_queue = opt_u32(env, argv[0], "rxQueue");
        if (opt_u32(env, argv[0], "idleTimeoutMs"))
            cfg.idle_timeout_ms = opt_u32(env, argv[0], "idleTimeoutMs");
        if (opt_u32(env, argv[0], "rxRingBytes"))
            cfg.rx_ring_bytes = opt_u32(env, argv[0], "rxRingBytes");
    }
    dx_ctx* ctx = dx_create(&cfg);
    if (!ctx) {
        napi_throw_error(env, NULL, "dx_create failed (invalid settings or out of memory)");
        return NULL;
    }
    napi_value ext;
    CALL(env, napi_create_external(env, ctx, NULL, NULL, &ext)); /* freed by destroy() */
    return ext;
}

static napi_value js_destroy(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1];
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    dx_destroy(arg_ctx(env, argv[0]));
    return NULL;
}

/* listen(ctx, address, port), connect(ctx, address, port) -> result code / connection handle */
static napi_value addr_call(napi_env env, napi_callback_info info, int connect)
{
    size_t argc = 3;
    napi_value argv[3];
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    char addr[128];
    size_t n = 0;
    int32_t port = 0;
    if (argc < 3 || napi_get_value_string_utf8(env, argv[1], addr, sizeof addr, &n) != napi_ok ||
        !arg_int(env, argv[2], &port))
        return make_int(env, DX_ERR_INVALID_ARG);
    dx_ctx* ctx = arg_ctx(env, argv[0]);
    if (!connect) return make_int(env, dx_listen(ctx, addr, (uint16_t)port));
    uint32_t conn = 0;
    int rc = dx_connect(ctx, addr, (uint16_t)port, &conn);
    napi_value out;
    if (rc < 0) return make_int(env, rc);
    CALL(env, napi_create_uint32(env, conn, &out)); /* handles are never negative numbers */
    return out;
}

static napi_value js_listen(napi_env env, napi_callback_info info)
{
    return addr_call(env, info, 0);
}

static napi_value js_connect(napi_env env, napi_callback_info info)
{
    return addr_call(env, info, 1);
}

static napi_value js_port(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1];
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    return make_int(env, dx_listen_port(arg_ctx(env, argv[0])));
}

/* close(ctx, conn) */
static napi_value js_close(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2];
    uint32_t conn = 0;
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    napi_get_value_uint32(env, argv[1], &conn);
    return make_int(env, dx_close(arg_ctx(env, argv[0]), conn));
}

/* send(ctx, conn, packetId, Uint8Array) -> result code */
static napi_value js_send(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value argv[4];
    uint32_t conn = 0, id = 0;
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    napi_get_value_uint32(env, argv[1], &conn);
    napi_get_value_uint32(env, argv[2], &id);
    void* data = NULL;
    size_t len = 0;
    if (napi_get_typedarray_info(env, argv[3], NULL, &len, &data, NULL, NULL) != napi_ok)
        return make_int(env, DX_ERR_INVALID_ARG);
    return make_int(env, dx_send(arg_ctx(env, argv[0]), conn, (uint16_t)id, data, (uint32_t)len));
}

/* sendBatch(ctx, meta: Uint32Array [conn, id, length] * n, payloads: Uint8Array) -> how many
 * were queued, or the first error. The payloads are concatenated in order. Two arguments instead
 * of an array of objects: reading JavaScript objects from C costs one call per property. */
static napi_value js_send_batch(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value argv[3];
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    void *meta = NULL, *data = NULL;
    size_t meta_len = 0, data_len = 0;
    if (napi_get_typedarray_info(env, argv[1], NULL, &meta_len, &meta, NULL, NULL) != napi_ok ||
        napi_get_typedarray_info(env, argv[2], NULL, &data_len, &data, NULL, NULL) != napi_ok)
        return make_int(env, DX_ERR_INVALID_ARG);
    dx_ctx* ctx = arg_ctx(env, argv[0]);
    const uint32_t* m = meta;
    size_t n = meta_len / 3, offset = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t len = m[3 * i + 2];
        if (offset + len > data_len) return make_int(env, i ? (int)i : DX_ERR_INVALID_ARG);
        int rc = dx_send(ctx, m[3 * i], (uint16_t)m[3 * i + 1], (const uint8_t*)data + offset, len);
        if (rc != DX_OK) return make_int(env, i ? (int)i : rc);
        offset += len;
    }
    return make_int(env, (int)n);
}

static napi_value js_flush(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1];
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    return make_int(env, dx_flush(arg_ctx(env, argv[0])));
}

/* poll(ctx, timeoutMs, max) -> [meta, payloads] or a negative code.
 *   meta:     Uint32Array of [conn, id, sequence, length] per packet
 *   payloads: one Buffer holding every payload of the batch, in order (copied once)
 * Two allocations per batch instead of several N-API calls per packet: crossing into JavaScript
 * is what costs, and a copy into JavaScript memory can never dangle after the release. */
static napi_value js_poll(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value argv[3];
    int32_t timeout = 0, max = 64;
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    arg_int(env, argv[1], &timeout);
    arg_int(env, argv[2], &max);
    if (max < 1 || max > 1024) return make_int(env, DX_ERR_INVALID_ARG);

    dx_packet pk[1024];
    dx_ctx* ctx = arg_ctx(env, argv[0]);
    int n = dx_poll(ctx, pk, max, timeout);
    if (n < 0) return make_int(env, n);

    size_t total = 0;
    for (int i = 0; i < n; i++)
        total += pk[i].length;
    void *meta_mem = NULL, *data_mem = NULL;
    napi_value meta_ab, meta, data, out;
    CALL(env, napi_create_arraybuffer(env, (size_t)n * 16u, &meta_mem, &meta_ab));
    CALL(env, napi_create_typedarray(env, napi_uint32_array, (size_t)n * 4u, meta_ab, 0, &meta));
    CALL(env, napi_create_buffer(env, total ? total : 1, &data_mem, &data));
    uint32_t* m = meta_mem;
    size_t offset = 0;
    for (int i = 0; i < n; i++) {
        m[4 * i] = pk[i].conn;
        m[4 * i + 1] = pk[i].packet_id;
        m[4 * i + 2] = pk[i].sequence;
        m[4 * i + 3] = pk[i].length;
        if (pk[i].length) memcpy((uint8_t*)data_mem + offset, pk[i].data, pk[i].length);
        offset += pk[i].length;
    }
    CALL(env, napi_create_array_with_length(env, 2, &out));
    CALL(env, napi_set_element(env, out, 0, meta));
    CALL(env, napi_set_element(env, out, 1, data));
    return out;
}

/* release(ctx, count) */
static napi_value js_release(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2];
    int32_t count = 0;
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    arg_int(env, argv[1], &count);
    dx_release(arg_ctx(env, argv[0]), count);
    return NULL;
}

static napi_value js_strerror(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1], out;
    int32_t code = 0;
    CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    arg_int(env, argv[0], &code);
    CALL(env, napi_create_string_utf8(env, dx_strerror(code), NAPI_AUTO_LENGTH, &out));
    return out;
}

static napi_value js_abi_version(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value out;
    CALL(env, napi_create_uint32(env, dx_abi_version(), &out));
    return out;
}

NAPI_MODULE_INIT()
{
    static const struct {
        const char* name;
        napi_callback fn;
    } functions[] = {{"create", js_create},
                     {"destroy", js_destroy},
                     {"listen", js_listen},
                     {"connect", js_connect},
                     {"port", js_port},
                     {"close", js_close},
                     {"send", js_send},
                     {"sendBatch", js_send_batch},
                     {"flush", js_flush},
                     {"poll", js_poll},
                     {"release", js_release},
                     {"strerror", js_strerror},
                     {"abiVersion", js_abi_version}};
    for (size_t i = 0; i < sizeof functions / sizeof functions[0]; i++) {
        napi_value fn;
        if (napi_create_function(env, functions[i].name, NAPI_AUTO_LENGTH, functions[i].fn, NULL,
                                 &fn) != napi_ok ||
            napi_set_named_property(env, exports, functions[i].name, fn) != napi_ok)
            return NULL;
    }
    return exports;
}
