#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <string.h>
#include "rocket/net/eventloop.h"
#include "rocket/common/log.h"
#include "rocket/common/util.h"


#define ADD_TO_EPOLL() \
    auto it = m_listen_fds.find(event->getFd()); \
    int op = EPOLL_CTL_ADD; \
    if (it != m_listen_fds.end()) { \
      op = EPOLL_CTL_MOD; \
    } \
    epoll_event tmp = event->getEpollEvent(); \
    INFOLOG("epoll_event.events = %d", (int)tmp.events); \
    int rt = epoll_ctl(m_epoll_fd, op, event->getFd(), &tmp); \
    if (rt == -1) { \
      ERRORLOG("failed epoll_ctl when add fd, errno=%d, error=%s", errno, strerror(errno)); \
    } \
    m_listen_fds.insert(event->getFd()); \
    DEBUGLOG("add event success, fd[%d]", event->getFd()) \
// 上面的 epoll_ctl 就是给 m_epoll_fd 里面的 event->getFd() 进行操作 op，
// 而 &tmp 就是标识要操作的内容以及描述这个 fd 的信息。
// 比如 EPOLL_CTL_MOD 就是把原本的 event->getEpollEvent() 进行修改，然后
// 这里面的事件就是要给 event->getFd() 改成的样子。

#define DELETE_TO_EPOLL() \
    auto it = m_listen_fds.find(event->getFd()); \
    if (it == m_listen_fds.end()) { \
      return; \
    } \
    int op = EPOLL_CTL_DEL; \
    epoll_event tmp = event->getEpollEvent(); \
    int rt = epoll_ctl(m_epoll_fd, op, event->getFd(), NULL); \
    if (rt == -1) { \
      ERRORLOG("failed epoll_ctl when add fd, errno=%d, error=%s", errno, strerror(errno)); \
    } \
    m_listen_fds.erase(event->getFd()); \
    DEBUGLOG("delete event success, fd[%d]", event->getFd()); \

namespace rocket {

static thread_local EventLoop* t_current_eventloop = NULL; // thread_local 使得这个静态变量是线程局部的，也就
                                                           // 是每个线程用的是不同的变量，每个线程里面有个单例
static int g_epoll_max_timeout = 10000; // 单位 ms
static int g_epoll_max_events = 10;

EventLoop::EventLoop() {
  if (t_current_eventloop != NULL) {
    ERRORLOG("failed to create event loop, this thread has created event loop");
    exit(0); // 这个是终止整个进程的。
  }
  m_thread_id = getThreadId();

  m_epoll_fd = epoll_create(10); // 想起来了，传这个参数是没有实际作用的（已经废弃），但是要求传，并且是正整数，填个 1 也就行。

  if (m_epoll_fd == -1) {
    ERRORLOG("failed to create event loop, epoll_create error, error info[%d]", errno);
    exit(0);
  }

  initWakeUpFdEevent();
  initTimer();

  INFOLOG("succ create event loop in thread %d", m_thread_id);
  t_current_eventloop = this;
}

EventLoop::~EventLoop() {
  close(m_epoll_fd);
  if (m_wakeup_fd_event) {
    delete m_wakeup_fd_event;
    m_wakeup_fd_event = NULL;
  }
  if (m_timer) {
    delete m_timer;
    m_timer = NULL;
  }
}


void EventLoop::initTimer() {
  m_timer = new Timer();
  addEpollEvent(m_timer);
}

void EventLoop::addTimerEvent(TimerEvent::s_ptr event) {
  m_timer->addTimerEvent(event);
}

void EventLoop::initWakeUpFdEevent() {
  m_wakeup_fd = eventfd(0, EFD_NONBLOCK); // 第一个参数：eventfd 底层是一个计数器，它的值可以通过写入到
                                          // 该对象中来增加，可以通过从该对象中读取来减少。初始计数值指定
                                          // 了创建时计数器的初始值。在这种情况下，初始计数值为 0 意味着
                                          // 创建的 eventfd 对象的初始计数器值为 0。
                                          // 第二个参数：设置为非阻塞。
                                          // eventfd 是用于线程同步或进程间通信的。
  if (m_wakeup_fd < 0) {
    ERRORLOG("failed to create event loop, eventfd create error, error info[%d]", errno);
    exit(0);
  }
  INFOLOG("wakeup fd = %d", m_wakeup_fd);

  m_wakeup_fd_event = new WakeUpFdEvent(m_wakeup_fd);

  m_wakeup_fd_event->listen(FdEvent::IN_EVENT, [this]() {
    char buf[8];
    // while(read(m_wakeup_fd, buf, 8) != -1 && errno != EAGAIN) {// 这个条件本来是 && 的，我改成 || 了，这个看不下去了，必须改的。
    while(read(m_wakeup_fd, buf, 8) != -1 || errno != EAGAIN) { // 把文件描述符的缓冲区清空
    }
    DEBUGLOG("read full bytes from wakeup fd[%d]", m_wakeup_fd);
  });

  addEpollEvent(m_wakeup_fd_event);

}


void EventLoop::loop() { // 就说日志是怎么弄的，这个 loop 被谁调用过了？
  printf("begin EventLoop::loop()\n");
  m_is_looping = true;
  while(!m_stop_flag) {
    ScopeMutex<Mutex> lock(m_mutex); 
    std::queue<std::function<void()>> tmp_tasks; 
    m_pending_tasks.swap(tmp_tasks); 
    lock.unlock();

    while (!tmp_tasks.empty()) {
      std::function<void()> cb = tmp_tasks.front();
      tmp_tasks.pop();
      if (cb) {
        cb();
      }
    }

    // 如果有定时任务需要执行，那么执行
    // 1. 怎么判断一个定时任务需要执行？ （now() > TimerEvent.arrtive_time）
    // 2. arrtive_time 如何让 eventloop 监听

    int timeout = g_epoll_max_timeout; 
    epoll_event result_events[g_epoll_max_events];
    // DEBUGLOG("now begin to epoll_wait");
    int rt = epoll_wait(m_epoll_fd, result_events, g_epoll_max_events, timeout); // timeout 单位 ms
    // DEBUGLOG("now end epoll_wait, rt = %d", rt);

    if (rt < 0) {
      ERRORLOG("epoll_wait error, errno=%d, error=%s", errno, strerror(errno));
    } else {
      for (int i = 0; i < rt; ++i) {
        epoll_event trigger_event = result_events[i];
        FdEvent* fd_event = static_cast<FdEvent*>(trigger_event.data.ptr);
        if (fd_event == NULL) {
          ERRORLOG("fd_event = NULL, continue");
          continue;
        }

        // int event = (int)(trigger_event.events); 
        // DEBUGLOG("unkonow event = %d", event);

        if (trigger_event.events & EPOLLIN) { 

          // DEBUGLOG("fd %d trigger EPOLLIN event", fd_event->getFd())
          addTask(fd_event->handler(FdEvent::IN_EVENT));
        }
        if (trigger_event.events & EPOLLOUT) { 
          // DEBUGLOG("fd %d trigger EPOLLOUT event", fd_event->getFd())
          addTask(fd_event->handler(FdEvent::OUT_EVENT));
        }

        // EPOLLHUP EPOLLERR
        if (trigger_event.events & EPOLLERR) {
          DEBUGLOG("fd %d trigger EPOLLERROR event", fd_event->getFd())
          // 删除出错的套接字
          deleteEpollEvent(fd_event);
          if (fd_event->handler(FdEvent::ERROR_EVENT) != nullptr) {
            DEBUGLOG("fd %d add error callback", fd_event->getFd())
            addTask(fd_event->handler(FdEvent::OUT_EVENT));
          }
        }
      }
    }
    
  }

}

void EventLoop::wakeup() {
  INFOLOG("WAKE UP");
  m_wakeup_fd_event->wakeup();
}

void EventLoop::stop() {
  m_stop_flag = true;
  wakeup();
}

void EventLoop::dealWakeup() {

}

void EventLoop::addEpollEvent(FdEvent* event) {
  // 因为 EventLoop 不止用在线程级单例中，其他地方也会创建，所以主 Reactor 在给
  // 子 Reactor 添加事件的时候如果子 Reactor 自己也在添加，就冲突了，所以就要求
  // 只有在子 Reactor 中才真正添加到 epoll 中，而其他人来添加的时候就是添加为一
  // 个 task。
  // 而添加 task 是不会引起 epoll 的反应的，它只是添加在任务队列中，上面那个处理
  // epoll 事件的 loop 也是通过在队列中添加回调函数，等一波 epoll_wait 之后就把
  // 堆积的任务完成了。也因此如果 epoll 等待过程中没人唤醒一下，它最多就得等 10s
  // 才能执行任务，这样响应就太慢了，所以就有了 wake_fd 这个东西，专门敲下 epoll。
  if (isInLoopThread()) {
    ADD_TO_EPOLL();
  } else {
    auto cb = [this, event]() { // 这个 this 是有必要的，因为那些 m_ 变量其实本质上是把
                                // this-> 隐藏了，所以匿名函数需要传个 this 给里面去用
      ADD_TO_EPOLL();
    };
    addTask(cb, true);
  }

}

void EventLoop::deleteEpollEvent(FdEvent* event) {
  if (isInLoopThread()) {
    DELETE_TO_EPOLL();
  } else {

    auto cb = [this, event]() {
      DELETE_TO_EPOLL();
    };
    addTask(cb, true);
  }

}

void EventLoop::addTask(std::function<void()> cb, bool is_wake_up /*=false*/) {
  ScopeMutex<Mutex> lock(m_mutex);
  m_pending_tasks.push(cb);
  lock.unlock();

  if (is_wake_up) {
    wakeup();
  }
}

bool EventLoop::isInLoopThread() {
  return getThreadId() == m_thread_id;
}

// 这是一个线程内单例的实现，由于线程内只有一个任务序列，所以不加锁也没事。如果说不
// 止一个任务序列，可以按照之前学的那样，在一个保证还只有一个任务序列的情况下就先调
// 用一下，保证已经初始化，比如线程刚开始的时候。
EventLoop* EventLoop::GetCurrentEventLoop() {
  if (t_current_eventloop) {
    return t_current_eventloop;
  }
  t_current_eventloop = new EventLoop();
  return t_current_eventloop;
}


bool EventLoop::isLooping() {
  return m_is_looping;
}

}
