
#include <pthread.h>
#include "rocket/common/log.h"
#include "rocket/common/config.h"
#include "rocket/net/eventloop.h"
#include <unistd.h>

void* fun(void*) {
  
  int i = 20;
  while (i--) {
    DEBUGLOG("debug this is thread in %s", "fun");
    INFOLOG("info this is thread in %s", "fun");
  }

  return NULL;
}

void *CallLoop(void *arg) {
  auto poEventLoop = reinterpret_cast<rocket::EventLoop*>(arg);
  poEventLoop->loop();
  return nullptr;
}

// abcpony@ubuntu:~/QQMail/rocket/bin$ ./test_log
// 记得工作路径（pwd，也即 ~/QQMail/rocket/bin）不要搞错，因为配置文件里面打日志的路径是相对于 pwd 的。
int main() {

  rocket::Config::SetGlobalConfig("../conf/rocket.xml");

  rocket::Logger::InitGlobalLogger(1);

  pthread_t thread;
  pthread_create(&thread, NULL, &fun, NULL);

  int i = 20;
  while (i--) {
    DEBUGLOG("test debug log %s", "11");
    INFOLOG("test info log %s", "11");
  }

  // 别的用处不清楚，但是这里测试出来就真的要自己手动调用一下这个
  // poEventLoop->loop() 函数才行，也就是说确实是没有调用过的。。
  pthread_t pLoopThread;
  auto *poEventLoop = rocket::EventLoop::GetCurrentEventLoop();
  pthread_create(&pLoopThread, NULL, &CallLoop, poEventLoop);
  
  sleep(1);
  
  pthread_join(thread, NULL);
  return 0;
}