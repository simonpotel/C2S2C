// duplex.hpp - C++17 header-only wrapper over the Duplex C API (RAII, exceptions, lambdas).
// SPDX-License-Identifier: MIT
//
// In plain words: the C library hands out raw pointers that must be released in the right order
// and reports errors as negative numbers. This wrapper turns that into C++ habits:
//   - Context owns the library context and frees it automatically;
//   - Batch owns the packets of one poll and releases them when it goes out of scope, so you
//     cannot use a packet pointer after it was given back;
//   - errors are thrown as duplex::Error (carrying the DX_ERR_* code and its message);
//   - handlers and hooks are std::function (lambdas work).
#pragma once

#include <Duplex/duplex.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace duplex
{

/// An error from the library: code() is the DX_ERR_* value, what() its text.
class Error : public std::runtime_error
{
  public:
    explicit Error(int code) : std::runtime_error(dx_strerror(code)), code_(code) {}
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
    Writer(uint8_t* buf, uint32_t cap) { dx_writer_init(&w_, buf, cap); }
    Writer& u8(uint8_t v)
    {
        dx_w_u8(&w_, v);
        return *this;
    }
    Writer& u16(uint16_t v)
    {
        dx_w_u16(&w_, v);
        return *this;
    }
    Writer& u32(uint32_t v)
    {
        dx_w_u32(&w_, v);
        return *this;
    }
    Writer& u64(uint64_t v)
    {
        dx_w_u64(&w_, v);
        return *this;
    }
    Writer& f32(float v)
    {
        dx_w_f32(&w_, v);
        return *this;
    }
    Writer& f64(double v)
    {
        dx_w_f64(&w_, v);
        return *this;
    }
    Writer& string(std::string_view s)
    {
        dx_w_string(&w_, s.data(), static_cast<uint16_t>(s.size()));
        return *this;
    }
    Writer& bytes(const uint8_t* p, uint32_t n)
    {
        dx_w_bytes(&w_, p, n);
        return *this;
    }
    /// Number of bytes written; throws Error if any write failed.
    uint32_t size() const
    {
        check(w_.err);
        return w_.len;
    }

  private:
    dx_writer w_;
};

/// Reads payload fields; every call throws Error when the data is malformed or too short.
/// Views returned by string() and bytes() point into the packet and die with it.
class Reader
{
  public:
    Reader(const uint8_t* data, uint32_t len) { dx_reader_init(&r_, data, len); }
    uint8_t u8() { return ok(dx_r_u8(&r_)); }
    uint16_t u16() { return ok(dx_r_u16(&r_)); }
    uint32_t u32() { return ok(dx_r_u32(&r_)); }
    uint64_t u64() { return ok(dx_r_u64(&r_)); }
    float f32() { return ok(dx_r_f32(&r_)); }
    double f64() { return ok(dx_r_f64(&r_)); }
    std::string_view string()
    {
        uint16_t n = 0;
        const char* s = dx_r_string(&r_, &n);
        check(r_.err);
        return {s, n};
    }
    std::pair<const uint8_t*, uint32_t> bytes()
    {
        uint32_t n = 0;
        const uint8_t* p = dx_r_bytes(&r_, &n);
        check(r_.err);
        return {p, n};
    }

  private:
    template <class T> T ok(T v)
    {
        check(r_.err);
        return v;
    }
    dx_reader r_;
};

/// A received packet. Valid only while the Batch (or the handler call) that gave it is alive.
class Packet
{
  public:
    explicit Packet(const dx_packet& p) : p_(p) {}
    uint32_t conn() const { return p_.conn; }
    uint16_t id() const { return p_.packet_id; }
    uint32_t sequence() const { return p_.sequence; }
    const uint8_t* data() const { return p_.data; }
    uint32_t size() const { return p_.length; }
    Reader reader() const { return Reader(p_.data, p_.length); }
    /// Reason of a DX_SYS_DISCONNECTED event (0 = normal close, negative = DX_ERR_*).
    int reason() const { return static_cast<int32_t>(reader().u32()); }

  private:
    dx_packet p_;
};

/// The packets of one Context::poll call. Releases them to the library when destroyed.
class Batch
{
  public:
    Batch(dx_ctx* ctx, std::vector<dx_packet> packets) : ctx_(ctx), packets_(std::move(packets)) {}
    Batch(Batch&& o) noexcept : ctx_(o.ctx_), packets_(std::move(o.packets_)) { o.ctx_ = nullptr; }
    Batch& operator=(Batch&&) = delete;
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    ~Batch()
    {
        if (ctx_ && !packets_.empty()) dx_release(ctx_, static_cast<int>(packets_.size()));
    }
    size_t size() const { return packets_.size(); }
    Packet operator[](size_t i) const { return Packet(packets_[i]); }

  private:
    dx_ctx* ctx_;
    std::vector<dx_packet> packets_;
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

/// One network engine. Use it from one thread. Neither copyable nor movable (callbacks keep its
/// address); freed on destruction.
class Context
{
  public:
    explicit Context(const dx_config* cfg = nullptr) : ctx_(dx_create(cfg)), impl_(new Impl)
    {
        if (!ctx_) throw Error(DX_ERR_NO_MEMORY);
        impl_->self = ctx_.get();
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    void listen(const std::string& addr, uint16_t port = 0)
    {
        check(dx_listen(ctx_.get(), addr.c_str(), port));
    }
    int port() const { return check(dx_listen_port(ctx_.get())); }
    uint32_t connect(const std::string& addr, uint16_t port)
    {
        uint32_t conn = 0;
        check(dx_connect(ctx_.get(), addr.c_str(), port, &conn));
        return conn;
    }
    void close(uint32_t conn) { check(dx_close(ctx_.get(), conn)); }

    /// Sends a packet. Throws Error(DX_ERR_WOULD_BLOCK) if the send buffer is full; see try_send.
    void send(uint32_t conn, uint16_t id, const uint8_t* data, uint32_t len)
    {
        check(dx_send(ctx_.get(), conn, id, data, len));
    }
    /// Like send, but returns false instead of throwing when the send buffer is full.
    bool try_send(uint32_t conn, uint16_t id, const uint8_t* data, uint32_t len)
    {
        int rc = dx_send(ctx_.get(), conn, id, data, len);
        if (rc == DX_ERR_WOULD_BLOCK) return false;
        check(rc);
        return true;
    }

    /// Waits up to timeout_ms and returns up to max packets, released when the Batch dies.
    Batch poll(int timeout_ms, int max = 64)
    {
        std::vector<dx_packet> pk(static_cast<size_t>(max));
        int n = check(dx_poll(ctx_.get(), pk.data(), max, timeout_ms));
        pk.resize(static_cast<size_t>(n));
        return Batch(ctx_.get(), std::move(pk));
    }

    using Handler = std::function<void(Context&, const Packet&)>;
    /// Calls fn for packets with this id (0 = default handler); an empty function removes it.
    void on_packet(uint16_t id, Handler fn)
    {
        if (fn) {
            impl_->handlers[id] = std::move(fn);
            check(dx_on_packet(ctx_.get(), id, &Impl::handler, this));
        } else {
            impl_->handlers.erase(id);
            check(dx_on_packet(ctx_.get(), id, nullptr, nullptr));
        }
    }
    /// Installs hooks (replacing earlier ones).
    void set_hooks(Hooks hooks)
    {
        impl_->hooks = std::move(hooks);
        dx_hooks h{};
        h.size = sizeof h;
        h.user = impl_.get();
        h.on_connect = [](dx_ctx*, uint32_t c, void* u) {
            auto& f = static_cast<Impl*>(u)->hooks.on_connect;
            if (f) f(c);
        };
        h.on_disconnect = [](dx_ctx*, uint32_t c, int r, void* u) {
            auto& f = static_cast<Impl*>(u)->hooks.on_disconnect;
            if (f) f(c, r);
        };
        h.on_error = [](dx_ctx*, uint32_t c, int e, void* u) {
            auto& f = static_cast<Impl*>(u)->hooks.on_error;
            if (f) f(c, e);
        };
        h.before_send = [](dx_ctx*, uint32_t c, uint16_t id, const uint8_t* d, uint32_t n,
                           void* u) {
            auto& f = static_cast<Impl*>(u)->hooks.before_send;
            return f && f(c, id, d, n) ? 1 : 0;
        };
        h.after_recv = [](dx_ctx*, const dx_packet* p, void* u) {
            auto& f = static_cast<Impl*>(u)->hooks.after_recv;
            return f && f(Packet(*p)) ? 1 : 0;
        };
        check(dx_set_hooks(ctx_.get(), &h));
    }
    /// Like poll, but runs handlers and hooks, then releases. Returns the number of entries.
    int dispatch(int timeout_ms) { return check(dx_dispatch(ctx_.get(), timeout_ms)); }

    /// The underlying C context, for features the wrapper does not cover.
    dx_ctx* raw() { return ctx_.get(); }

  private:
    struct Deleter {
        void operator()(dx_ctx* c) const { dx_destroy(c); }
    };
    struct Impl {
        dx_ctx* self = nullptr;
        std::map<uint16_t, Handler> handlers;
        Hooks hooks;
        static void handler(dx_ctx*, const dx_packet* p, void* user)
        {
            auto* c = static_cast<Context*>(user);
            auto& map = c->impl_->handlers;
            auto it = map.find(p->packet_id);
            if (it == map.end()) it = map.find(0); // default handler
            if (it != map.end()) it->second(*c, Packet(*p));
        }
    };
    std::unique_ptr<dx_ctx, Deleter> ctx_;
    std::unique_ptr<Impl> impl_;
};

} // namespace duplex
