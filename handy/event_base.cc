#include "event_base.h"
#include <fcntl.h>
#include <string.h>
#include <map>
#include "conn.h"
#include "logging.h"
#include "poller.h"
#include "util.h"
using namespace std;

namespace handy {

namespace {

// refer to: https://www.xilidou.com/2018/03/22/redis-event/#%E6%97%B6%E9%97%B4%E4%BA%8B%E4%BB%B6
// processTimeEvent
//      Redis 使用这个函数处理所有的时间事件，我们整理一下执行思路：

//      记录最新一次执行这个函数的时间，用于处理系统时间被修改产生的问题。
//      遍历链表找出所有 when_sec 和 when_ms 小于现在时间的事件。
//              (when_sec和when_ms记录事件要执行的时间;)
//              (如果现在的时间大于它了，说明已经超时了，该事件就得执行;)
//              (如果现在时间小于它，说明还没到执行时间，就不执行)
//      执行事件对应的处理函数。
//      检查事件类型，如果是周期事件则刷新该事件下一次的执行事件。
//      否则从列表中删除事件。



// typedef std::pair<int64_t, int64_t>
// timerId.first = milli;               // 作为timer的执行时间
// timerId.second = ++timerSeq_         // 记录该timer是创建的第多少个timer
// std::map<TimerId, xxx> timer         // map 默认对根据key对键值对进行从小到大排序, timer.begin()->first 就是最小的 milli


// 周期性的timer 转化为 等价的无穷个单次的timer去执行
struct TimerRepeatable {
    // at 表示重复时间事件首次执行的时间
    int64_t at;  // current timer timeout timestamp
    int64_t interval;
    TimerId timerid; // 对应的单次timer中的timerid，单次timer执行后，就会更新为下一次的单次timer的timerid
    Task cb;
};

struct IdleNode {
    TcpConnPtr con_;
    int64_t updated_;
    TcpCallBack cb_;
};

}  // namespace

struct IdleIdImp {
    IdleIdImp() {}
    typedef list<IdleNode>::iterator Iter;
    IdleIdImp(list<IdleNode> *lst, Iter iter) : lst_(lst), iter_(iter) {}
    list<IdleNode> *lst_;
    Iter iter_;
};

struct EventsImp {
    EventBase *base_;
    PollerBase *poller_;
    std::atomic<bool> exit_;
    int wakeupFds_[2];
    SafeQueue<Task> tasks_;

    // 最早的单次时间事件过 nextTimeout_ 后执行
    int nextTimeout_;
    // 记录所有的周期执行的timer
    // 将周期执行的timer转为单次执行的timer
    // (每次执行的时候更新 TimerRepeatable::at 和 TimerRepeatable::timerId，然后生成新的单次timer存到timers_中)
    std::map<TimerId, TimerRepeatable> timerReps_;
    // 记录单次执行执行的timer
    std::map<TimerId, Task> timers_;
    // timerSeq_ 所有的创建的timerId的计数，包含所有的单次的和重复的
    std::atomic<int64_t> timerSeq_;

    // 记录每个idle时间（单位秒）下所有的连接。链表中的所有连接，最新的插入到链表末尾。连接若有活动，会把连接从链表中移到链表尾部，做法参考memcache
    std::map<int, std::list<IdleNode>> idleConns_;
    std::set<TcpConnPtr> reconnectConns_;
    bool idleEnabled;

    EventsImp(EventBase *base, int taskCap);
    ~EventsImp();
    void init();
    void callIdles();
    IdleId registerIdle(int idle, const TcpConnPtr &con, const TcpCallBack &cb);
    void unregisterIdle(const IdleId &id);
    void updateIdle(const IdleId &id);
    void handleTimeouts();
    void refreshNearest(const TimerId *tid = NULL);
    void repeatableTimeout(TimerRepeatable *tr);

    // eventbase functions
    EventBase &exit() {
        exit_ = true;
        wakeup();
        return *base_;
    }
    bool exited() { return exit_; }
    void safeCall(Task &&task) {
        tasks_.push(move(task));
        wakeup();
    }
    void loop();
    void loop_once(int waitMs) {
        // 处理文件事件
        poller_->loop_once(std::min(waitMs, nextTimeout_));
        // 处理时间事件
        handleTimeouts();
    }
    void wakeup() {
        int r = write(wakeupFds_[1], "", 1);
        fatalif(r <= 0, "write error wd %d %d %s", r, errno, strerror(errno));
    }

    bool cancel(TimerId timerid);
    TimerId runAt(int64_t milli, Task &&task, int64_t interval);
};

EventBase::EventBase(int taskCapacity) {
    imp_.reset(new EventsImp(this, taskCapacity));
    imp_->init();
}

EventBase::~EventBase() {}

EventBase &EventBase::exit() {
    return imp_->exit();
}

bool EventBase::exited() {
    return imp_->exited();
}

void EventBase::safeCall(Task &&task) {
    imp_->safeCall(move(task));
}

void EventBase::wakeup() {
    imp_->wakeup();
}

void EventBase::loop() {
    imp_->loop();
}

void EventBase::loop_once(int waitMs) {
    imp_->loop_once(waitMs);
}

bool EventBase::cancel(TimerId timerid) {
    return imp_ && imp_->cancel(timerid);
}

TimerId EventBase::runAt(int64_t milli, Task &&task, int64_t interval) {
    return imp_->runAt(milli, std::move(task), interval);
}

EventsImp::EventsImp(EventBase *base, int taskCap)
    : base_(base), poller_(createPoller()), exit_(false), nextTimeout_(1 << 30), tasks_(taskCap), timerSeq_(0), idleEnabled(false) {}

void EventsImp::loop() {
    while (!exit_)
        loop_once(10000);
    timerReps_.clear();
    timers_.clear();
    idleConns_.clear();
    for (auto recon : reconnectConns_) {  //重连的连接无法通过channel清理，因此单独清理
        recon->cleanup(recon);
    }
    loop_once(0);
}

void EventsImp::init() {
    int r = pipe(wakeupFds_);
    fatalif(r, "pipe failed %d %s", errno, strerror(errno));
    r = util::addFdFlag(wakeupFds_[0], FD_CLOEXEC);
    fatalif(r, "addFdFlag failed %d %s", errno, strerror(errno));
    r = util::addFdFlag(wakeupFds_[1], FD_CLOEXEC);
    fatalif(r, "addFdFlag failed %d %s", errno, strerror(errno));
    trace("wakeup pipe created %d %d", wakeupFds_[0], wakeupFds_[1]);
    Channel *ch = new Channel(base_, wakeupFds_[0], kReadEvent);
    ch->onRead([=] {
        char buf[1024];
        int r = ch->fd() >= 0 ? ::read(ch->fd(), buf, sizeof buf) : 0;
        if (r > 0) {
            Task task;
            while (tasks_.pop_wait(&task, 0)) {
                task();
            }
        } else if (r == 0) {
            delete ch;
        } else if (errno == EINTR) {
        } else {
            fatal("wakeup channel read error %d %d %s", r, errno, strerror(errno));
        }
    });
}

EventsImp::~EventsImp() {
    delete poller_;
    ::close(wakeupFds_[1]);
}

void EventsImp::callIdles() {
    int64_t now = util::timeMilli() / 1000;
    for (auto &l : idleConns_) {
        int idle = l.first;
        auto lst = l.second;
        while (lst.size()) {
            IdleNode &node = lst.front();
            if (node.updated_ + idle > now) {
                break;
            }
            node.updated_ = now;
            lst.splice(lst.end(), lst, lst.begin());
            node.cb_(node.con_);
        }
    }
}

IdleId EventsImp::registerIdle(int idle, const TcpConnPtr &con, const TcpCallBack &cb) {
    if (!idleEnabled) {
        base_->runAfter(1000, [this] { callIdles(); }, 1000);
        idleEnabled = true;
    }
    auto &lst = idleConns_[idle];
    lst.push_back(IdleNode{con, util::timeMilli() / 1000, move(cb)});
    trace("register idle");
    return IdleId(new IdleIdImp(&lst, --lst.end()));
}

void EventsImp::unregisterIdle(const IdleId &id) {
    trace("unregister idle");
    id->lst_->erase(id->iter_);
}

void EventsImp::updateIdle(const IdleId &id) {
    trace("update idle");
    id->iter_->updated_ = util::timeMilli() / 1000;
    id->lst_->splice(id->lst_->end(), *id->lst_, id->iter_);
}

// 处理单次时间事件(重复时间事件在添加时就被转化为了单次时间事件)
void EventsImp::handleTimeouts() {
    int64_t now = util::timeMilli();
    TimerId tid{now, 1L << 62};
    // timers_.begin()->first 作为 timer的执行时间，如果小于当前时间了，说明timer超时了，该执行了
    // 单次 timer 执行完(如果是重复执行timer对应的单次timer, 同时会生成新的单次timer)就清除
    while (timers_.size() && timers_.begin()->first < tid) {
        Task task = move(timers_.begin()->second);
        timers_.erase(timers_.begin());
        task();
    }
    refreshNearest();
}

// 更新 nextTimeout_
// 最近的单次时间事件在 nextTimeout_ 毫秒之后执行
// 每新增一个单次的 timerId 都需要更新 nextTimeout_
void EventsImp::refreshNearest(const TimerId *tid) {
    if (timers_.empty()) {
        nextTimeout_ = 1 << 30;
    } else {
        // (timers_.begin()->first).first 是最近马上要执行的timer的执行时间
        // 如果它比当前的时间晚的话，nextTimeout_ 就为两者时间差；否则的话，nextTimeout_ 就为 0
        const TimerId &t = timers_.begin()->first;
        nextTimeout_ = t.first - util::timeMilli();
        nextTimeout_ = nextTimeout_ < 0 ? 0 : nextTimeout_;
    }
}

// 更新重复timer信息，生成过interval时间后执行的单次timer，执行重复timer事件
void EventsImp::repeatableTimeout(TimerRepeatable *tr) {
    // 更新 TimerRepeatable::at 和 TimerRepeatable::timerid
    tr->at += tr->interval;
    tr->timerid = {tr->at, ++timerSeq_};
    // 重新生成一个单次的timer
    timers_[tr->timerid] = [this, tr] { repeatableTimeout(tr); };
    // 更新 nextTimeout_
    refreshNearest(&tr->timerid);
    // 执行 重复timer 的回调函数
    tr->cb();
}

// 添加时间事件
// 这里注意:
//      如果 milli 传的是一个比较小的数，即 milli < util::timeMilli()，
//      那么 nextTimeout_ 就会更新为 0，那么事件循环一启动，epoll_wait等待时间为0，
//      这个事件紧跟立刻就会被执行，如果该事件是一个周期时间事件，那么epoll_wait就会长时间等待时间为0，直到nextTimeout_ > 0
//      相对而言,实际使用中 runAfter() 更合理一些
TimerId EventsImp::runAt(int64_t milli, Task &&task, int64_t interval) {
    if (exit_) {
        return TimerId();
    }
    if (interval) {
        // interval 不为0，是一个重复执行事件
        // timerReqs_[{-milli, ++timerSeq_}] = {milli, interval, {milli, ++timerSeq_}, move(task)};
        // -milli : 将重复执行事件对应的timer的timerId.first指定为负数是因为，我们不会直接执行它，而是将它转化为单次执行的timer
        TimerId tid{-milli, ++timerSeq_};
        TimerRepeatable &rtr = timerReps_[tid];
        rtr = {milli, interval, {milli, ++timerSeq_}, move(task)};
        //
        TimerRepeatable *tr = &rtr;
        // timers_[{milli, ++timerSeq_}] = [this, tr] { repeatableTimeout(tr); };
        // 创建一个特殊的单次执行的 timer，该 timer 的回调函数是repeatableTimeout，该回调函数被调用时做4件事:
        //      更新重复timer的 TimerRepeatable::at 和 TimerRepeatable::timerid
        //      用更新后的 TimerRepeatable::timerid 重新创建一个新的单次timer，回调函数仍然指定为 repeatableTimeout
        //      调用 refreshNearest() 更新 nextTimeout_
        //      调用重复timer的回调函数
        // 这样就可以将一个 周期性的timer 转化为 等价的无穷个单次的timer
        timers_[tr->timerid] = [this, tr] { repeatableTimeout(tr); };
        refreshNearest(&tr->timerid);
        return tid;
    } else {
        // interval 为0，是单次执行事件
        TimerId tid{milli, ++timerSeq_};
        timers_.insert({tid, move(task)});
        refreshNearest(&tid);
        return tid;
    }
}

bool EventsImp::cancel(TimerId timerid) {
    if (timerid.first < 0) { // timerid.first < 0 表示是周期时间事件
        auto p = timerReps_.find(timerid);
        auto ptimer = timers_.find(p->second.timerid);
        if (ptimer != timers_.end()) {
            timers_.erase(ptimer);  // 清除对应的单次timer
        }
        timerReps_.erase(p);    // 清除重复timer
        return true;
    } else { // 单次时间事件
        auto p = timers_.find(timerid);
        if (p != timers_.end()) {
            timers_.erase(p);
            return true;
        }
        return false;
    }
}

void MultiBase::loop() {
    int sz = bases_.size();
    vector<thread> ths(sz - 1);
    for (int i = 0; i < sz - 1; i++) {
        thread t([this, i] { bases_[i].loop(); });
        ths[i].swap(t);
    }
    bases_.back().loop();
    for (int i = 0; i < sz - 1; i++) {
        ths[i].join();
    }
}

Channel::Channel(EventBase *base, int fd, int events) : base_(base), fd_(fd), events_(events) {
    fatalif(net::setNonBlock(fd_) < 0, "channel set non block failed");
    static atomic<int64_t> id(0);
    id_ = ++id;
    poller_ = base_->imp_->poller_;
    poller_->addChannel(this);
}

Channel::~Channel() {
    close();
}

void Channel::enableRead(bool enable) {
    if (enable) {
        events_ |= kReadEvent;
    } else {
        events_ &= ~kReadEvent;
    }
    poller_->updateChannel(this);
}

void Channel::enableWrite(bool enable) {
    if (enable) {
        events_ |= kWriteEvent;
    } else {
        events_ &= ~kWriteEvent;
    }
    poller_->updateChannel(this);
}

void Channel::enableReadWrite(bool readable, bool writable) {
    if (readable) {
        events_ |= kReadEvent;
    } else {
        events_ &= ~kReadEvent;
    }
    if (writable) {
        events_ |= kWriteEvent;
    } else {
        events_ &= ~kWriteEvent;
    }
    poller_->updateChannel(this);
}

void Channel::close() {
    if (fd_ >= 0) {
        trace("close channel %ld fd %d", (long) id_, fd_);
        poller_->removeChannel(this);
        ::close(fd_);
        fd_ = -1;
        handleRead();
    }
}

bool Channel::readEnabled() {
    return events_ & kReadEvent;
}
bool Channel::writeEnabled() {
    return events_ & kWriteEvent;
}

void handyUnregisterIdle(EventBase *base, const IdleId &idle) {
    base->imp_->unregisterIdle(idle);
}

void handyUpdateIdle(EventBase *base, const IdleId &idle) {
    base->imp_->updateIdle(idle);
}

TcpConn::TcpConn()
    : base_(NULL), channel_(NULL), state_(State::Invalid), destPort_(-1), connectTimeout_(0), reconnectInterval_(-1), connectedTime_(util::timeMilli()) {}

TcpConn::~TcpConn() {
    trace("tcp destroyed %s - %s", local_.toString().c_str(), peer_.toString().c_str());
    delete channel_;
}

void TcpConn::addIdleCB(int idle, const TcpCallBack &cb) {
    if (channel_) {
        idleIds_.push_back(getBase()->imp_->registerIdle(idle, shared_from_this(), cb));
    }
}

void TcpConn::reconnect() {
    auto con = shared_from_this();
    getBase()->imp_->reconnectConns_.insert(con);
    long long interval = reconnectInterval_ - (util::timeMilli() - connectedTime_);
    interval = interval > 0 ? interval : 0;
    info("reconnect interval: %d will reconnect after %lld ms", reconnectInterval_, interval);
    getBase()->runAfter(interval, [this, con]() {
        getBase()->imp_->reconnectConns_.erase(con);
        connect(getBase(), destHost_, (unsigned short) destPort_, connectTimeout_, localIp_);
    });
    delete channel_;
    channel_ = NULL;
}

}  // namespace handy