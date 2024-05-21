#include <sys/timerfd.h>
#include <string.h>
#include "rocket/net/timer.h"
#include "rocket/common/log.h"
#include "rocket/common/util.h"

namespace rocket {


Timer::Timer() : FdEvent() {

  // 当定时器到期时，timerfd 会变为可读，epoll_wait 将返回，指示 timerfd 上发生了可读事件。
  // 也就是你可以设置一个到期时间，然后时间到了就触发 EPOLLIN 事件，你就可以在这个时候去处理堆积的事件。
  // 而这些事件都是准备就绪的，所以就可以直接顺利一个个完成了。
  m_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); // TFD_CLOEXEC 表示关闭“执行后继承”，意思是比如我现在这
                                                                      // 个进程去启动另一个进程，不想让另一个进程去继承我的时钟。
                                                                      // 这个应该不用太了解，知道这样创建变量就行。
  DEBUGLOG("timer fd=%d", m_fd);

  // 把 fd 可读事件放到了 eventloop 上监听
  listen(FdEvent::IN_EVENT, std::bind(&Timer::onTimer, this));
}

Timer::~Timer() {
}


void Timer::onTimer() {
  // 处理缓冲区数据，防止下一次继续触发可读事件
  // 上面这句作者说的，如果是边沿触发模式就不用担心“下一次继续触发可读事件”这个问
  // 题，不过读走还是有必要的，毕竟这些数据都只是用来沟通用的，历史记录没有意义。
  // DEBUGLOG("ontimer");
  char buf[8];
  while(1) {
    if ((read(m_fd, buf, 8) == -1) && errno == EAGAIN) {
      break;
    }
  }

  // 执行定时任务
  int64_t now = getNowMs();

  std::vector<TimerEvent::s_ptr> tmps;
  std::vector<std::pair<int64_t, std::function<void()>>> tasks;

  ScopeMutex<Mutex> lock(m_mutex);
  auto it = m_pending_events.begin();

  for (it = m_pending_events.begin(); it != m_pending_events.end(); ++it) {
    if ((*it).first <= now) {
      if (!(*it).second->isCancled()) {
        tmps.push_back((*it).second);
        tasks.push_back(std::make_pair((*it).second->getArriveTime(), (*it).second->getCallBack()));
      }
    } else {
      break; // 后面的任务放下一批再执行，一批一批来。
    }
  }

  m_pending_events.erase(m_pending_events.begin(), it);
  lock.unlock();


  // 需要把循环的 Event 再次添加进去
  for (auto i = tmps.begin(); i != tmps.end(); ++i) {
    if ((*i)->isRepeated()) {
      // 调整 arriveTime
      (*i)->resetArriveTime();
      addTimerEvent(*i);
    }
  }

  resetArriveTime();

  for (auto i: tasks) {
    if (i.second) {
      i.second();
    }
  }

}

void Timer::resetArriveTime() {
  ScopeMutex<Mutex> lock(m_mutex);
  auto tmp = m_pending_events;
  lock.unlock();

  if (tmp.size() == 0) { // 如果任务队列没东西就直接先停一停了。
    return;
  }

  int64_t now = getNowMs();

  auto it = tmp.begin();
  int64_t inteval = 0;
  if (it->second->getArriveTime() > now) {
    inteval = it->second->getArriveTime() - now; // 这里只是直接这样设置，如果时间相差很短，我不确定边缘触发会不会出问题。不过好在作者用的是水平触发
  } else {
    inteval = 100; // 这个随便设置，不要太久就行。。100ms 应该还行。
  }

  timespec ts;
  memset(&ts, 0, sizeof(ts));
  ts.tv_sec = inteval / 1000;
  ts.tv_nsec = (inteval % 1000) * 1000000;

  itimerspec value;
  memset(&value, 0, sizeof(value));
  value.it_value = ts;

  int rt = timerfd_settime(m_fd, 0, &value, NULL); // 设置定时器的到期时间，“0”表示使用相对时间，也就是给计时器设置为此刻加上 value 的值。
                                                   // 如果是 TFD_TIMER_ABSTIME 就表示绝对时间。
  if (rt != 0) {
    ERRORLOG("timerfd_settime error, errno=%d, error=%s", errno, strerror(errno));
  }
  // DEBUGLOG("timer reset to %lld", now + inteval);

}

void Timer::addTimerEvent(TimerEvent::s_ptr event) {
  bool is_reset_timerfd = false;

  ScopeMutex<Mutex> lock(m_mutex);
  if (m_pending_events.empty()) {
    is_reset_timerfd = true; // 为空的时候已经是停下来了并且还没人给它个启动信号，现在重新加进去任务要重置一下时钟，才会执行任务。
  } else { // 而下面这种情况不用管，因为队列还不为空说明有在执行任务（OnTimer），而 OnTimer 里面是会自动调用 resetArriveTime 的
    auto it = m_pending_events.begin();
    if ((*it).second->getArriveTime() > event->getArriveTime()) {
      is_reset_timerfd = true;
    }
  }
  m_pending_events.emplace(event->getArriveTime(), event);
  lock.unlock();

  if (is_reset_timerfd) {
    resetArriveTime();
  }


}

void Timer::deleteTimerEvent(TimerEvent::s_ptr event) {
  event->setCancled(true);

  ScopeMutex<Mutex> lock(m_mutex);

  auto begin = m_pending_events.lower_bound(event->getArriveTime());
  auto end = m_pending_events.upper_bound(event->getArriveTime());

  auto it = begin;
  for (it = begin; it != end; ++it) {
    if (it->second == event) {
      break;
    }
  }

  if (it != end) {
    m_pending_events.erase(it);
  }
  lock.unlock();

  DEBUGLOG("success delete TimerEvent at arrive time %lld", event->getArriveTime());

}




  
}