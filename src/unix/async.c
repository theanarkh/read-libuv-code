/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* This file contains both the uv__async internal infrastructure and the
 * user-facing uv_async_t functions.
 */

#include "uv.h"
#include "internal.h"
#include "atomic-ops.h"

#include <errno.h>
#include <stdio.h>  /* snprintf() */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void uv__async_send(uv_loop_t* loop);
static int uv__async_start(uv_loop_t* loop);
static int uv__async_eventfd(void);


int uv_async_init(uv_loop_t* loop, uv_async_t* handle, uv_async_cb async_cb) {
  int err;
  // 给libuv注册一个用于异步通信的io观察者
  err = uv__async_start(loop);
  if (err)
    return err;
  // 设置相关字段，给libuv插入一个async_handle
  uv__handle_init(loop, (uv_handle_t*)handle, UV_ASYNC);
  handle->async_cb = async_cb;
  // 标记是否有任务完成了
  handle->pending = 0;
  // 插入async队列，poll io阶段判断是否有任务与完成
  QUEUE_INSERT_TAIL(&loop->async_handles, &handle->queue);
  // 激活handle为active状态
  uv__handle_start(handle);

  return 0;
}

// 通知主线程有任务完成
int uv_async_send(uv_async_t* handle) {
  /* Do a cheap read first. */
  if (ACCESS_ONCE(int, handle->pending) != 0)
    return 0;
  // 如pending是0，则设置为1，返回0，如果是1则返回1，所以同一个async如果多次调用该函数是会被合并的
  if (cmpxchgi(&handle->pending, 0, 1) == 0)
    uv__async_send(handle->loop);

  return 0;
}

// 关闭async
void uv__async_close(uv_async_t* handle) {
  // 移出aysnc队列
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
}

// 有异步通知的时候执行的回调
static void uv__async_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  char buf[1024];
  ssize_t r;
  QUEUE queue;
  QUEUE* q;
  uv_async_t* h;
  // 用于异步通信的io观察者
  assert(w == &loop->async_io_watcher);

  for (;;) {
    // 判断通信内容
    r = read(w->fd, buf, sizeof(buf));
    // 如果数据大于buf的长度，接着读，清空这一轮写入的数据
    if (r == sizeof(buf))
      continue;
    // 不等于-1，说明读成功，失败的时候返回-1，errno是错误码
    if (r != -1)
      break;

    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;
    // 被信号中断，继续读
    if (errno == EINTR)
      continue;
    // 出错，发送abort信号
    abort();
  }
  // 把async_handles队列里的所有节点都移到queue变量中
  QUEUE_MOVE(&loop->async_handles, &queue);
  while (!QUEUE_EMPTY(&queue)) {
    // 逐个取出节点
    q = QUEUE_HEAD(&queue);
    // 根据结构体字段获取结构体首地址
    h = QUEUE_DATA(q, uv_async_t, queue);
    // 从队列中移除该节点
    QUEUE_REMOVE(q);
    // 重新插入async_handles队列，等待下次事件
    QUEUE_INSERT_TAIL(&loop->async_handles, q);
    /*
      将第一个参数和第二个参数进行比较，如果相等，
      则将第三参数写入第一个参数，返回第二个参数的值，
      如果不相等，则返回第一个参数的值。
    */
    //判断哪些async被触发了。pending在uv_async_send里设置成1，如果pending等于1，则清0，返回1.如果pending等于0，则返回0
    if (cmpxchgi(&h->pending, 1, 0) == 0)
      continue;

    if (h->async_cb == NULL)
      continue;
    // 执行上层回调
    h->async_cb(h);
  }
}


static void uv__async_send(uv_loop_t* loop) {
  const void* buf;
  ssize_t len;
  int fd;
  int r;

  buf = "";
  len = 1;
  // 用于异步通信的管道的写端
  fd = loop->async_wfd;

#if defined(__linux__)
  // 说明用的是eventfd而不是管道
  if (fd == -1) {
    static const uint64_t val = 1;
    buf = &val;
    len = sizeof(val);
    // 见uv__async_start
    fd = loop->async_io_watcher.fd;  /* eventfd */
  }
#endif
  // 通知读端
  do
    r = write(fd, buf, len);
  while (r == -1 && errno == EINTR);

  if (r == len)
    return;

  if (r == -1)
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;

  abort();
}

/*
  初始化异步通信的io观察者
  给loop注册一个用于主线程和线程池线程通信的io观察者
*/
static int uv__async_start(uv_loop_t* loop) {
  int pipefd[2];
  int err;
  // 用于主线程和子线程通信的fd，管道的读端，主线程使用，判断loop->async_io_watcher.fd而不是async_wfd的值，因为在使用eventfd通信的时候，async_wfd是-1
  if (loop->async_io_watcher.fd != -1)
    return 0;
  // 获取一个用于进程间通信的fd
  err = uv__async_eventfd();
  // 成功则保存起来，不支持则使用管道通信作为进程间通信
  if (err >= 0) {
    pipefd[0] = err;
    pipefd[1] = -1;
  }
  else if (err == UV_ENOSYS) {
    err = uv__make_pipe(pipefd, UV__F_NONBLOCK);
#if defined(__linux__)
    /* Save a file descriptor by opening one of the pipe descriptors as
     * read/write through the procfs.  That file descriptor can then
     * function as both ends of the pipe.
     */
    if (err == 0) {
      char buf[32];
      int fd;

      snprintf(buf, sizeof(buf), "/proc/self/fd/%d", pipefd[0]);
      // 通过fd就可以实现对管道的读写
      fd = uv__open_cloexec(buf, O_RDWR);
      if (fd >= 0) {
        // 关掉旧的
        uv__close(pipefd[0]);
        uv__close(pipefd[1]);
        // 赋值新的
        pipefd[0] = fd;
        pipefd[1] = fd;
      }
    }
#endif
  }

  if (err < 0)
    return err;
  // 初始化io观察者async_io_watcher
  uv__io_init(&loop->async_io_watcher, uv__async_io, pipefd[0]);
  // 注册io观察者到loop里，并注册需要监听的事件POLLIN，读
  uv__io_start(loop, &loop->async_io_watcher, POLLIN);
  // 用于主线程和子线程通信的fd，管道的写端，子线程使用
  loop->async_wfd = pipefd[1];

  return 0;
}


int uv__async_fork(uv_loop_t* loop) {
  // 没有注册过async节点，fd在第一次注册的时候赋值的
  if (loop->async_io_watcher.fd == -1) /* never started */
    return 0;
  // 关闭异步通信的管道，把用于异步通信的io观察者移出队列
  uv__async_stop(loop);
  // 重新申请管道（或eventfd）用于主线程和子线程通信，注册io观察者到队列
  return uv__async_start(loop);
}


void uv__async_stop(uv_loop_t* loop) {
  if (loop->async_io_watcher.fd == -1)
    return;
  // 在使用eventfd通信时是-1
  if (loop->async_wfd != -1) {
    // 这两一般情况下并不相等
    if (loop->async_wfd != loop->async_io_watcher.fd)
      uv__close(loop->async_wfd);
    loop->async_wfd = -1;
  }
  // 注销io观察者
  uv__io_stop(loop, &loop->async_io_watcher, POLLIN);
  // 关闭和重置文件描述符
  uv__close(loop->async_io_watcher.fd);
  loop->async_io_watcher.fd = -1;
}


static int uv__async_eventfd(void) {
#if defined(__linux__)
  static int no_eventfd2;
  static int no_eventfd;
  int fd;

  if (no_eventfd2)
    goto skip_eventfd2;

  fd = uv__eventfd2(0, UV__EFD_CLOEXEC | UV__EFD_NONBLOCK);
  if (fd != -1)
    return fd;

  if (errno != ENOSYS)
    return UV__ERR(errno);

  no_eventfd2 = 1;

skip_eventfd2:

  if (no_eventfd)
    goto skip_eventfd;

  fd = uv__eventfd(0);
  if (fd != -1) {
    uv__cloexec(fd, 1);
    uv__nonblock(fd, 1);
    return fd;
  }

  if (errno != ENOSYS)
    return UV__ERR(errno);

  no_eventfd = 1;

skip_eventfd:

#endif

  return UV_ENOSYS;
}
