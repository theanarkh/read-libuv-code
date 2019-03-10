/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
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

#include "uv-common.h"

#if !defined(_WIN32)
# include "unix/internal.h"
#endif

#include <stdlib.h>

#define MAX_THREADPOOL_SIZE 128

static uv_once_t once = UV_ONCE_INIT;
static uv_cond_t cond;
static uv_mutex_t mutex;
static unsigned int idle_threads;
static unsigned int slow_io_work_running;
static unsigned int nthreads;
static uv_thread_t* threads;
static uv_thread_t default_threads[4];
static QUEUE exit_message;
static QUEUE wq;
static QUEUE run_slow_work_message;
static QUEUE slow_io_pending_wq;

static unsigned int slow_work_thread_threshold(void) {
  return (nthreads + 1) / 2;
}

static void uv__cancelled(struct uv__work* w) {
  abort();
}


/* To avoid deadlock with uv_cancel() it's crucial that the worker
 * never holds the global mutex and the loop-local mutex at the same time.
 */
static void worker(void* arg) {
  struct uv__work* w;
  QUEUE* q;
  int is_slow_work;

  uv_sem_post((uv_sem_t*) arg);
  arg = NULL;

  uv_mutex_lock(&mutex);
  for (;;) {
    /* `mutex` should always be locked at this point. */

    /* Keep waiting while either no work is present or only slow I/O
       and we're at the threshold for that. */
    /*
      如果队列为空，或者不为空，但是队列里只有慢IO任务且正在执行的慢IO任务个数达到阈值，
      则空闲函数加一，等待费慢IO任务，防止慢IO占用资源线程，导致其他快的任务无法得到执行
    */
    while (QUEUE_EMPTY(&wq) ||
           (QUEUE_HEAD(&wq) == &run_slow_work_message &&
            QUEUE_NEXT(&run_slow_work_message) == &wq &&
            slow_io_work_running >= slow_work_thread_threshold())) {
      idle_threads += 1;
      // 阻塞，等待唤醒
      uv_cond_wait(&cond, &mutex);
      idle_threads -= 1;
    }
    // 取出头结点，头指点可能是退出消息、慢IO，一般请求
    q = QUEUE_HEAD(&wq);
    // 如果头结点是退出消息，则结束线程
    if (q == &exit_message) {
      uv_cond_signal(&cond);
      uv_mutex_unlock(&mutex);
      break;
    }
    // 移除节点 
    QUEUE_REMOVE(q);
    QUEUE_INIT(q);  /* Signal uv_cancel() that the work req is executing. */

    is_slow_work = 0;
    // 如果当前节点等于慢IO节点
    if (q == &run_slow_work_message) {
      /* If we're at the slow I/O threshold, re-schedule until after all
         other work in the queue is done. */
      // 遇到阈值，重新入队 
      if (slow_io_work_running >= slow_work_thread_threshold()) {
        QUEUE_INSERT_TAIL(&wq, q);
        continue;
      }

      /* If we encountered a request to run slow I/O work but there is none
         to run, that means it's cancelled => Start over. */
      // 没有慢IO任务则继续
      if (QUEUE_EMPTY(&slow_io_pending_wq))
        continue;
      // 处理慢IO任务
      is_slow_work = 1;
      // 正在处理慢IO任务的个数累加，用于其他线程判断慢IO任务个数是否达到阈值
      slow_io_work_running++;
      // 取出任务
      q = QUEUE_HEAD(&slow_io_pending_wq);
      QUEUE_REMOVE(q);
      QUEUE_INIT(q);

      /* If there is more slow I/O work, schedule it to be run as well. */
      // 取出一个任务后，如果还有满IO任务则把慢IO标记节点重新入队，表示还有满IO任务，因为上面把该标记节点出队了 
      if (!QUEUE_EMPTY(&slow_io_pending_wq)) {
        QUEUE_INSERT_TAIL(&wq, &run_slow_work_message);
        // 有空闲线程则唤醒他，因为还有任务处理
        if (idle_threads > 0)
          uv_cond_signal(&cond);
      }
    }

    uv_mutex_unlock(&mutex);
    // q是慢IO或者一般任务
    w = QUEUE_DATA(q, struct uv__work, wq);
    // 执行业务回调，该函数一般会阻塞
    w->work(w);

    uv_mutex_lock(&w->loop->wq_mutex);
    w->work = NULL;  /* Signal uv_cancel() that the work req is done
                        executing. */
    // 执行完任务,插入到loop的wq队列,在uv__work_done的时候会执行该队列的节点
    QUEUE_INSERT_TAIL(&w->loop->wq, &w->wq);
    // 通知loop的wq_async节点
    uv_async_send(&w->loop->wq_async);
    uv_mutex_unlock(&w->loop->wq_mutex);

    /* Lock `mutex` since that is expected at the start of the next
     * iteration. */
    uv_mutex_lock(&mutex);
    // 执行完满IO任务，记录正在执行的慢IO个数变量减1
    if (is_slow_work) {
      /* `slow_io_work_running` is protected by `mutex`. */
      slow_io_work_running--;
    }
  }
}

// 把任务插入队列等待线程处理
static void post(QUEUE* q, enum uv__work_kind kind) {
  uv_mutex_lock(&mutex);
  // 类型是慢IO
  if (kind == UV__WORK_SLOW_IO) {
    /* Insert into a separate queue. */
    // 插入慢IO对应的队列
    QUEUE_INSERT_TAIL(&slow_io_pending_wq, q);
    /*
      有慢IO任务的时候，需要给主队列wq插入一个消息节点run_slow_work_message,
      说明有慢IO任务，所以如果run_slow_work_message是空，说明还没有插入主队列。
      需要进行q = &run_slow_work_message;赋值，然后把run_slow_work_message插入
      主队列 
    */
    if (!QUEUE_EMPTY(&run_slow_work_message)) {
      /* Running slow I/O tasks is already scheduled => Nothing to do here.
         The worker that runs said other task will schedule this one as well. */
      uv_mutex_unlock(&mutex);
      return;
    }
    q = &run_slow_work_message;
  }
  // 把节点插入主队列，可能是慢IO消息节点或者一般任务
  QUEUE_INSERT_TAIL(&wq, q);
  // 有空闲线程则唤醒他
  if (idle_threads > 0)
    uv_cond_signal(&cond);
  uv_mutex_unlock(&mutex);
}


#ifndef _WIN32
UV_DESTRUCTOR(static void cleanup(void)) {
  unsigned int i;

  if (nthreads == 0)
    return;

  post(&exit_message, UV__WORK_CPU);

  for (i = 0; i < nthreads; i++)
    if (uv_thread_join(threads + i))
      abort();

  if (threads != default_threads)
    uv__free(threads);

  uv_mutex_destroy(&mutex);
  uv_cond_destroy(&cond);

  threads = NULL;
  nthreads = 0;
}
#endif


static void init_threads(void) {
  unsigned int i;
  const char* val;
  uv_sem_t sem;

  nthreads = ARRAY_SIZE(default_threads);
  val = getenv("UV_THREADPOOL_SIZE");
  if (val != NULL)
    nthreads = atoi(val);
  if (nthreads == 0)
    nthreads = 1;
  if (nthreads > MAX_THREADPOOL_SIZE)
    nthreads = MAX_THREADPOOL_SIZE;

  threads = default_threads;
  if (nthreads > ARRAY_SIZE(default_threads)) {
    threads = uv__malloc(nthreads * sizeof(threads[0]));
    if (threads == NULL) {
      nthreads = ARRAY_SIZE(default_threads);
      threads = default_threads;
    }
  }
  // 初始化条件变量
  if (uv_cond_init(&cond))
    abort();
  // 初始化互斥变量
  if (uv_mutex_init(&mutex))
    abort();

  // 初始化三个队列
  QUEUE_INIT(&wq);
  QUEUE_INIT(&slow_io_pending_wq);
  QUEUE_INIT(&run_slow_work_message);

  // 初始化信号量变量，值为0
  if (uv_sem_init(&sem, 0))
    abort();
  // 创建多个线程
  for (i = 0; i < nthreads; i++)
    if (uv_thread_create(threads + i, worker, &sem))
      abort();
  // 等待sem信号量为非0的时候减去一，为0则阻塞
  for (i = 0; i < nthreads; i++)
    uv_sem_wait(&sem);

  uv_sem_destroy(&sem);
}


#ifndef _WIN32
static void reset_once(void) {
  uv_once_t child_once = UV_ONCE_INIT;
  memcpy(&once, &child_once, sizeof(child_once));
}
#endif


static void init_once(void) {
#ifndef _WIN32
  /* Re-initialize the threadpool after fork.
   * Note that this discards the global mutex and condition as well
   * as the work queue.
   */
  if (pthread_atfork(NULL, NULL, &reset_once))
    abort();
#endif
  init_threads();
}

// 给线程池提交一个任务
void uv__work_submit(uv_loop_t* loop,
                     struct uv__work* w,
                     enum uv__work_kind kind,
                     void (*work)(struct uv__work* w),
                     void (*done)(struct uv__work* w, int status)) {
  // 保证已经初始化线程，并只执行一次
  uv_once(&once, init_once);
  w->loop = loop;
  w->work = work;
  w->done = done;
  post(&w->wq, kind);
}


static int uv__work_cancel(uv_loop_t* loop, uv_req_t* req, struct uv__work* w) {
  int cancelled;
  // 加锁，为了把节点移出队列
  uv_mutex_lock(&mutex);
  // 加锁，为了判断w->wq是否为空
  uv_mutex_lock(&w->loop->wq_mutex);
  // w在一个队列中并work不为空，则可取消
  cancelled = !QUEUE_EMPTY(&w->wq) && w->work != NULL;
  // 删除该节点
  if (cancelled)
    QUEUE_REMOVE(&w->wq);

  uv_mutex_unlock(&w->loop->wq_mutex);
  uv_mutex_unlock(&mutex);

  if (!cancelled)
    return UV_EBUSY;
  // 重置回调函数
  w->work = uv__cancelled;
 
  uv_mutex_lock(&loop->wq_mutex);
   // 插入loop的wq队列
  QUEUE_INSERT_TAIL(&loop->wq, &w->wq);
  // 通知主线程执行任务回调
  uv_async_send(&loop->wq_async);
  uv_mutex_unlock(&loop->wq_mutex);

  return 0;
}


void uv__work_done(uv_async_t* handle) {
  struct uv__work* w;
  uv_loop_t* loop;
  QUEUE* q;
  QUEUE wq;
  int err;

  loop = container_of(handle, uv_loop_t, wq_async);

  uv_mutex_lock(&loop->wq_mutex);
  // 把loop->wq队列的节点全部移到wp变量中，wq的队列在线程处理函数work里进行设置
  QUEUE_MOVE(&loop->wq, &wq);
  uv_mutex_unlock(&loop->wq_mutex);

  while (!QUEUE_EMPTY(&wq)) {
    q = QUEUE_HEAD(&wq);
    QUEUE_REMOVE(q);

    w = container_of(q, struct uv__work, wq);
    err = (w->work == uv__cancelled) ? UV_ECANCELED : 0;
    // 执行回调
    w->done(w, err);
  }
}


static void uv__queue_work(struct uv__work* w) {
  uv_work_t* req = container_of(w, uv_work_t, work_req);

  req->work_cb(req);
}


static void uv__queue_done(struct uv__work* w, int err) {
  uv_work_t* req;

  req = container_of(w, uv_work_t, work_req);
  uv__req_unregister(req->loop, req);

  if (req->after_work_cb == NULL)
    return;

  req->after_work_cb(req, err);
}


int uv_queue_work(uv_loop_t* loop,
                  uv_work_t* req,
                  uv_work_cb work_cb,
                  uv_after_work_cb after_work_cb) {
  if (work_cb == NULL)
    return UV_EINVAL;

  uv__req_init(loop, req, UV_WORK);
  req->loop = loop;
  req->work_cb = work_cb;
  req->after_work_cb = after_work_cb;
  uv__work_submit(loop,
                  &req->work_req,
                  UV__WORK_CPU,
                  uv__queue_work,
                  uv__queue_done);
  return 0;
}


int uv_cancel(uv_req_t* req) {
  struct uv__work* wreq;
  uv_loop_t* loop;

  switch (req->type) {
  case UV_FS:
    loop =  ((uv_fs_t*) req)->loop;
    wreq = &((uv_fs_t*) req)->work_req;
    break;
  case UV_GETADDRINFO:
    loop =  ((uv_getaddrinfo_t*) req)->loop;
    wreq = &((uv_getaddrinfo_t*) req)->work_req;
    break;
  case UV_GETNAMEINFO:
    loop = ((uv_getnameinfo_t*) req)->loop;
    wreq = &((uv_getnameinfo_t*) req)->work_req;
    break;
  case UV_WORK:
    loop =  ((uv_work_t*) req)->loop;
    wreq = &((uv_work_t*) req)->work_req;
    break;
  default:
    return UV_EINVAL;
  }

  return uv__work_cancel(loop, req, wreq);
}
