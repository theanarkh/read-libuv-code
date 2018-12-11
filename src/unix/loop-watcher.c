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

#include "uv.h"
#include "internal.h"

#define UV_LOOP_WATCHER_DEFINE(name, type)                                    \
  int uv_##name##_init(uv_loop_t* loop, uv_##name##_t* handle) {              \

    // 初始化handle的类型，所属loop，打上UV_HANDLE_REF，并且把handle插入loop->handle_queue队列的队尾
    uv__handle_init(loop, (uv_handle_t*)handle, UV_##type);                   \
    handle->name##_cb = NULL;                                                 \
    return 0;                                                                 \
  }                                                                           \
                                                                              \
  int uv_##name##_start(uv_##name##_t* handle, uv_##name##_cb cb) {           \

   // 如果已经执行过start函数则直接返回
    if (uv__is_active(handle)) return 0;                                      \
    if (cb == NULL) return UV_EINVAL;                                         \

    // 把handle插入loop中相应类型的队列，loop有prepare，idle和check三个队列
    QUEUE_INSERT_HEAD(&handle->loop->name##_handles, &handle->queue);         \

    // 挂载回调，下一轮循环的时候被执行
    handle->name##_cb = cb;                                                   \

   // 设置UV_HANDLE_ACTIVE标记位，并且loop中的handle数加一，init的时候只是把handle挂载到loop，start的时候handle才处于激活态
    uv__handle_start(handle);                                                 \
    return 0;                                                                 \
  }                                                                           \
                                                                              \
  int uv_##name##_stop(uv_##name##_t* handle) {                               \
    if (!uv__is_active(handle)) return 0;                                     \

    // 把handle从loop中相应的队列移除，但是还挂载到handle_queue中
    QUEUE_REMOVE(&handle->queue);                                             \

   // 清除active标记位并且减去loop中handle的active数
    uv__handle_stop(handle);                                                  \
    return 0;                                                                 \
  }                                                                           \
                                                                              \

  // 在每一轮循环中执行该函数，执行时机见uv_run
  void uv__run_##name(uv_loop_t* loop) {                                      \
    uv_##name##_t* h;                                                         \
    QUEUE queue;                                                              \
    QUEUE* q;                                                                 \

    // 把该类型对应的队列中所有节点摘下来挂载到queue变量
    QUEUE_MOVE(&loop->name##_handles, &queue);                                \

   // 遍历队列，执行每个节点里面的函数
    while (!QUEUE_EMPTY(&queue)) {                                            \

      // 取下当前待处理的节点
      q = QUEUE_HEAD(&queue);                                                 \

      // 取得该节点对应的整个结构体的基地址
      h = QUEUE_DATA(q, uv_##name##_t, queue);                                \

      // 把该节点移出当前队列
      QUEUE_REMOVE(q);                                                        \

     // 重新插入原来的队列
      QUEUE_INSERT_TAIL(&loop->name##_handles, q);                            \

     // 执行回调函数
      h->name##_cb(h);                                                        \
    }                                                                         \
  }                                                                           \
                                                                              \
  void uv__##name##_close(uv_##name##_t* handle) {                            \
    uv_##name##_stop(handle);                                                 \
  }

UV_LOOP_WATCHER_DEFINE(prepare, PREPARE)
UV_LOOP_WATCHER_DEFINE(check, CHECK)
UV_LOOP_WATCHER_DEFINE(idle, IDLE)



// 利用宏定义，在预处理阶段拓展成三个不同类型，但是处理逻辑一样的代码。有三种类型，分别是prepare，check，idle。



