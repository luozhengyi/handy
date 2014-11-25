#include "conn.h"
#include "logging.h"
#include <sys/epoll.h>
#include <poll.h>
#include <fcntl.h>


using namespace std;
namespace handy {
int a;

void handyUnregisterIdle(EventBase* base, const IdleId& idle);
void handyUpdateIdle(EventBase* base, const IdleId& idle);

void TcpConn::attach(EventBase* base, int fd, Ip4Addr local, Ip4Addr peer)
{
    fatalif((!isClient_ && state_ != State::Invalid) || (isClient_ && state_ != State::Handshaking),
        "you should use a new TcpConn to attach. state: %d", state_);
    state_ = State::Handshaking;
    local_ = local;
    peer_ = peer;
    channel_ = new Channel(base, fd, EPOLLOUT|EPOLLIN);
    trace("tcp constructed %s - %s fd: %d",
        local_.toString().c_str(),
        peer_.toString().c_str(),
        fd);
    TcpConnPtr con = shared_from_this();
    con->channel_->onRead([=] { con->handleRead(con); });
    con->channel_->onWrite([=] { con->handleWrite(con); });
}

int TcpConn::connect(EventBase* base, const string& host, short port, int timeout) {
    fatalif(state_ != State::Invalid, "you should use a new TcpConn to connect. state: %d", state_);
    isClient_ = true;
    Ip4Addr addr(host, port);
    int fd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
    net::setNonBlock(fd);
    int r = ::connect(fd, (sockaddr*)&addr.getAddr(), sizeof (sockaddr_in));
    if (r != 0 && errno != EINPROGRESS) {
        error("connect to %s error %d %s", addr.toString().c_str(), errno, strerror(errno));
        ::close(fd);
        return errno;
    }

    sockaddr_in local;
    socklen_t alen = sizeof(local);
    r = getsockname(fd, (sockaddr*)&local, &alen);
    if (r < 0) {
        error("getsockname failed %d %s", errno, strerror(errno));
        ::close(fd);
        return errno;
    }
    state_ = State::Handshaking;
    attach(base, fd, Ip4Addr(local), addr);
    if (timeout) {
        TcpConnPtr con = shared_from_this();
        base->runAfter(timeout, [con] {
            if (con->getState() == Handshaking) { con->close(true); }
        });
    }
    return 0;
}

void TcpConn::close(bool cleanupNow) {
    if (channel_) {
        channel_->close();
        if (cleanupNow) {
            channel_->handleRead();
        } else {
            TcpConnPtr con = shared_from_this();
            getBase()->safeCall([con]{ if (con->channel_) con->channel_->handleRead(); });
        }
    }
}

void TcpConn::cleanup(const TcpConnPtr& con) {
    if (readcb_ && input_.size()) {
        readcb_(con);
    }
    if (state_ == State::Handshaking) {
        state_ = State::Failed;
    } else {
        state_ = State::Closed;
    }
    trace("tcp closing %s - %s fd %d %d",
        local_.toString().c_str(),
        peer_.toString().c_str(),
        channel_->fd(), errno);
    for (auto& idle: idleIds_) {
        handyUnregisterIdle(getBase(), idle);
    }
    if (statecb_) {
        statecb_(con);
    }
    //channel may have hold TcpConnPtr, set channel_ to NULL before delete
    Channel* ch = channel_;
    channel_ = NULL;
    readcb_ = writablecb_ = statecb_ = nullptr;
    delete ch;
}

void TcpConn::handleRead(const TcpConnPtr& con) {
    if (state_ == State::Handshaking && handleHandshake(con)) {
        return;
    }
    while(state_ == State::Connected) {
        input_.makeRoom();
        int rd = 0;
        if (channel_->fd() >= 0) {
            rd = readImp(channel_->fd(), input_.end(), input_.space());
            trace("channel %ld fd %d readed %d bytes", channel_->id(), channel_->fd(), rd);
        }
        if (rd == -1 && errno == EINTR) {
            continue;
        } else if (rd == -1 && (errno == EAGAIN || errno == EWOULDBLOCK) ) {
            for(auto& idle: idleIds_) {
                handyUpdateIdle(getBase(), idle);
            }
            if (readcb_ && input_.size()) {
                readcb_(con);
            }
            break;
        } else if (channel_->fd() == -1 || rd == 0 || rd == -1) {
            cleanup(con);
            break;
        } else { //rd > 0
            input_.addSize(rd);
        }
    }
}

int TcpConn::handleHandshake(const TcpConnPtr& con) {
    fatalif(state_ != Handshaking, "handleHandshaking called when state_=%d", state_);
    struct pollfd pfd;
    pfd.fd = channel_->fd();
    pfd.events = POLLOUT | POLLERR;
    int r = poll(&pfd, 1, 0);
    if (r == 1 && pfd.revents == POLLOUT) {
        channel_->enableReadWrite(true, false);
        state_ = State::Connected;
        if (state_ == State::Connected) {
            trace("tcp connected %s - %s fd %d",
                local_.toString().c_str(), peer_.toString().c_str(), channel_->fd());
            if (statecb_) {
                statecb_(con);
            }
        }
    } else {
        trace("poll fd %d return %d revents %d", channel_->fd(), r, pfd.revents);
        cleanup(con);
        return -1;
    }
    return 0;
}

void TcpConn::handleWrite(const TcpConnPtr& con) {
    if (state_ == State::Handshaking) {
        handleHandshake(con);
    } else if (state_ == State::Connected) {
        ssize_t sended = isend(output_.begin(), output_.size());
        output_.consume(sended);
        if (output_.empty() && writablecb_) {
            writablecb_(con);
        }
        if (output_.empty() && channel_->writeEnabled()) { // writablecb_ may write something
            channel_->enableWrite(false);
        }
    } else {
        error("handle write unexpected");
    }
}

ssize_t TcpConn::isend(const char* buf, size_t len) {
    size_t sended = 0;
    while (len > sended) {
        ssize_t wd = writeImp(channel_->fd(), buf + sended, len - sended);
        trace("channel %ld fd %d write %ld bytes", channel_->id(), channel_->fd(), wd);
        if (wd > 0) {
            sended += wd;
            continue;
        } else if (wd == -1 && errno == EINTR) {
            continue;
        } else if (wd == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (channel_->writeEnabled() == false) {
                channel_->enableWrite(true);
            }
            break;
        } else {
            error("write error: wd %ld %d %s", wd, errno, strerror(errno));
            break;
        }
    }
    return sended;
}

void TcpConn::send(Buffer& buf) {
    if (channel_) {
        if (channel_->writeEnabled()) { //just full
            output_.absorb(buf);
        } 
        if (buf.size()) {
            ssize_t sended = isend(buf.begin(), buf.size());
            buf.consume(sended);
        }
        if (buf.size()) {
            output_.absorb(buf);
            if (!channel_->writeEnabled()) {
                channel_->enableWrite(true);
            }
        }
    } else {
        warn("connection %s - %s closed, but still writing %lu bytes",
            local_.toString().c_str(), peer_.toString().c_str(), buf.size());
    }
}

void TcpConn::send(const char* buf, size_t len) {
    if (channel_) {
        if (output_.empty()) {
            ssize_t sended = isend(buf, len);
            buf += sended;
            len -= sended;
        }
        if (len) {
            output_.append(buf, len);
        }
    } else {
        warn("connection %s - %s closed, but still writing %lu bytes",
            local_.toString().c_str(), peer_.toString().c_str(), len);
    }
}

void TcpConn::onMsg(const MsgCallBack& cb) {
    onRead([cb](const TcpConnPtr& con) {
        Slice msg;
        int r = con->codec_->tryDecode(con->getInput(), msg);
        if (r < 0) {
            con->close(true);
        } else if (r > 0) {
            cb(con, msg);
            con->getInput().consume(r);
        }
    });
}

void TcpConn::sendMsg(Slice msg) {
    codec_->encode(msg, getOutput());
    sendOutput();
}

TcpServer::TcpServer(EventBases* bases, const string& host, short port):
base_(bases->allocBase()),
bases_(bases),
addr_(host, port), createcb_([]{ return TcpConnPtr(new TcpConn); })
{
    int fd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
    int r = net::setReuseAddr(fd);
    fatalif(r, "set socket reuse option failed");
    r = ::bind(fd,(struct sockaddr *)&addr_.getAddr(),sizeof(struct sockaddr));
    fatalif(r, "bind to %s failed %d %s", addr_.toString().c_str(), errno, strerror(errno));
    r = listen(fd, 20);
    fatalif(r, "listen failed %d %s", errno, strerror(errno));
    info("fd %d listening at %s", fd, addr_.toString().c_str());
    listen_channel_ = new Channel(base_, fd, EPOLLIN);
    listen_channel_->onRead([this]{ handleAccept(); });
}

void TcpServer::handleAccept() {
    struct sockaddr_in raddr;
    socklen_t rsz = sizeof(raddr);
    int cfd;
    while ((cfd = accept4(listen_channel_->fd(),(struct sockaddr *)&raddr,&rsz, SOCK_CLOEXEC))>=0) {
        sockaddr_in peer, local;
        socklen_t alen = sizeof(peer);
        int r = getpeername(cfd, (sockaddr*)&peer, &alen);
        if (r < 0) {
            error("get peer name failed %d %s", errno, strerror(errno));
            continue;
        }
        r = getsockname(cfd, (sockaddr*)&local, &alen);
        if (r < 0) {
            error("getsockname failed %d %s", errno, strerror(errno));
            continue;
        }
        EventBase* b = bases_->allocBase();
        auto addcon = [=] {
            TcpConnPtr con = createcb_();
            con->attach(b, cfd, local, peer);
            if (readcb_) {
                con->onRead(readcb_);
            }
        };
        if (b == base_) {
            addcon();
        } else {
            b->safeCall(move(addcon));
        }
    }
    if (errno != EAGAIN && errno != EINTR) {
        warn("accept return %d  %d %s", cfd, errno, strerror(errno));
    }
}

}