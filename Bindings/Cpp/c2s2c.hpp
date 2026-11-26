// c2s2c.hpp - C++17 header-only wrapper over the C2S2C C API (RAII, exceptions, lambdas).
// SPDX-License-Identifier: MIT
//
// The C library hands out raw pointers that must be released in the right order
// and reports errors as negative numbers. This wrapper turns that into C++ habits:
//   - Context owns the library context and frees it automatically;
//   - Batch owns the packets of one poll and releases them when it goes out of scope, so you
//     cannot use a packet pointer after it was given back;
//   - errors are thrown as c2s2c::Error (carrying the C2S2C_ERR_* code and its message);
//   - handlers and hooks are std::function (lambdas work), run in C++ over batched polls.
#pragma once

#include <C2S2C/c2s2c.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace c2s2c
{

/// An error from the library: code() is the C2S2C_ERR_* value, what() its text.
class Error : public std::runtime_error
{
  public:
    explicit Error(int code) : std::runtime_error(c2s2c_strerror(code)), code_(code) {}
    int code() const { return code_; }

  private:
    int code_;
};

/// Throws Error if rc is negative, otherwise returns rc.
inline int check(int rc)
{
    if (rc < 0) throw Error(rc);
    return rc;
}

/// Writes payload fields into a buffer you own. Errors are thrown by size().
class Writer
{
  public:
    Writer(uint8_t* buf, uint32_t cap) { c2s2c_writer_init(&w_, buf, cap); }
    Writer& u8(uint8_t v)
    {
        c2s2c_w_u8(&w_, v);
        return *this;
    }
    Writer& u16(uint16_t v)
    {
        c2s2c_w_u16(&w_, v);
        return *this;
    }
    Writer& u32(uint32_t v)
    {
        c2s2c_w_u32(&w_, v);
        return *this;
    }
    Writer& u64(uint64_t v)
    {
        c2s2c_w_u64(&w_, v);
        return *this;
    }
    Writer& f32(float v)
    {
        c2s2c_w_f32(&w_, v);
        return *this;
    }
    Writer& f64(double v)
    {
        c2s2c_w_f64(&w_, v);
        return *this;
    }
    Writer& string(std::string_view s)
    {
        c2s2c_w_string(&w_, s.data(), static_cast<uint16_t>(s.size()));
        return *this;
    }
    Writer& bytes(const uint8_t* p, uint32_t n)
    {
        c2s2c_w_bytes(&w_, p, n);
        return *this;
    }
    /// Number of bytes written; throws Error if any write failed.
    uint32_t size() const
    {
        check(w_.err);
        return w_.len;
    }

  private:
    c2s2c_writer w_;
};

/// Reads payload fields; every call throws Error when the data is malformed or too short.
/// Views returned by string() and bytes() point into the packet and die with it.
class Reader
{
  public:
    Reader(const uint8_t* data, uint32_t len) { c2s2c_reader_init(&r_, data, len); }
    uint8_t u8() { return ok(c2s2c_r_u8(&r_)); }
    uint16_t u16() { return ok(c2s2c_r_u16(&r_)); }
    uint32_t u32() { return ok(c2s2c_r_u32(&r_)); }
    uint64_t u64() { return ok(c2s2c_r_u64(&r_)); }
    float f32() { return ok(c2s2c_r_f32(&r_)); }
    double f64() { return ok(c2s2c_r_f64(&r_)); }
    std::string_view string()
    {
        uint16_t n = 0;
        const char* s = c2s2c_r_string(&r_, &n);
        check(r_.err);
        return {s, n};
    }
    std::pair<const uint8_t*, uint32_t> bytes()
    {
        uint32_t n = 0;
        const uint8_t* p = c2s2c_r_bytes(&r_, &n);
        check(r_.err);
        return {p, n};
    }

  private:
    template <class T> T ok(T v)
    {
        check(r_.err);
        return v;
    }
    c2s2c_reader r_;
};

/// A received packet. Valid only while the Batch (or the handler call) that gave it is alive.
class Packet
{
  public:
    explicit Packet(const c2s2c_packet& p) : p_(p) {}
    uint32_t conn() const { return p_.conn; }
    uint16_t id() const { return p_.packet_id; }
    uint32_t sequence() const { return p_.sequence; }
    const uint8_t* data() const { return p_.data; }
    uint32_t size() const { return p_.length; }
    Reader reader() const { return Reader(p_.data, p_.length); }
    /// Reason of a C2S2C_SYS_DISCONNECTED event (0 = normal close, negative = C2S2C_ERR_*).
    int reason() const { return static_cast<int32_t>(reader().u32()); }

  private:
    c2s2c_packet p_;
};

/// The packets of one Context::poll call. Releases them to the library when destroyed.
class Batch
{
  public:
    Batch(c2s2c_ctx* ctx, std::vector<c2s2c_packet> packets)
        : ctx_(ctx), packets_(std::move(packets))
    {
    }
    Batch(Batch&& o) noexcept : ctx_(o.ctx_), packets_(std::move(o.packets_)) { o.ctx_ = nullptr; }
    Batch& operator=(Batch&&) = delete;
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    ~Batch()
    {
        if (ctx_ && !packets_.empty()) c2s2c_release(ctx_, static_cast<int>(packets_.size()));
    }
    size_t size() const { return packets_.size(); }
    Packet operator[](size_t i) const { return Packet(packets_[i]); }

  private:
    c2s2c_ctx* ctx_;
    std::vector<c2s2c_packet> packets_;
};

/// Callbacks for events; every member is optional. The Context must outlive the callbacks' use.
struct Hooks {
    std::function<void(uint32_t conn)> on_connect;
    std::function<void(uint32_t conn, int reason)> on_disconnect;
    std::function<void(uint32_t conn, int error)> on_error;
    std::function<bool(uint32_t conn, uint16_t id, const uint8_t* data, uint32_t len)>
        before_send;                               // true = reject
    std::function<bool(const Packet&)> after_recv; // true = drop
};

/// One network engine. Use it from one thread. Not copyable; freed on destruction.
/// Handlers and hooks run here, in C++, over one batched poll: the C library never calls back.
class Context
{
  public:
    explicit Context(const c2s2c_config* cfg = nullptr) : ctx_(c2s2c_create(cfg))
    {
        if (!ctx_) throw Error(C2S2C_ERR_NO_MEMORY);
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    void listen(const std::string& addr, uint16_t port = 0)
    {
        check(c2s2c_listen(ctx_.get(), addr.c_str(), port));
    }
    int port() const { return check(c2s2c_listen_port(ctx_.get())); }
    uint32_t connect(const std::string& addr, uint16_t port)
    {
        uint32_t conn = 0;
        check(c2s2c_connect(ctx_.get(), addr.c_str(), port, &conn));
        return conn;
    }
    void close(uint32_t conn) { check(c2s2c_close(ctx_.get(), conn)); }

    /// Queues a packet (written at the next poll/dispatch, or by flush). Throws
    /// Error(C2S2C_ERR_WOULD_BLOCK) if the send buffer is full (see try_send), and
    /// Error(C2S2C_ERR_REJECTED) if the before_send hook refuses it.
    void send(uint32_t conn, uint16_t id, const uint8_t* data, uint32_t len)
    {
        check(raw_send(conn, id, data, len));
    }
    /// Like send, but returns false instead of throwing when the send buffer is full.
    bool try_send(uint32_t conn, uint16_t id, const uint8_t* data, uint32_t len)
    {
        int rc = raw_send(conn, id, data, len);
        if (rc == C2S2C_ERR_WOULD_BLOCK) return false;
        check(rc);
        return true;
    }
    /// Writes queued packets now (only needed when no poll/dispatch follows).
    void flush() { check(c2s2c_flush(ctx_.get())); }

    /// Waits up to timeout_ms and returns up to max packets, released when the Batch dies.
    Batch poll(int timeout_ms, int max = 64)
    {
        std::vector<c2s2c_packet> pk(static_cast<size_t>(max));
        int n = check(c2s2c_poll(ctx_.get(), pk.data(), max, timeout_ms));
        pk.resize(static_cast<size_t>(n));
        return Batch(ctx_.get(), std::move(pk));
    }

    using Handler = std::function<void(Context&, const Packet&)>;
    /// Calls fn for packets with this id (0 = default handler); an empty function removes it.
    void on_packet(uint16_t id, Handler fn)
    {
        if (fn)
            handlers_[id] = std::move(fn);
        else
            handlers_.erase(id);
    }
    /// Installs hooks (replacing earlier ones).
    void set_hooks(Hooks hooks) { hooks_ = std::move(hooks); }

    /// Polls once, then runs hooks and handlers for the whole batch and releases it.
    /// Returns the number of entries handled.
    int dispatch(int timeout_ms, int max = 64)
    {
        Batch batch = poll(timeout_ms, max);
        for (size_t i = 0; i < batch.size(); i++)
            handle(batch[i]);
        return static_cast<int>(batch.size());
    }

    /// The underlying C context, for features the wrapper does not cover (c2s2c_tunnel...).
    c2s2c_ctx* raw() { return ctx_.get(); }

  private:
    int raw_send(uint32_t conn, uint16_t id, const uint8_t* data, uint32_t len)
    {
        if (hooks_.before_send && hooks_.before_send(conn, id, data, len))
            return C2S2C_ERR_REJECTED;
        return c2s2c_send(ctx_.get(), conn, id, data, len);
    }
    void handle(const Packet& p)
    {
        if (p.id() == C2S2C_SYS_CONNECTED) {
            if (hooks_.on_connect) hooks_.on_connect(p.conn());
        } else if (p.id() == C2S2C_SYS_DISCONNECTED) {
            int reason = p.reason();
            if (reason < 0 && hooks_.on_error) hooks_.on_error(p.conn(), reason);
            if (hooks_.on_disconnect) hooks_.on_disconnect(p.conn(), reason);
        } else if (!(hooks_.after_recv && hooks_.after_recv(p))) {
            auto it = handlers_.find(p.id());
            if (it == handlers_.end()) it = handlers_.find(0); // default handler
            if (it != handlers_.end()) it->second(*this, p);
        }
    }
    struct Deleter {
        void operator()(c2s2c_ctx* c) const { c2s2c_destroy(c); }
    };
    std::unique_ptr<c2s2c_ctx, Deleter> ctx_;
    std::map<uint16_t, Handler> handlers_;
    Hooks hooks_;
};

} // namespace c2s2c
