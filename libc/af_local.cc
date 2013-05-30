#include "af_local.h"
#include "osv/file.h"
#include "osv/mutex.h"
#include "osv/types.h"
#include <deque>
#include <atomic>
#include <memory>
#include <boost/intrusive_ptr.hpp>
#include <sys/stat.h>
#include <sys/socket.h>
#include <osv/fcntl.h>
#include <osv/condvar.h>
#include "fs/fs.hh"
#include "fs/unsupported.h"
#include "libc.hh"
#include <osv/poll.h>

struct af_local_buffer {
    static constexpr size_t max_buf = 8192;
public:
    af_local_buffer() = default;
    af_local_buffer(const af_local_buffer&) = delete;
    int read(uio* data);
    int write(uio* data);
    int read_events();
    int write_events();
    void detach_sender();
    void detach_receiver();
    void attach_sender(struct file *f);
    void attach_receiver(struct file *f);
private:
    int read_events_unlocked();
    int write_events_unlocked();
private:
    mutex mtx;
    std::deque<char> q;
    struct file *receiver = nullptr;
    struct file *sender = nullptr;
    std::atomic<unsigned> refs = {};
    condvar may_read;
    condvar may_write;
    friend void intrusive_ptr_add_ref(af_local_buffer* p) {
        p->refs.fetch_add(1, std::memory_order_relaxed);
    }
    friend void intrusive_ptr_release(af_local_buffer* p) {
        if (p->refs.fetch_add(-1, std::memory_order_acquire) == 1) {
            delete p;
        }
    }
};

typedef boost::intrusive_ptr<af_local_buffer> af_local_buffer_ref;

void af_local_buffer::detach_sender()
{
    std::lock_guard<mutex> guard(mtx);
    if (sender) {
        sender = nullptr;
        if (receiver)
            poll_wake(receiver, POLLRDHUP);
        may_read.wake_all();
    }
}

void af_local_buffer::detach_receiver()
{
    std::lock_guard<mutex> guard(mtx);
    if (receiver) {
        receiver = nullptr;
        if (sender)
            poll_wake(sender, POLLHUP);
        may_write.wake_all();
    }
}

void af_local_buffer::attach_sender(struct file *f)
{
    assert(sender == nullptr);
    sender = f;
}

void af_local_buffer::attach_receiver(struct file *f)
{
    assert(receiver == nullptr);
    receiver = f;
}

int af_local_buffer::read_events_unlocked()
{
    int ret = 0;
    ret |= !q.empty() ? POLLIN : 0;
    ret |= !sender ? POLLRDHUP : 0;
    return ret;
}

int af_local_buffer::write_events_unlocked()
{
    if (!receiver) {
        return POLLHUP;
    }
    int ret = 0;
    ret |= q.size() < max_buf ? POLLOUT : 0;
    return ret;
}

int af_local_buffer::read_events()
{
    return with_lock(mtx, [=] { return read_events_unlocked(); });
}

int af_local_buffer::write_events()
{
    return with_lock(mtx, [=] { return write_events_unlocked(); });
}

int af_local_buffer::read(uio* data)
{
    if (!data->uio_resid) {
        return 0;
    }
    with_lock(mtx, [&] {
        int r;
        while ((r = read_events_unlocked()) == 0) {
            may_read.wait(&mtx);
        }
        if (!(r & POLLIN)) {
            assert(r & POLLRDHUP);
            return;
        }
        auto iov = data->uio_iov;
        while (data->uio_resid && (read_events_unlocked() & POLLIN)) {
            auto n = std::min(q.size(), iov->iov_len);
            char* p = static_cast<char*>(iov->iov_base);
            std::copy(q.begin(), q.begin() + n, p);
            q.erase(q.begin(), q.begin() + n);
            data->uio_resid -= n;
        }
        if (write_events_unlocked() && POLLOUT)
            poll_wake(sender, POLLOUT);
    });
    may_write.wake_all();
    return 0;
}

int af_local_buffer::write(uio* data)
{
    if (!data->uio_resid) {
        return 0;
    }
    int err = 0;
    with_lock(mtx, [&] {
        int r;
        while ((r = write_events_unlocked()) == 0) {
            may_write.wait(&mtx);
        }
        if (!(r & POLLOUT)) {
            assert(r & POLLHUP);
            err = EPIPE;
            return;
        }
        auto iov = data->uio_iov;
        while (data->uio_resid && (write_events_unlocked() & POLLOUT)) {
            auto n = std::min(max_buf - q.size(), iov->iov_len);
            char* p = static_cast<char*>(iov->iov_base);
            std::copy(p, p + n, std::back_inserter(q));
            data->uio_resid -= n;
        }
        if (read_events_unlocked() && POLLIN)
            poll_wake(receiver, POLLIN);
    });
    may_read.wake_all();
    return err;
}

struct af_local {
    af_local(af_local_buffer* s, af_local_buffer* r)
        : send(s), receive(r) {}
    ~af_local() {
        send->detach_sender();
        receive->detach_receiver();
    }
    af_local_buffer_ref send;
    af_local_buffer_ref receive;
};

int af_local_init(file* f)
{
    af_local* afl = static_cast<af_local*>(f->f_data);
    afl->send->attach_sender(f);
    afl->receive->attach_receiver(f);
    return 0;
}

int af_local_read(file* f, uio* data, int flags)
{
    af_local* afl = static_cast<af_local*>(f->f_data);
    return afl->receive->read(data);
}

int af_local_write(file* f, uio* data, int flags)
{
    af_local* afl = static_cast<af_local*>(f->f_data);
    return afl->send->write(data);
}

int af_local_poll(file* f, int events)
{
    af_local* afl = static_cast<af_local*>(f->f_data);
    int revents = 0;
    if (events & POLLIN) {
        revents |= afl->receive->read_events();
    }
    if (events & POLLOUT) {
        revents |= afl->send->write_events();
    }
    return revents;
}

int af_local_close(file* f)
{
    auto afl = static_cast<af_local*>(f->f_data);
    delete afl;
    f->f_data = nullptr;
    return 0;
}

fileops af_local_ops = {
    af_local_init,
    af_local_read,
    af_local_write,
    unsupported_truncate,
    unsupported_ioctl,
    af_local_poll,
    unsupported_stat,
    af_local_close,
    unsupported_chmod,
};

int socketpair_af_local(int type, int proto, int sv[2])
{
    assert(type == SOCK_STREAM);
    assert(proto == 0);
    auto b1 = new af_local_buffer;
    auto b2 = new af_local_buffer;
    std::unique_ptr<af_local> s1{new af_local(b1, b2)};
    std::unique_ptr<af_local> s2{new af_local(b2, b1)};
    try {
        fileref f1{falloc_noinstall()};
        fileref f2{falloc_noinstall()};
        finit(f1.get(), FREAD|FWRITE, DTYPE_UNSPEC, s1.release(), &af_local_ops);
        finit(f2.get(), FREAD|FWRITE, DTYPE_UNSPEC, s2.release(), &af_local_ops);
        fdesc fd1(f1);
        fdesc fd2(f2);
        // all went well, user owns descriptors now
        sv[0] = fd1.release();
        sv[1] = fd2.release();
        return 0;
    } catch (int error) {
        return libc_error(error);
    }
}

