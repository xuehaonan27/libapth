# libapth I/O 钩子函数 & Event Manager 全面审查报告

## 一、`apth_syscall.c` I/O 钩子函数分析

### 1. 已实现且设计合理的部分

**`read`/`write`/`readv`/`writev`/`recvfrom`/`sendto`/`send`/`recv`**：
- ✅ 统一采用 "强制 NONBLOCK + event-based retry loop" 模式，设计正确
- ✅ 写操作（write/writev/sendto）正确实现了完整写语义（短写后继续循环）
- ✅ 读操作允许短读直接返回（符合 POSIX）
- ✅ EAGAIN/EWOULDBLOCK 都做了检测（跨平台兼容）
- ✅ 使用 `apth_shield` 保护 errno 恢复 fdmode
- ✅ `send`/`recv` 正确委托给 `sendto`/`recvfrom`

**`connect`**：
- ✅ 非阻塞 connect + EINPROGRESS 等待 + getsockopt(SO_ERROR) 检测，逻辑正确

**`accept`**：
- ✅ 非阻塞 accept + EAGAIN 重试，逻辑正确

**`nanosleep`/`usleep`/`sleep`**：
- ✅ 转换为 apth_event_time + apth_wait_event，正确让出 CPU

**`select` (hooked)**:
- ✅ 先做 zero-timeout 快速探测，命中则直接返回
- ✅ 未命中则创建 ev_select + ev_timeout 事件组，挂入等待队列
- ✅ 纯延迟场景（nfd==0）做了特殊处理

### 2. 发现的 Bug 和问题

#### Bug 1: `select` 钩子中 nfd==0 纯延迟路径的死代码
```c
// 当 nfd == 0 && rfds == NULL && wfds == NULL && efds == NULL 进入此分支后：
if (rfds != NULL) FD_ZERO(rfds);  // ← rfds 必为 NULL，永远不执行
if (wfds != NULL) FD_ZERO(wfds);  // ← 同上
if (efds != NULL) FD_ZERO(efds);  // ← 同上
```
虽无害但说明逻辑冗余，建议删除。

#### Bug 2: `select` 钩子中 `selected` 变量定义为 `false` 但用于 `int` 赋值
```c
selected = false;  // 声明为 int，但 false 只是 0，后续未对 selected 做更多有意义使用
```
`selected` 变量被赋值但从未真正被用来决定返回值，`rc` 才是关键。`selected` 是残留逻辑。

#### Bug 3: `accept` 未正确处理 EINTR
```c
while ((rv = apth_syscall_raw(accept)(fd, addr, addrlen)) == -1 
       && (errno == EAGAIN || errno == EWOULDBLOCK) ...)
```
缺少对 `EINTR` 的处理。应为：
```c
while ((rv = ...) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ...)
```

#### Bug 4: 每次 I/O 调用都执行 `apth_fdmode` 导致 fcntl 系统调用风暴
每个 `read`/`write` 等都做 2 次 `apth_fdmode`（设置+恢复），而 `apth_fdmode` 内部调用 `fcntl(F_GETFL)` + `fcntl(F_SETFL)`。在高频 I/O 场景下，一次 `read` 会产生多达 4 次 fcntl 系统调用，严重违背"尽可能避免系统调用"的设计目标。

**建议**：维护一个 per-fd 的 shadow 状态表（如 `struct apth_fd_info { int original_mode; int refcount; }`），在 fd 首次被 libapth 管理时设为 NONBLOCK 并记录原始模式，引用计数归零时恢复。这样可以将 fcntl 调用从 O(n_io_ops) 降到 O(n_fds)。

#### Bug 5: 快速路径中使用 `select` 做单 fd 可读/可写探测效率低
```c
FD_ZERO(&fds); FD_SET(fd, &fds);
apth_syscall_raw(select)(fd + 1, &fds, NULL, NULL, &delay);
```
对单个 fd 来说，`select` 需要初始化整个 `fd_set`（128 字节）+ 一次系统调用。更好的方式：
- 使用 `poll(&(struct pollfd){fd, POLLIN, 0}, 1, 0)` — 一次系统调用，无需初始化大结构
- 或直接尝试非阻塞 I/O（因为已经设为 NONBLOCK），省掉探测系统调用

**建议**：既然 fd 已经是 NONBLOCK，可以直接尝试 I/O 操作。如果返回 EAGAIN，再走事件等待路径。这样在数据已就绪时只需 1 次系统调用而非 2 次。

#### Bug 6: `writev` 中 `sizeof(tiov_stack)` 比较有误
```c
struct iovec tiov_stack[32];
if (iovcnt > (int)sizeof(tiov_stack))  // sizeof(tiov_stack) = 32 * sizeof(struct iovec) = 512
```
应该是 `iovcnt > 32` 或 `iovcnt > (int)(sizeof(tiov_stack)/sizeof(tiov_stack[0]))`，否则实际上只有当 `iovcnt > 512` 时才会走 malloc 路径，而 `UIO_MAXIOV` 通常是 1024，意味着 32-512 之间会访问 tiov_stack 越界。

#### Bug 7: 未实现的关键钩子 — `socket`, `close`, `poll`, `setsockopt`
这些函数都是 `TODO("unimplemented ...")`，会直接 panic。这意味着：
- 用户无法创建 socket（`socket`）
- 用户无法关闭 fd（`close`）— 非常严重，内存泄漏和 fd 泄漏
- `poll` 未实现，很多现代应用依赖 poll

**实现建议**：
- **`socket`**：直接调用 raw syscall 即可，可选择性地设为 NONBLOCK
- **`close`**：直接调用 raw syscall，如果有 fd shadow 表则清理对应条目
- **`setsockopt`**：直接透传给 raw syscall
- **`poll`**：将 pollfd 数组转换为事件等待，类似 select 的处理

### 3. 架构级改进建议

#### 建议 A: 消除 "先探测再 I/O" 的双系统调用模式
当前模式：`select(fd, 0-timeout)` → 如果就绪 → `read(fd)` — 两次系统调用。
改进后：直接 `read(fd)`（fd 已是 NONBLOCK）→ 如果 EAGAIN → 走事件等待。
在数据已就绪时，系统调用数从 2 降到 1。

#### 建议 B: 引入 per-fd 状态管理
```c
struct apth_fd_entry {
    int orig_flags;     // 原始 fcntl flags
    int managed;        // 是否被 libapth 管理
    int refcount;       // 并发使用计数
};
static struct apth_fd_entry apth_fd_table[FD_SETSIZE];
```
好处：
1. 避免 fcntl 风暴
2. 为未来 epoll 整合提供基础设施
3. `close` 钩子可以清理状态

---

## 二、`apth_event.c` Event Manager 分析

### 1. 当前设计的核心问题：使用 `select` 做事件轮询

**问题 1: `select` 的 O(n) 扫描**
`select` 需要线性扫描 0 到 `fdmax` 的每一个 bit。当 fd 编号较大（如 fd=999）但实际只关注 2-3 个 fd 时，内核仍然扫描所有 1000 个位。

**问题 2: `FD_SETSIZE` 限制**
`select` 受 `FD_SETSIZE`（通常 1024）限制。超过此值的 fd 无法监控，且 `FD_SET` 会产生未定义行为（缓冲区溢出）。

**问题 3: 每次调用都要重建 fd_set**
每次 event manager 循环都要：
1. `FD_ZERO` 三个 fd_set
2. 遍历所有等待 apth 的所有事件，组装 fd_set
3. 调用 `select`
4. 再遍历所有等待 apth 检查结果

这是 O(waiting_apths × events_per_apth × fdmax) 的复杂度。

**问题 4: 两遍遍历 (first_loop + second_loop)**
event manager 对 waiting queue 做两遍完整遍历，第一遍组装 fd_set + 检查非I/O事件，第二遍检查 I/O 结果。持有 waiting_queue 锁的时间非常长。

### 2. 建议：使用 epoll 替代 select

#### 设计方案

```c
// 每个 scheduler 维护一个 epoll 实例
struct apth_perpthr_scheduler {
    // ... existing fields ...
    int epoll_fd;  // epoll instance for this scheduler
};
```

**核心改造点**：

**A. 在 scheduler init 时创建 epoll fd**
```c
sched->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
```

**B. 当 apth 进入 waiting 状态且有 FD 事件时，注册到 epoll**
```c
// 在 apth 被推入 waiting_queue 时
struct epoll_event ee;
ee.events = 0;
if (ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE) ee.events |= EPOLLIN;
if (ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE) ee.events |= EPOLLOUT;
if (ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION) ee.events |= EPOLLPRI;
ee.data.ptr = th;  // 直接关联到 apth
epoll_ctl(sched->epoll_fd, EPOLL_CTL_ADD, fd, &ee);
```

**C. event manager 改为 epoll_wait**
```c
void apth_sched_eventmanager_epoll(apth_sched_t sched, apth_time_t *now, bool dopoll) {
    // 1. 先处理非 I/O 事件（timer, signal, tid, func）— 单遍遍历
    //    这些事件数量通常远少于 I/O 事件
    
    // 2. 计算 epoll timeout
    int timeout_ms = dopoll ? 0 : compute_next_timer_ms(sched, now);
    
    // 3. epoll_wait — O(1) 就绪事件返回
    struct epoll_event events[64];
    int nready = epoll_wait(sched->epoll_fd, events, 64, timeout_ms);
    
    // 4. 只处理就绪的 fd 对应的 apth — O(nready)，不是 O(total_waiting)
    for (int i = 0; i < nready; i++) {
        apth_t th = (apth_t)events[i].data.ptr;
        // 标记事件为 OCCURRED，移到 waked_queue
        mark_fd_events_occurred(th, events[i].events);
        transfer_th(th, sched->waiting_queue, sched->waked_queue);
        epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    }
}
```

**优势**：
| 维度 | select (当前) | epoll (改进后) |
|------|--------------|---------------|
| 复杂度 | O(fdmax × n_waiting) | O(n_ready) |
| fd 上限 | 1024 (FD_SETSIZE) | 数十万 |
| 每轮开销 | 重建 fd_set + 两遍遍历 | epoll_wait 一次调用 |
| 系统调用 | select 每轮 1 次 | epoll_wait 每轮 1 次，但 fd 增删是增量的 |
| 锁持有时间 | 两遍完整遍历 waiting_queue | 只处理就绪的 apth |

**D. SELECT 类型事件的兼容方案**
对于 `APTH_EVENT_TYPE_SELECT` 类型的事件（用户调用了 hooked `select`），可以将其中的 fd_set 拆解为多个 epoll 监控项，或者作为 fallback 仍使用 `apth_syscall_raw(select)` 单独检查这一个事件的 fd_set。

### 3. Event Manager 中的其他问题

#### 问题 1: `__first_loop` 中 `APTH_EVENT_TYPE_FD` 不做即时判断
FD 事件在 first_loop 中只是组装 fd_set，不做判断。这意味着即使 fd 已经就绪，也要等到 `select` 返回后的 second_loop 才能发现。如果改用 epoll，这个问题自然解决。

#### 问题 2: `__second_loop` 中 `rc < 0` 时的 re-check 逻辑
```c
else if (aux->rc < 0) {
    // re-check particular filedescriptor
    int rc2;
    ...
    while ((rc2 = apth_syscall_raw(select)(...)) < 0 && errno == EINTR);
```
当全局 select 出错时，为每个 fd 事件单独再做一次 select。如果有 N 个 fd 事件，就是 N 次额外的系统调用。使用 epoll 后此问题不存在。

#### 问题 3: Timer 精度受 I/O 等待影响
当 `dopoll=false` 且存在 timer 时，event manager 会用 timer 作为 select 的 timeout 等待。但如果 I/O 在 timer 之前就绪，timer 检查会延迟到下一轮。使用 epoll + timerfd 可以统一处理。

#### 问题 4: 信号事件检查在两遍中重复
`APTH_EVENT_TYPE_SIGS` 在 first_loop 和 second_loop 中都检查 `sigpending`，代码几乎完全重复。可以只在一遍中处理。

#### 问题 5: 内存分配
```c
ev = (apth_event_t)malloc(sizeof(struct apth_event_st));
```
每次创建事件都 malloc，每次 free。高频 I/O 场景下这是一个性能瓶颈。建议引入 per-scheduler 的事件对象池（slab allocator）。

---

## 三、总结：推荐的改进优先级

| 优先级 | 改进项 | 影响 |
|--------|--------|------|
| **P0 (Bug)** | 修复 `writev` 中 `sizeof(tiov_stack)` 的比较错误 | 内存越界 |
| **P0 (Bug)** | 实现 `close`, `socket`, `setsockopt` | 基本功能缺失 |
| **P0 (Bug)** | `accept` 添加 EINTR 处理 | 可能在信号频繁时卡住 |
| **P1 (性能)** | I/O 钩子去掉"先 select 探测"，直接尝试非阻塞 I/O | 每次 I/O 减少 1 次系统调用 |
| **P1 (性能)** | 引入 per-fd 状态表，避免 fcntl 风暴 | 每次 I/O 减少 2-4 次系统调用 |
| **P1 (性能)** | Event manager 从 select 迁移到 epoll | 从 O(fdmax×n) 到 O(n_ready) |
| **P2 (性能)** | 事件对象池（避免 malloc/free） | 减少堆分配开销 |
| **P2 (功能)** | 实现 `poll` 钩子 | 现代应用兼容性 |
| **P3 (清理)** | 清理 `select` 钩子中的死代码和 `selected` 残留变量 | 代码质量 |

# 四、详细改进执行指导

以下按优先级分组，每个改进项给出：__目标、涉及文件、具体代码变更步骤、注意事项__。

---

## Phase 0: 修复现有 Bug（P0）

### 0.1 修复 `writev` 中 `sizeof(tiov_stack)` 的比较错误

__目标__：防止 iovcnt 在 33~512 之间时 tiov_stack 数组越界。

__涉及文件__：`src/internal/apth_syscall.c`

__当前代码（已在你的最新版本中修复了第一处，但 cleanup 处还有一处遗漏）__：

我看到你已经修复了分配判断：

```c
if (iovcnt > (int)(sizeof(tiov_stack)/sizeof(tiov_stack[0])))
```

但在函数末尾的 cleanup 部分仍然是旧代码：

```c
// Cleanup
if (iovcnt > (int)sizeof(tiov_stack))  // ← 这里也需要修复！
    free(tiov);
```

__修改__：将 cleanup 处也改为：

```c
if (iovcnt > (int)(sizeof(tiov_stack)/sizeof(tiov_stack[0])))
    free(tiov);
```

__建议__：为避免重复，定义一个常量：

```c
#define APTH_WRITEV_TIOV_STACK_SIZE (sizeof(tiov_stack)/sizeof(tiov_stack[0]))
```

---

### 0.2 实现 `socket` 钩子

__目标__：让用户程序能正常创建 socket。

__涉及文件__：`src/internal/apth_syscall.c`

__实现__：

```c
APTH_DEFINE_SYSCALL(int, socket,
                    (int domain, int type, int protocol),
                    (domain, type, protocol))
{
    apth_hook_debug(socket);

    // 直接调用 libc 的 socket
    int fd = apth_syscall_raw(socket)(domain, type, protocol);
    if (fd < 0)
        return fd;

    // 可选：在 APTH_FD_TABLE 中注册此 fd
    if (fd >= 0 && fd < FD_SETSIZE)
    {
        APTH_FD_TABLE[fd].orig_flags = fcntl(fd, F_GETFL, 0);
        APTH_FD_TABLE[fd].managed = 1;
        APTH_FD_TABLE[fd].refcount = 0;
    }

    return fd;
}
```

__注意__：

- 不要在 socket 创建时就设为 NONBLOCK，因为用户可能期望 blocking 语义。NONBLOCK 由各 I/O 钩子在需要时临时设置。
- 如果未来引入 per-fd 管理（Phase 1），此处是注册 fd 的关键入口。

---

### 0.3 实现 `close` 钩子

__目标__：让用户程序能正常关闭 fd，并清理内部状态。

__涉及文件__：`src/internal/apth_syscall.c`

__实现__：

```c
APTH_DEFINE_SYSCALL(int, close, (int fd), (fd))
{
    apth_hook_debug(close);

    // 清理 APTH_FD_TABLE 中的条目
    if (fd >= 0 && fd < FD_SETSIZE)
    {
        APTH_FD_TABLE[fd].orig_flags = 0;
        APTH_FD_TABLE[fd].managed = 0;
        APTH_FD_TABLE[fd].refcount = 0;
    }

    // 未来如果使用 epoll：需要在此处将 fd 从所有 scheduler 的 epoll 实例中移除
    // epoll 在 fd close 时会自动移除，但如果 fd 被 dup 过则不会。
    // 安全起见可以显式 EPOLL_CTL_DEL（忽略错误）。

    // 调用 libc 的 close
    return apth_syscall_raw(close)(fd);
}
```

__注意__：

- `close` 是非常高频的操作，实现必须轻量。
- 如果有其他 apth 正在等待这个 fd 的 I/O 事件，`close` 后 event manager 中 `select`/`epoll` 会对该 fd 报错，second_loop 中的 `APTH_EV_STATUS_FAILED` 路径会处理它。

---

### 0.4 实现 `setsockopt` 钩子

__目标__：直接透传。

__涉及文件__：`src/internal/apth_syscall.c`

__实现__：

```c
APTH_DEFINE_SYSCALL(
    int, setsockopt,
    (int fd, int level, int option_name, const void *option_value, socklen_t option_len),
    (fd, level, option_name, option_value, option_len))
{
    apth_hook_debug(setsockopt);
    return apth_syscall_raw(setsockopt)(fd, level, option_name, option_value, option_len);
}
```

---

### 0.5 `select` 钩子中的 `selected` 变量残留修复

__目标__：确认你的最新版本中 `selected` 被注释掉后的逻辑是否正确。

我看到你的最新代码中有这样的结构：

```c
if (ev_select->ev_status == APTH_EV_STATUS_OCCURRED)
    // selected = true;
    if (timeout != NULL && ev_timeout->ev_status == APTH_EV_STATUS_OCCURRED)
    {
```

__⚠️ 这里有一个严重的逻辑 bug！__ 注释掉 `selected = true;` 后，`if (ev_select->ev_status == APTH_EV_STATUS_OCCURRED)` 变成了下面那个 `if` 的条件前缀，即：

```c
if (ev_select == OCCURRED)
    if (timeout != NULL && ev_timeout == OCCURRED)
```

这意味着只有当 __select 事件和 timeout 事件都发&#x751F;__&#x65F6;才会进入清零分支。但原意是：如果 timeout 发生了（而 select 没发生），应该清零 fd_set 并返回 0。

__修复__：应该用大括号或者重新组织逻辑：

```c
if (ev_select->ev_status == APTH_EV_STATUS_FAILED)
    return apth_error(-1, EBADF);

// 如果 select 事件发生了，rc 已经在 ev_args.SELECT.n 中被设置
// 如果 timeout 发生但 select 未发生，返回 0 并清零 fd_set
if (timeout != NULL && ev_timeout->ev_status == APTH_EV_STATUS_OCCURRED
    && ev_select->ev_status != APTH_EV_STATUS_OCCURRED)
{
    if (rfds != NULL) FD_ZERO(rfds);
    if (wfds != NULL) FD_ZERO(wfds);
    if (efds != NULL) FD_ZERO(efds);
    rc = 0;
}

return rc;
```

---

## Phase 1: 性能优化（P1）

### 1.1 消除 "先探测再 I/O" 的双系统调用模式

__目标__：在 I/O 钩子函数中，去掉 `select` 快速探测，直接尝试非阻塞 I/O。

__涉及文件__：`src/internal/apth_syscall.c` 中的 `read`、`write`、`readv`、`writev`、`recvfrom`、`sendto`

__改造模板（以 `read` 为例）__：

当前代码：

```c
// 1. apth_fdmode(fd, NONBLOCK)         — 2 次 fcntl
// 2. select(fd, 0-timeout)             — 1 次系统调用（快速探测）
// 3. if ready: read(fd)                — 1 次系统调用
//    else: event_wait → read(fd)
// 4. apth_fdmode(fd, fdmode)           — 2 次 fcntl
// 总计最佳情况：6 次系统调用
```

改造后：

```c
APTH_DEFINE_SYSCALL(ssize_t, read,
                    (int fd, void *buf, size_t nbytes), (fd, buf, nbytes))
{
    if (nbytes == 0) return 0;
    if (!apth_util_fd_valid(fd)) return apth_error(-1, EBADF);

    int fdmode;
    if ((fdmode = apth_fdmode(fd, APTH_FDMODE_NONBLOCK)) == APTH_FDMODE_ERROR)
        return apth_error(-1, EBADF);

    ssize_t rv;
    for (;;)
    {
        // 直接尝试非阻塞 read — 如果数据已就绪，一次系统调用搞定
        while ((rv = apth_syscall_raw(read)(fd, buf, nbytes)) < 0 && errno == EINTR)
            ;

        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // 数据未就绪，让出 CPU 等待事件
            apth_event_t ev = apth_event_fd(
                APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, fd);
            apth_wait_event(ev);
            continue;  // 被唤醒后重试
        }

        // rv >= 0 (成功/EOF) 或 rv < 0 (真正的错误)
        break;
    }

    apth_shield { apth_fdmode(fd, fdmode); }
    return rv;
}
// 总计最佳情况：3 次系统调用（fdmode_set + read + fdmode_restore）
// 如果配合 Phase 1.2 的 per-fd 表：1 次系统调用（仅 read）
```

__对所有 I/O 钩子重复此模板__：

- `write`：循环中处理短写（`buf += s; nbytes -= s;`），EAGAIN 时 wait writable
- `readv`：同 read，不需处理短读
- `writev`：同 write，需要 iov_advance
- `recvfrom`：同 read
- `sendto`：同 write

---

### 1.2 引入 per-fd 状态管理，消除 fcntl 风暴

__目标__：将每次 I/O 操作的 4 次 fcntl 降到 0 次（均摊）。

__涉及文件__：

- `src/internal_types.h`（已有 `struct apth_fd_entry`）
- `src/internal/apth_fd.c`（已有 `APTH_FD_TABLE`）
- `src/internal_funcs.h`（添加新函数声明）
- `src/utils/fd_utils.c`（修改 `apth_fdmode`）
- `src/internal/apth_syscall.c`（修改所有 I/O 钩子）

#### 步骤 1：扩展 `struct apth_fd_entry`

在 `src/internal_types.h` 中（已有定义），确认字段：

```c
struct apth_fd_entry
{
    int orig_flags;         // 原始 fcntl flags（创建/打开时的）
    _Atomic(int) managed;   // 是否被 libapth 管理（改为 atomic 更安全）
    _Atomic(int) refcount;  // 当前有多少 apth 正在对此 fd 做 I/O
};
extern struct apth_fd_entry APTH_FD_TABLE[FD_SETSIZE];
```

#### 步骤 2：添加 fd 管理函数

在 `src/internal_funcs.h` 中声明：

```c
APTH_INTERNAL void apth_fd_table_init(void);
APTH_INTERNAL int  apth_fd_acquire(int fd);   // 增加 refcount，首次时设 NONBLOCK
APTH_INTERNAL void apth_fd_release(int fd);   // 减少 refcount，归零时恢复原始 flags
APTH_INTERNAL void apth_fd_register(int fd);  // socket/open 时注册
APTH_INTERNAL void apth_fd_unregister(int fd); // close 时注销
```

在 `src/internal/apth_fd.c` 中实现：

```c
#include "internal_types.h"
#include "internal_funcs.h"
#include <fcntl.h>

struct apth_fd_entry APTH_FD_TABLE[FD_SETSIZE];

APTH_INTERNAL void apth_fd_table_init(void)
{
    memset(APTH_FD_TABLE, 0, sizeof(APTH_FD_TABLE));
}

APTH_INTERNAL void apth_fd_register(int fd)
{
    if (fd < 0 || fd >= FD_SETSIZE) return;
    int flags = fcntl(fd, F_GETFL, 0);
    APTH_FD_TABLE[fd].orig_flags = flags;
    atomic_store(&APTH_FD_TABLE[fd].managed, 1);
    atomic_store(&APTH_FD_TABLE[fd].refcount, 0);
}

APTH_INTERNAL void apth_fd_unregister(int fd)
{
    if (fd < 0 || fd >= FD_SETSIZE) return;
    // 恢复原始 flags（如果有人还在用，refcount > 0 的情况理论上不应该发生）
    if (atomic_load(&APTH_FD_TABLE[fd].managed))
    {
        fcntl(fd, F_SETFL, APTH_FD_TABLE[fd].orig_flags);
        atomic_store(&APTH_FD_TABLE[fd].managed, 0);
        atomic_store(&APTH_FD_TABLE[fd].refcount, 0);
    }
}

// 在 I/O 操作前调用：如果是首次使用，设为 NONBLOCK
APTH_INTERNAL int apth_fd_acquire(int fd)
{
    if (fd < 0 || fd >= FD_SETSIZE) return -1;

    struct apth_fd_entry *e = &APTH_FD_TABLE[fd];
    if (!atomic_load(&e->managed))
    {
        // 首次遇到此 fd（可能是用户在 libapth 之外打开的）
        e->orig_flags = fcntl(fd, F_GETFL, 0);
        if (e->orig_flags == -1) return -1;
        atomic_store(&e->managed, 1);
    }

    int old_ref = atomic_fetch_add(&e->refcount, 1);
    if (old_ref == 0)
    {
        // 首个使用者，设为 NONBLOCK
        if (!(e->orig_flags & O_NONBLOCK))
            fcntl(fd, F_SETFL, e->orig_flags | O_NONBLOCK);
    }
    // 如果 refcount > 0，fd 已经是 NONBLOCK，无需再次 fcntl

    return (e->orig_flags & O_NONBLOCK) ? APTH_FDMODE_NONBLOCK : APTH_FDMODE_BLOCK;
}

// 在 I/O 操作后调用：引用计数归零时恢复原始 flags
APTH_INTERNAL void apth_fd_release(int fd)
{
    if (fd < 0 || fd >= FD_SETSIZE) return;

    struct apth_fd_entry *e = &APTH_FD_TABLE[fd];
    int new_ref = atomic_fetch_sub(&e->refcount, 1) - 1;
    if (new_ref == 0)
    {
        // 最后一个使用者，恢复原始 flags
        if (!(e->orig_flags & O_NONBLOCK))
            fcntl(fd, F_SETFL, e->orig_flags);
    }
}
```

#### 步骤 3：修改 I/O 钩子

将所有 I/O 钩子中的：

```c
int fdmode;
if ((fdmode = apth_fdmode(fd, APTH_FDMODE_NONBLOCK)) == APTH_FDMODE_ERROR)
    return apth_error(-1, EBADF);
...
apth_shield { apth_fdmode(fd, fdmode); }
```

替换为：

```c
int orig_mode = apth_fd_acquire(fd);
if (orig_mode < 0)
    return apth_error(-1, EBADF);
...
apth_shield { apth_fd_release(fd); }
```

__效果__：

- 第一次 I/O 操作：2 次 fcntl（F_GETFL + F_SETFL）
- 后续 I/O 操作（同一 fd）：0 次 fcntl（仅原子 refcount 操作）
- 最后一个操作结束时：1 次 fcntl（恢复）
- 如果 fd 一直在被使用（常见场景）：__0 次 fcntl per I/O__

---

### 1.3 Event Manager 从 select 迁移到 epoll

__目标__：用 O(n_ready) 替代 O(fdmax × n_waiting)。

__涉及文件__：

- `src/internal_types.h`（scheduler 结构体加 epoll_fd）
- `src/internal/apth_sched.c`（init/kill 中创建/销毁 epoll）
- `src/internal/apth_event.c`（重写 eventmanager）

#### 步骤 1：在 scheduler 中添加 epoll fd

`src/internal_types.h` — `struct apth_perpthr_scheduler` 中添加：

```c
#include <sys/epoll.h>

struct apth_perpthr_scheduler
{
    // ... existing fields ...
    int epoll_fd;             // epoll instance for this scheduler
    int epoll_fd_count;       // 当前注册在 epoll 中的 fd 数量
};
```

#### 步骤 2：scheduler 初始化和销毁

`src/internal/apth_sched.c` — `apth_scheduler_init` 中添加：

```c
sched->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
if (sched->epoll_fd < 0)
    return apth_error(false, errno);
sched->epoll_fd_count = 0;
```

`apth_scheduler_kill` 中添加：

```c
if (sched->epoll_fd >= 0)
{
    apth_syscall_raw(close)(sched->epoll_fd);  // 用 raw close 避免递归
    sched->epoll_fd = -1;
}
```

#### 步骤 3：事件注册/注销辅助函数

在 `src/internal/apth_event.c` 中添加：

```c
#include <sys/epoll.h>

// 将 apth 的 FD 事件注册到 scheduler 的 epoll
static void epoll_register_fd_events(apth_sched_t sched, apth_t th)
{
    FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
    {
        apth_event_t event = apth_event_t_list_entry(ev_e);
        if (event->ev_type != APTH_EVENT_TYPE_FD)
            continue;
        if (event->ev_status != APTH_EV_STATUS_PENDING)
            continue;

        struct epoll_event ee = {0};
        if (event->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
            ee.events |= EPOLLIN;
        if (event->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
            ee.events |= EPOLLOUT;
        if (event->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
            ee.events |= EPOLLPRI;
        ee.events |= EPOLLONESHOT;  // 一次触发后自动禁用，避免重复通知
        ee.data.ptr = th;

        int rc = epoll_ctl(sched->epoll_fd, EPOLL_CTL_ADD, event->ev_args.FD.fd, &ee);
        if (rc < 0 && errno == EEXIST)
        {
            // fd 已注册（可能多个 apth 等待同一 fd），使用 MOD
            // 注意：epoll 不支持同一 fd 关联不同 data.ptr
            // 解决方案：使用间接层，后面详述
            epoll_ctl(sched->epoll_fd, EPOLL_CTL_MOD, event->ev_args.FD.fd, &ee);
        }
        if (rc == 0)
            sched->epoll_fd_count++;
    }
}

// 将 apth 的 FD 事件从 epoll 中注销
static void epoll_unregister_fd_events(apth_sched_t sched, apth_t th)
{
    FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
    {
        apth_event_t event = apth_event_t_list_entry(ev_e);
        if (event->ev_type != APTH_EVENT_TYPE_FD)
            continue;

        int rc = epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, event->ev_args.FD.fd, NULL);
        if (rc == 0)
            sched->epoll_fd_count--;
    }
}
```

__关于同一 fd 被多个 apth 等待的情况__：epoll 对同一 fd 只能注册一次（除非用 dup 创建不同的 file description）。解决方案：

1. __方案 A（简单）__：对 FD 事件仍使用旧的 select 逻辑作为 fallback（当同一 fd 有多个等待者时）。
2. __方案 B（推荐）__：维护一个 per-scheduler 的 `fd → list of waiting apths` 映射表。epoll 返回就绪 fd 时，查表唤醒所有等待该 fd 的 apth。

方案 B 实现：

```c
// 在 scheduler 中添加
struct apth_epoll_fd_waiters {
    int fd;
    uint32_t events;           // 聚合的事件掩码
    struct list waiting_apths; // 等待此 fd 的 apth 列表
    struct list_elem elem;
};
```

由于复杂度较高，__建议先用方案 A__：大多数场景下每个 fd 只有一个 apth 在等待（一个 reader 一个 writer 是不同事件类型），可以先实现简单版本。

#### 步骤 4：重写 `apth_sched_eventmanager`

```c
APTH_INTERNAL void apth_sched_eventmanager(apth_sched_t sched, apth_time_t *now, bool dopoll)
{
    // ==================== 阶段 1：处理非 I/O 事件 ====================
    // 遍历 waiting queue，处理 timer/signal/tid/func 事件
    // 同时将有 FD 事件的 apth 注册到 epoll（如果尚未注册）
    
    apth_time_t nexttimer_value;
    apth_time_set(&nexttimer_value, APTH_TIME_ZERO);
    bool has_timer = false;
    size_t waked_count = 0;

    // 单遍遍历 waiting queue
    lll_lock(&sched->waiting_queue->th_list_lock, "eventmanager");
    
    struct list_elem *e = list_begin(&sched->waiting_queue->th_list);
    while (e != list_end(&sched->waiting_queue->th_list))
    {
        apth_t th = apth_t_list_entry(e);
        struct list_elem *next = list_next(e);  // 保存 next，因为可能要移动 th
        bool wake_this = false;

        // 检查取消请求
        if (th->cancelreq)
            wake_this = true;

        // 遍历此 apth 的事件列表
        FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
        {
            apth_event_t event = apth_event_t_list_entry(ev_e);
            if (event->ev_status != APTH_EV_STATUS_PENDING)
                continue;

            switch (event->ev_type)
            {
            case APTH_EVENT_TYPE_TIME:
                if (apth_time_cmp(&event->ev_args.TIME.tv, now) < 0) {
                    event->ev_status = APTH_EV_STATUS_OCCURRED;
                    wake_this = true;
                } else if (!has_timer || apth_time_cmp(&event->ev_args.TIME.tv, &nexttimer_value) < 0) {
                    apth_time_set(&nexttimer_value, &event->ev_args.TIME.tv);
                    has_timer = true;
                }
                break;

            case APTH_EVENT_TYPE_SIGS:
                // 检查 sigpending（只在这一遍中做）
                for (int sig = 1; sig < APTH_NSIG; sig++) {
                    if (sigismember(event->ev_args.SIGS.sigs, sig)) {
                        lll_lock(&th->siglock, "ev_sigs");
                        if (sigismember(&th->sigpending, sig)) {
                            if (event->ev_args.SIGS.sig) *(event->ev_args.SIGS.sig) = sig;
                            sigdelset(&th->sigpending, sig);
                            th->sigpendcnt--;
                            lll_unlock(&th->siglock, "ev_sigs");
                            event->ev_status = APTH_EV_STATUS_OCCURRED;
                            wake_this = true;
                            break;
                        }
                        lll_unlock(&th->siglock, "ev_sigs");
                    }
                }
                break;

            case APTH_EVENT_TYPE_TID:
                if (apth_state_matches_event_goal(state_holder_of(event->ev_args.TID.tid), event->ev_goal)) {
                    event->ev_status = APTH_EV_STATUS_OCCURRED;
                    wake_this = true;
                }
                break;

            case APTH_EVENT_TYPE_FUNC:
                if (event->ev_args.FUNC.func(event->ev_args.FUNC.arg)) {
                    event->ev_status = APTH_EV_STATUS_OCCURRED;
                    wake_this = true;
                } else {
                    apth_time_t tv;
                    apth_time_set(&tv, now);
                    apth_time_add(&tv, &event->ev_args.FUNC.tv);
                    if (!has_timer || apth_time_cmp(&tv, &nexttimer_value) < 0) {
                        apth_time_set(&nexttimer_value, &tv);
                        has_timer = true;
                    }
                }
                break;

            case APTH_EVENT_TYPE_FD:
            case APTH_EVENT_TYPE_SELECT:
                // FD 事件在 epoll_wait 阶段处理
                break;

            default:
                break;
            }
        }

        if (wake_this) {
            // 移到 waked queue（此处需要从 waiting queue 中移除）
            // ... transfer logic ...
            waked_count++;
        }

        e = next;
    }
    lll_unlock(&sched->waiting_queue->th_list_lock, "eventmanager");

    // ==================== 阶段 2：epoll_wait 处理 I/O 事件 ====================
    
    int timeout_ms;
    if (dopoll || waked_count > 0) {
        timeout_ms = 0;  // 非阻塞探测
    } else if (has_timer) {
        apth_time_t diff;
        apth_time_set(&diff, &nexttimer_value);
        apth_time_sub(&diff, now);
        timeout_ms = (int)(diff.tv_sec * 1000 + diff.tv_usec / 1000);
        if (timeout_ms < 0) timeout_ms = 0;
    } else {
        timeout_ms = 0;  // 不阻塞，让 scheduler 去做其他事（work stealing 等）
    }

    if (sched->epoll_fd_count > 0 || (!dopoll && has_timer)) {
        struct epoll_event events[64];
        int nready = epoll_wait(sched->epoll_fd, events, 64, timeout_ms);

        for (int i = 0; i < nready; i++) {
            apth_t th = (apth_t)events[i].data.ptr
```

Checkpoint

Compare

Restore

Task Completed

# 四、详细改进执行指导

以下按优先级分组，每个改进项给出：__目标、涉及文件、具体代码变更步骤、注意事项__。

---

## Phase 0: 修复现有 Bug（P0）

### 0.1 修复 `writev` 中 `sizeof(tiov_stack)` cleanup 处的遗漏

__涉及文件__：`src/internal/apth_syscall.c`

你已修复了分配判断处，但函数末尾 cleanup 仍是旧代码：

```c
// Cleanup
if (iovcnt > (int)sizeof(tiov_stack))  // ← 也需要修复！
    free(tiov);
```

改为：

```c
if (iovcnt > (int)(sizeof(tiov_stack)/sizeof(tiov_stack[0])))
    free(tiov);
```

建议定义常量 `#define APTH_WRITEV_TIOV_STACK_COUNT (sizeof(tiov_stack)/sizeof(tiov_stack[0]))` 避免重复。

---

### 0.2 实现 `socket` 钩子

```c
APTH_DEFINE_SYSCALL(int, socket,
                    (int domain, int type, int protocol),
                    (domain, type, protocol))
{
    int fd = apth_syscall_raw(socket)(domain, type, protocol);
    if (fd >= 0 && fd < FD_SETSIZE) {
        APTH_FD_TABLE[fd].orig_flags = fcntl(fd, F_GETFL, 0);
        APTH_FD_TABLE[fd].managed = 1;
        APTH_FD_TABLE[fd].refcount = 0;
    }
    return fd;
}
```

不要在创建时设 NONBLOCK，由各 I/O 钩子在需要时设置。

---

### 0.3 实现 `close` 钩子

```c
APTH_DEFINE_SYSCALL(int, close, (int fd), (fd))
{
    if (fd >= 0 && fd < FD_SETSIZE) {
        APTH_FD_TABLE[fd].orig_flags = 0;
        APTH_FD_TABLE[fd].managed = 0;
        APTH_FD_TABLE[fd].refcount = 0;
    }
    // 未来用 epoll 时：epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, fd, NULL); (忽略错误)
    return apth_syscall_raw(close)(fd);
}
```

---

### 0.4 实现 `setsockopt` 钩子

直接透传：

```c
APTH_DEFINE_SYSCALL(int, setsockopt,
    (int fd, int level, int option_name, const void *option_value, socklen_t option_len),
    (fd, level, option_name, option_value, option_len))
{
    return apth_syscall_raw(setsockopt)(fd, level, option_name, option_value, option_len);
}
```

---

### 0.5 ⚠️ `select` 钩子注释 `selected` 后产生的严重逻辑 Bug

你最新版本中：

```c
if (ev_select->ev_status == APTH_EV_STATUS_OCCURRED)
    // selected = true;
    if (timeout != NULL && ev_timeout->ev_status == APTH_EV_STATUS_OCCURRED)
    { ... }
```

注释掉 `selected = true;` 后，两个 `if` 变成了嵌套关系！只有当 select 和 timeout __都__ OCCURRED 才进清零分支。

__修复__：

```c
if (ev_select->ev_status == APTH_EV_STATUS_FAILED)
    return apth_error(-1, EBADF);

// timeout 到达但 select 事件未发生 → 返回 0 并清零
if (timeout != NULL && ev_timeout->ev_status == APTH_EV_STATUS_OCCURRED
    && ev_select->ev_status != APTH_EV_STATUS_OCCURRED)
{
    if (rfds != NULL) FD_ZERO(rfds);
    if (wfds != NULL) FD_ZERO(wfds);
    if (efds != NULL) FD_ZERO(efds);
    rc = 0;
}
return rc;
```

---

## Phase 1: 性能优化（P1）

### 1.1 消除"先 select 探测再 I/O"的双系统调用

__改造模板（以 read 为例）__：去掉 `FD_ZERO+FD_SET+select` 快速探测，直接尝试非阻塞 I/O：

```c
APTH_DEFINE_SYSCALL(ssize_t, read, (int fd, void *buf, size_t nbytes), (fd, buf, nbytes))
{
    if (nbytes == 0) return 0;
    if (!apth_util_fd_valid(fd)) return apth_error(-1, EBADF);

    int fdmode;
    if ((fdmode = apth_fdmode(fd, APTH_FDMODE_NONBLOCK)) == APTH_FDMODE_ERROR)
        return apth_error(-1, EBADF);

    ssize_t rv;
    for (;;) {
        while ((rv = apth_syscall_raw(read)(fd, buf, nbytes)) < 0 && errno == EINTR) ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            apth_event_t ev = apth_event_fd(APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, fd);
            apth_wait_event(ev);
            continue;
        }
        break;
    }
    apth_shield { apth_fdmode(fd, fdmode); }
    return rv;
}
```

对 write（循环处理短写+EAGAIN）、readv、writev、recvfrom、sendto 做同样改造。最佳情况从 6 次系统调用降到 3 次。

---

### 1.2 引入 per-fd 状态管理，消除 fcntl 风暴

__在 `src/internal/apth_fd.c` 中添加__：

```c
APTH_INTERNAL int apth_fd_acquire(int fd) {
    if (fd < 0 || fd >= FD_SETSIZE) return -1;
    struct apth_fd_entry *e = &APTH_FD_TABLE[fd];
    if (!atomic_load(&e->managed)) {
        e->orig_flags = fcntl(fd, F_GETFL, 0);
        if (e->orig_flags == -1) return -1;
        atomic_store(&e->managed, 1);
    }
    int old_ref = atomic_fetch_add(&e->refcount, 1);
    if (old_ref == 0 && !(e->orig_flags & O_NONBLOCK))
        fcntl(fd, F_SETFL, e->orig_flags | O_NONBLOCK);
    return (e->orig_flags & O_NONBLOCK) ? APTH_FDMODE_NONBLOCK : APTH_FDMODE_BLOCK;
}

APTH_INTERNAL void apth_fd_release(int fd) {
    if (fd < 0 || fd >= FD_SETSIZE) return;
    struct apth_fd_entry *e = &APTH_FD_TABLE[fd];
    int new_ref = atomic_fetch_sub(&e->refcount, 1) - 1;
    if (new_ref == 0 && !(e->orig_flags & O_NONBLOCK))
        fcntl(fd, F_SETFL, e->orig_flags);
}
```

在 I/O 钩子中将 `apth_fdmode(fd, NONBLOCK)` / `apth_fdmode(fd, fdmode)` 替换为 `apth_fd_acquire(fd)` / `apth_fd_release(fd)`。配合 1.1 后，__最佳情况每次 I/O 仅 1 次系统调用__。

---

### 1.3 Event Manager 迁移到 epoll

#### 步骤 1：scheduler 中添加 epoll fd

在 `struct apth_perpthr_scheduler` 加 `int epoll_fd;`，init 时 `epoll_create1(EPOLL_CLOEXEC)`，kill 时 `close`。

#### 步骤 2：FD 事件注册到 epoll

当 apth 进入 waiting 且有 FD 事件时：

```c
struct epoll_event ee = {0};
if (goal & APTH_GOAL_UNTIL_FD_READABLE) ee.events |= EPOLLIN;
if (goal & APTH_GOAL_UNTIL_FD_WRITEABLE) ee.events |= EPOLLOUT;
ee.events |= EPOLLONESHOT;
ee.data.ptr = th;
epoll_ctl(sched->epoll_fd, EPOLL_CTL_ADD, fd, &ee);
```

#### 步骤 3：重写 eventmanager 为两阶段

```javascript
阶段1（单遍遍历waiting queue）：
  - 处理 timer/signal/tid/func 事件（直接判断）
  - 确保 FD 事件已注册到 epoll
  - 计算 nexttimer 作为 epoll timeout

阶段2（epoll_wait）：
  timeout_ms = dopoll ? 0 : nexttimer_ms;
  nready = epoll_wait(epoll_fd, events, 64, timeout_ms);
  for each ready event:
    th = events[i].data.ptr;
    标记 FD 事件为 OCCURRED;
    移到 waked_queue;
```

__同一 fd 多个 apth 等待__：用 `EPOLLONESHOT` + 唤醒后重新注册。或维护 `fd → apth list` 映射。初期可以对这种罕见情况 fallback 到 select。

#### 步骤 4：SELECT 类型事件兼容

用户调用 hooked `select` 产生的 `APTH_EVENT_TYPE_SELECT` 事件，拆解为多个 epoll fd 注册项。或者对这种事件单独用一次 `apth_syscall_raw(select)` 检查（仅作为 fallback）。

---

## Phase 2: 功能完善和进一步优化（P2）

### 2.1 实现 `poll` 钩子

```c
APTH_DEFINE_SYSCALL(int, poll, (struct pollfd *fds, nfds_t nfds, int timeout), (fds, nfds, timeout))
{
    if (nfds == 0) {
        if (timeout > 0) { usleep(timeout * 1000); }
        return 0;
    }
    // 先做 0-timeout 探测
    int rc;
    while ((rc = apth_syscall_raw(poll)(fds, nfds, 0)) < 0 && errno == EINTR) ;
    if (rc > 0 || timeout == 0) return rc;

    // 构建事件列表：每个 pollfd 一个 FD 事件
    struct list event_list;
    list_init(&event_list);
    for (nfds_t i = 0; i < nfds; i++) {
        unsigned long goal = APTH_EVENT_MODE_STATIC;
        if (fds[i].events & POLLIN)  goal |= APTH_GOAL_UNTIL_FD_READABLE;
        if (fds[i].events & POLLOUT) goal |= APTH_GOAL_UNTIL_FD_WRITEABLE;
        apth_event_t ev = apth_event_fd(goal, fds[i].fd);
        apth_event_list_add(&event_list, ev);
    }
    if (timeout > 0) {
        apth_event_t ev_timeout = apth_event_time(APTH_EVENT_MODE_STATIC,
            apth_timeout(timeout / 1000, (timeout % 1000) * 1000));
        apth_event_list_add(&event_list, ev_timeout);
    }
    apth_wait_event_list(&event_list);

    // 重新 poll 以获取 revents
    while ((rc = apth_syscall_raw(poll)(fds, nfds, 0)) < 0 && errno == EINTR) ;
    return rc;
}
```

### 2.2 事件对象池

在 `struct apth_perpthr_scheduler` 中添加：

```c
struct apth_event_st event_pool[64];  // 预分配池
int event_pool_top;                    // 栈顶指针
```

`prepare_ev` 优先从池中取，`apth_event_free` 归还池中。溢出时 fallback 到 malloc。

### 2.3 `apth_fd_table_init` 调用

在 `apth_syscall_system_init` 中调用 `memset(APTH_FD_TABLE, 0, sizeof(APTH_FD_TABLE));` 初始化表。

---

## 总结：改造后的 I/O 数据流

```javascript
用户调用 read(fd, buf, n)
  → LD_PRELOAD 重定向到 apth_syscall_read
    → apth_fd_acquire(fd)           [0次 fcntl（如果 fd 已 managed）]
    → raw read(fd, buf, n)          [1次系统调用]
      → 成功？直接返回
      → EAGAIN？
        → apth_event_fd(READABLE)
        → apth_wait_event(ev)
          → submit WAITING state
          → apth_yield → 切回 scheduler
          → scheduler 运行其他 apth...
          → eventmanager: epoll_wait  [1次系统调用，O(n_ready)]
          → fd ready → 唤醒此 apth
        → 重试 raw read
    → apth_fd_release(fd)           [0次 fcntl]
```

最佳路径（数据已就绪）：__1 次系统调用__（仅 read 本身） 次佳路径（需要等待）：__1 次 read(EAGAIN) + 1 次 epoll_wait + 1 次 read = 3 次__ 对比原来最佳路径的 6 次，提升显著。


接下来我将从 apth_sched_eventmanager 的重写步骤 4 中断的位置继续，并详细给出方案 B（per-scheduler 的 fd → waiting apths 映射表）的完整设计。

步骤 4（续）：重写 apth_sched_eventmanager — 完整版
前置：方案 B 的 fd→apths 映射表数据结构
首先需要定义映射表的数据结构。

在 src/internal_types.h 中添加：

#include <sys/epoll.h>

// ============================== Epoll FD Waiter Map ==============================

// 一个等待某个 fd 的 apth 条目（链入 epoll_fd_slot 的等待链表）
struct apth_epoll_waiter {
    apth_t th;                   // 等待的 apth
    apth_event_t ev;             // 对应的事件对象（方便标记 OCCURRED）
    struct list_elem elem;       // 链入 epoll_fd_slot.waiters
#define apth_epoll_waiter_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_epoll_waiter, elem)
};

// 一个被 epoll 监控的 fd 槽位
struct apth_epoll_fd_slot {
    int fd;                      // 被监控的 fd
    uint32_t aggregate_events;   // 所有等待者的事件掩码的聚合（EPOLLIN|EPOLLOUT|...）
    struct list waiters;         // 等待此 fd 的 apth 列表 [elem: struct apth_epoll_waiter]
    int waiter_count;            // 等待者数量
    bool registered;             // 是否已注册到 epoll
    struct list_elem elem;       // 链入 scheduler 的 active_fd_slots 链表
#define apth_epoll_fd_slot_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_epoll_fd_slot, elem)
};

// fd → slot 的快速查找表（数组，以 fd 为下标）
#define APTH_EPOLL_FD_SLOT_TABLE_SIZE FD_SETSIZE
在 struct apth_perpthr_scheduler 中添加：

struct apth_perpthr_scheduler
{
    // ... 现有字段 ...

    // ===== epoll 相关 =====
    int epoll_fd;                                                    // epoll 实例
    struct apth_epoll_fd_slot fd_slot_table[APTH_EPOLL_FD_SLOT_TABLE_SIZE]; // fd→slot 快速查找
    struct list active_fd_slots;                                     // 当前有等待者的 slot 链表
    int active_fd_count;                                             // 活跃的被监控 fd 数量
};
初始化和销毁
apth_scheduler_init 中添加：

sched->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
if (sched->epoll_fd < 0)
    return apth_error(false, errno);

// 初始化 fd slot 表
for (int i = 0; i < APTH_EPOLL_FD_SLOT_TABLE_SIZE; i++) {
    sched->fd_slot_table[i].fd = i;
    sched->fd_slot_table[i].aggregate_events = 0;
    list_init(&sched->fd_slot_table[i].waiters);
    sched->fd_slot_table[i].waiter_count = 0;
    sched->fd_slot_table[i].registered = false;
}
list_init(&sched->active_fd_slots);
sched->active_fd_count = 0;
apth_scheduler_kill 中添加：

if (sched->epoll_fd >= 0) {
    apth_syscall_raw(close)(sched->epoll_fd);
    sched->epoll_fd = -1;
}
// active_fd_slots 和 fd_slot_table 内的 waiters 在 drain_thqueue 过程中
// 已经随 apth 被清理，这里不需要额外处理
映射表操作辅助函数
在 src/internal/apth_event.c 中添加：


#include <sys/epoll.h>

// ==================== fd→apths 映射表操作 ====================

// 将一个 (apth, event) 对注册到 fd 对应的 slot 中
// 如果 fd 是首次被等待，同时注册到 epoll
static int epoll_map_add_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
{
    if (fd < 0 || fd >= APTH_EPOLL_FD_SLOT_TABLE_SIZE)
        return -1;

    struct apth_epoll_fd_slot *slot = &sched->fd_slot_table[fd];

    // 创建 waiter 条目
    struct apth_epoll_waiter *w = (struct apth_epoll_waiter *)malloc(sizeof(*w));
    if (w == NULL) return -1;
    w->th = th;
    w->ev = ev;

    // 计算此 waiter 需要的事件掩码
    uint32_t needed = 0;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)  needed |= EPOLLIN;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)  needed |= EPOLLOUT;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)  needed |= EPOLLPRI;

    // 加入 waiter 链表
    list_push_back(&slot->waiters, &w->elem);
    slot->waiter_count++;

    // 更新聚合掩码
    uint32_t old_aggregate = slot->aggregate_events;
    slot->aggregate_events |= needed;

    // 如果是此 fd 的第一个等待者，链入 active_fd_slots 并注册到 epoll
    if (!slot->registered) {
        list_push_back(&sched->active_fd_slots, &slot->elem);
        sched->active_fd_count++;

        struct epoll_event ee;
        ee.events = slot->aggregate_events;
        ee.data.fd = fd;  // 用 fd 而非 ptr，因为一个 fd 对应多个 apth
        int rc = epoll_ctl(sched->epoll_fd, EPOLL_CTL_ADD, fd, &ee);
        if (rc < 0) {
            // 注册失败，清理
            list_remove(&w->elem);
            slot->waiter_count--;
            slot->aggregate_events = old_aggregate;
            if (slot->waiter_count == 0) {
                list_remove(&slot->elem);
                sched->active_fd_count--;
            }
            free(w);
            return -1;
        }
        slot->registered = true;
    }
    else if (slot->aggregate_events != old_aggregate) {
        // 掩码变了（比如之前只有 EPOLLIN，现在又加了 EPOLLOUT），需要 MOD
        struct epoll_event ee;
        ee.events = slot->aggregate_events;
        ee.data.fd = fd;
        epoll_ctl(sched->epoll_fd, EPOLL_CTL_MOD, fd, &ee);
    }

    return 0;
}

// 将一个 (apth, event) 对从 fd 对应的 slot 中移除
// 如果 fd 的最后一个等待者被移除，同时从 epoll 中注销
static void epoll_map_remove_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
{
    if (fd < 0 || fd >= APTH_EPOLL_FD_SLOT_TABLE_SIZE)
        return;

    struct apth_epoll_fd_slot *slot = &sched->fd_slot_table[fd];

    // 在 waiters 链表中找到并移除对应的 waiter
    FOR_ELEMENT_IN_LIST(slot->waiters, e) {
        struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
        if (w->th == th && w->ev == ev) {
            list_remove(&w->elem);
            free(w);
            slot->waiter_count--;
            break;
        }
    }

    if (slot->waiter_count == 0) {
        // 最后一个等待者被移除，从 epoll 中注销
        if (slot->registered) {
            epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
            slot->registered = false;
            list_remove(&slot->elem);  // 从 active_fd_slots 中移除
            sched->active_fd_count--;
        }
        slot->aggregate_events = 0;
    }
    else {
        // 重新计算聚合掩码（因为被移除的 waiter 可能是唯一需要某个方向的）
        uint32_t new_aggregate = 0;
        FOR_ELEMENT_IN_LIST(slot->waiters, e2) {
            struct apth_epoll_waiter *w2 = apth_epoll_waiter_list_entry(e2);
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)  new_aggregate |= EPOLLIN;
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)  new_aggregate |= EPOLLOUT;
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)  new_aggregate |= EPOLLPRI;
        }
        if (new_aggregate != slot->aggregate_events) {
            slot->aggregate_events = new_aggregate;
            struct epoll_event ee;
            ee.events = new_aggregate;
            ee.data.fd = fd;
            epoll_ctl(sched->epoll_fd, EPOLL_CTL_MOD, fd, &ee);
        }
    }
}

// 当 epoll 返回 fd 就绪时，唤醒所有匹配的等待者
// 返回被唤醒的 apth 数量
static int epoll_map_wake_fd(apth_sched_t sched, int fd, uint32_t revents)
{
    if (fd < 0 || fd >= APTH_EPOLL_FD_SLOT_TABLE_SIZE)
        return 0;

    struct apth_epoll_fd_slot *slot = &sched->fd_slot_table[fd];
    int waked = 0;

    // 遍历此 fd 的所有 waiter，检查哪些的事件条件被满足
    struct list_elem *e = list_begin(&slot->waiters);
    while (e != list_end(&slot->waiters)) {
        struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
        struct list_elem *next = list_next(e);  // 提前保存，因为可能要移除

        bool matched = false;
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE) && (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)))
            matched = true;
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE) && (revents & (EPOLLOUT | EPOLLERR)))
            matched = true;
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION) && (revents & (EPOLLPRI | EPOLLERR)))
            matched = true;

        // EPOLLERR 和 EPOLLHUP 始终匹配（fd 出错或对端关闭，所有等待者都应被唤醒）
        if (revents & (EPOLLERR | EPOLLHUP))
            matched = true;

        if (matched) {
            // 标记事件为 OCCURRED
            w->ev->ev_status = APTH_EV_STATUS_OCCURRED;
            apth_debug("[epoll] fd=%d event occurred for apth \"%s\"", fd, w->th->name);

            // 从 waiter 链表中移除
            list_remove(&w->elem);
            slot->waiter_count--;

            // 记住需要被唤醒的 apth（但不在这里直接移动队列，
            // 因为一个 apth 可能有多个事件，由外层统一处理）
            free(w);
            waked++;
        }

        e = next;
    }

    // 如果所有等待者都被移除，从 epoll 中注销
    if (slot->waiter_count == 0 && slot->registered) {
        epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        slot->registered = false;
        list_remove(&slot->elem);
        sched->active_fd_count--;
        slot->aggregate_events = 0;
    }
    else if (waked > 0) {
        // 重新计算聚合掩码
        uint32_t new_aggregate = 0;
        FOR_ELEMENT_IN_LIST(slot->waiters, e2) {
            struct apth_epoll_waiter *w2 = apth_epoll_waiter_list_entry(e2);
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)  new_aggregate |= EPOLLIN;
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)  new_aggregate |= EPOLLOUT;
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)  new_aggregate |= EPOLLPRI;
        }
        if (new_aggregate != slot->aggregate_events) {
            slot->aggregate_events = new_aggregate;
            struct epoll_event ee;
            ee.events = new_aggregate;
            ee.data.fd = fd;
            epoll_ctl(sched->epoll_fd, EPOLL_CTL_MOD, fd, &ee);
        }
    }

    return waked;
}
完整的 apth_sched_eventmanager 重写

APTH_INTERNAL void apth_sched_eventmanager(apth_sched_t sched, apth_time_t *now, bool dopoll)
{
    apth_debug("enter in %s mode", dopoll ? "polling" : "waiting");

    for (;;)
    {
        bool loop_repeat = false;

        // ==================== 阶段 1：遍历 waiting queue ====================
        // 处理非 I/O 事件（timer, signal, tid, func），
        // 同时将 FD 事件注册到 epoll 映射表

        apth_time_t nexttimer_value;
        apth_time_set(&nexttimer_value, APTH_TIME_ZERO);
        apth_event_t nexttimer_ev = APTH_EVENT_NULL;
        apth_t nexttimer_th = APTH_NULL;
        bool has_timer = false;
        size_t notified_ths = 0;

        // 收集需要被唤醒的 apth（避免在持有 waiting_queue 锁时操作 waked_queue）
        // 使用一个临时数组/链表
        #define MAX_WAKE_BATCH 128
        apth_t wake_batch[MAX_WAKE_BATCH];
        int wake_count = 0;

        lll_lock(&sched->waiting_queue->th_list_lock, "eventmanager_phase1");

        FOR_ELEMENT_IN_LIST(sched->waiting_queue->th_list, e)
        {
            apth_t th = apth_t_list_entry(e);
            bool any_occurred = false;

            // 检查取消请求
            if (th->cancelreq)
                any_occurred = true;

            if (list_empty(&th->event_list))
                goto check_wake;

            FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
            {
                apth_event_t event = apth_event_t_list_entry(ev_e);
                if (event->ev_status != APTH_EV_STATUS_PENDING)
                {
                    // 之前的 epoll_map_wake_fd 可能已经标记了这个事件
                    any_occurred = true;
                    continue;
                }

                switch (event->ev_type)
                {
                case APTH_EVENT_TYPE_FD:
                    // 注册到 epoll 映射表（如果尚未注册）
                    // 注意：需要检查是否已经注册过（避免重复注册）
                    // 可以在 event 中加一个 flag 标记是否已注册，或者
                    // 简单地每轮都调用（epoll_map_add_waiter 内部处理去重）
                    // 这里采用简单方式：只在 event 首次进入 waiting 时注册
                    // 通过检查 slot 的 waiters 中是否已有此 (th, ev) 对
                    epoll_map_add_waiter(sched, event->ev_args.FD.fd, th, event);
                    break;

                case APTH_EVENT_TYPE_SELECT:
                    // SELECT 事件：将 fd_set 中的每个 fd 拆解注册到 epoll
                    // 或者作为 fallback 用旧的 select 检查
                    // （详见下方 SELECT 兼容方案）
                    {
                        // Fallback: 对 SELECT 事件做一次快速的 raw select 检查
                        struct timeval zero_tv = {0, 0};
                        fd_set trfds, twfds, tefds;
                        fd_set *prfds = NULL, *pwfds = NULL, *pefds = NULL;
                        if (event->ev_args.SELECT.rfds) { memcpy(&trfds, event->ev_args.SELECT.rfds, sizeof(fd_set)); prfds = &trfds; }
                        if (event->ev_args.SELECT.wfds) { memcpy(&twfds, event->ev_args.SELECT.wfds, sizeof(fd_set)); pwfds = &twfds; }
                        if (event->ev_args.SELECT.efds) { memcpy(&tefds, event->ev_args.SELECT.efds, sizeof(fd_set)); pefds = &tefds; }

                        int rc;
                        while ((rc = apth_syscall_raw(select)(event->ev_args.SELECT.nfd, prfds, pwfds, pefds, &zero_tv)) < 0 && errno == EINTR)
                            ;
                        if (rc > 0) {
                            // 有 fd 就绪
                            int n = apth_util_fds_select(event->ev_args.SELECT.nfd,
                                event->ev_args.SELECT.rfds, prfds,
                                event->ev_args.SELECT.wfds, pwfds,
                                event->ev_args.SELECT.efds, pefds);
                            if (event->ev_args.SELECT.n) *(event->ev_args.SELECT.n) = n;
                            event->ev_status = APTH_EV_STATUS_OCCURRED;
                            any_occurred = true;
                        } else if (rc < 0) {
                            event->ev_status = APTH_EV_STATUS_FAILED;
                            any_occurred = true;
                        }
                        // rc == 0: 还没就绪，下轮再检查
                    }
                    break;

                case APTH_EVENT_TYPE_SIGS:
                    // 信号检查（只在这一遍中做，不再需要 second_loop）
                    for (int sig = 1; sig < APTH_NSIG; sig++) {
                        if (sigismember(event->ev_args.SIGS.sigs, sig)) {
                            lll_lock(&th->siglock, "ev_sigs_epoll");
                            if (sigismember(&th->sigpending, sig)) {
                                if (event->ev_args.SIGS.sig) *(event->ev_args.SIGS.sig) = sig;
                                sigdelset(&th->sigpending, sig);
                                th->sigpendcnt--;
                                lll_unlock(&th->siglock, "ev_sigs_epoll");
                                event->ev_status = APTH_EV_STATUS_OCCURRED;
                                any_occurred = true;
                                break;
                            }
                            lll_unlock(&th->siglock, "ev_sigs_epoll");
                        }
                    }
                    break;

                case APTH_EVENT_TYPE_TIME:
                    if (apth_time_cmp(&event->ev_args.TIME.tv, now) < 0) {
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                        any_occurred = true;
                    } else {
                        if (!has_timer || apth_time_cmp(&event->ev_args.TIME.tv, &nexttimer_value) < 0) {
                            apth_time_set(&nexttimer_value, &event->ev_args.TIME.tv);
                            nexttimer_ev = event;
                            nexttimer_th = th;
                            has_timer = true;
                        }
                    }
                    break;

                case APTH_EVENT_TYPE_TID:
                    if ((event->ev_args.TID.tid == NULL && thqueue_size(sched->terminated_queue) != 0) ||
                        (event->ev_args.TID.tid != NULL &&
                         apth_state_matches_event_goal(state_holder_of(event->ev_args.TID.tid), event->ev_goal)))
                    {
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                        any_occurred = true;
                    }
                    break;

                case APTH_EVENT_TYPE_FUNC:
                    if (event->ev_args.FUNC.func(event->ev_args.FUNC.arg)) {
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                        any_occurred = true;
                    } else {
                        apth_time_t tv;
                        apth_time_set(&tv, now);
                        apth_time_add(&tv, &event->ev_args.FUNC.tv);
                        if (!has_timer || apth_time_cmp(&tv, &nexttimer_value) < 0) {
                            apth_time_set(&nexttimer_value, &tv);
                            nexttimer_ev = event;
                            nexttimer_th = th;
                            has_timer = true;
                        }
                    }
                    break;

                default:
                    break;
                }
            }

        check_wake:
            if (any_occurred) {
                notified_ths++;
                if (wake_count < MAX_WAKE_BATCH)
                    wake_batch[wake_count++] = th;
            }
        }

        lll_unlock(&sched->waiting_queue->th_list_lock, "eventmanager_phase1");

        // 将阶段 1 中发现的需唤醒 apth 从 waiting 移到 waked
        for (int i = 0; i < wake_count; i++) {
            apth_t th = wake_batch[i];
            // 从 epoll 映射表中移除此 apth 的所有 FD 等待
            FOR_ELEMENT_IN_LIST(th->event_list, ev_e) {
                apth_event_t event = apth_event_t_list_entry(ev_e);
                if (event->ev_type == APTH_EVENT_TYPE_FD)
                    epoll_map_remove_waiter(sched, event->ev_args.FD.fd, th, event);
            }
            // 移动到 waked queue
            transfer_th(th, sched->waiting_queue, sched->waked_queue);
        }

        // 如果阶段 1 有唤醒，则 epoll_wait 用 0 超时（纯探测）
        if (notified_ths > 0)
            dopoll = true;

        // ==================== 阶段 2：epoll_wait 处理 I/O 事件 ====================

        int timeout_ms;
        if (dopoll) {
            timeout_ms = 0;
        } else if (has_timer) {
            apth_time_t diff;
            apth_time_set(&diff, &nexttimer_value);
            apth_time_sub(&diff, now);
            timeout_ms = (int)(diff.tv_sec * 1000 + diff.tv_usec / 1000);
            if (timeout_ms < 0) timeout_ms = 0;
            if (timeout_ms > 60000) timeout_ms = 60000;  // 上限 60 秒
        } else {
            // 没有 timer 也没有急事，不阻塞（让 scheduler 去 work steal）
            timeout_ms = 0;
        }

        // 只有当有 fd 被监控或者需要等待 timer 时才调用 epoll_wait
        if (sched->active_fd_count > 0 || (!dopoll && has_timer))
        {
            struct epoll_event ep_events[64];
            int nready = epoll_wait(sched->epoll_fd, ep_events, 64, timeout_ms);

            if (nready > 0)
            {
                // 处理就绪的 fd
                for (int i = 0; i < nready; i++)
                {
                    int ready_fd = ep_events[i].data.fd;
                    uint32_t revents = ep_events[i].events;

                    // 通过映射表唤醒所有匹配的等待者
                    epoll_map_wake_fd(sched, ready_fd, revents);
                }

                // epoll_map_wake_fd 已经标记了事件为 OCCURRED，
                // 但还没有移动 apth 到 waked_queue。
                // 现在遍历 waiting queue 找出有 OCCURRED 事件的 apth 并移动。
                wake_count = 0;
                lll_lock(&sched->waiting_queue->th_list_lock, "eventmanager_phase2_wake");
                FOR_ELEMENT_IN_LIST(sched->waiting_queue->th_list, e)
                {
                    apth_t th = apth_t_list_entry(e);
                    bool should_wake = false;
                    FOR_ELEMENT_IN_LIST(th->event_list, ev_e) {
                        apth_event_t event = apth_event_t_list_entry(ev_e);
                        if (event->ev_status != APTH_EV_STATUS_PENDING) {
                            should_wake = true;
                            break;
                        }
                    }
                    if (should_wake && wake_count < MAX_WAKE_BATCH)
                        wake_batch[wake_count++] = th;
                }
                lll_unlock(&sched->waiting_queue->th_list_lock, "eventmanager_phase2_wake");

                for (int i = 0; i < wake_count; i++) {
                    apth_t th = wake_batch[i];
                    // 清理此 apth 剩余的 epoll 注册
                    FOR_ELEMENT_IN_LIST(th->event_list, ev_e) {
                        apth_event_t event = apth_event_t_list_entry(ev_e);
                        if (event->ev_type == APTH_EVENT_TYPE_FD &&
                            event->ev_status == APTH_EV_STATUS_PENDING)
                            epoll_map_remove_waiter(sched, event->ev_args.FD.fd, th, event);
                    }
                    transfer_th(th, sched->waiting_queue, sched->waked_queue);
                }
            }
            else if (nready == 0 && !dopoll && has_timer)
            {
                // epoll_wait 超时了，说明 timer 可能到期
                if (nexttimer_ev != NULL) {
                    if (nexttimer_ev->ev_type == APTH_EVENT_TYPE_FUNC) {
                        // 是 func 事件的隐式 timer，需要重新检查
                        loop_repeat = true;
                    } else {
                        // 是显式 timer 事件
                        nexttimer_ev->ev_status = APTH_EV_STATUS_OCCURRED;
                        apth_debug("[timeout] event occurred for apth \"%s\"", nexttimer_th->name);
                        // 移动到 waked queue
                        FOR_ELEMENT_IN_LIST(nexttimer_th->event_list, ev_e) {
                            apth_event_t event = apth_event_t_list_entry(ev_e);
                            if (event->ev_type == APTH_EVENT_TYPE_FD)
                                epoll_map_remove_waiter(sched, event->ev_args.FD.fd, nexttimer_th, event);
                        }
                        transfer_th(nexttimer_th, sched->waiting_queue, sched->waked_queue);
                    }
                }
            }
        }

        // 循环控制
        if (loop_repeat) {
            apth_time_set(now, APTH_TIME_NOW);
            continue;
        } else {
            break;
        }
    }

    apth_debug("leave");
}
关键：apth 被



Task Completed

（接上文，从"关键：apth 被唤醒后的清理"继续）

关键：apth 被唤醒后离开 waiting 状态时的 epoll 清理
当一个 apth 从 apth_wait_event / apth_wait_event_list 返回后，它的事件会被从 event_list 中移除。但在此之前，如果有 FD 事件注册在 epoll 映射表中，必须先清理。

这有两个时机可以做：

时机 A（推荐）：在 eventmanager 中移动 apth 到 waked_queue 之前

即上面代码中已经做的：


// 在 transfer_th 之前
FOR_ELEMENT_IN_LIST(th->event_list, ev_e) {
    apth_event_t event = apth_event_t_list_entry(ev_e);
    if (event->ev_type == APTH_EVENT_TYPE_FD)
        epoll_map_remove_waiter(sched, event->ev_args.FD.fd, th, event);
}
transfer_th(th, sched->waiting_queue, sched->waked_queue);
时机 B：安全网 — 在 apth_wait_event 返回后

修改 apth_wait_event 中在 list_remove(&ev->elem) 之前：


// 安全网：确保 FD 事件已从 epoll 映射表中移除
// （正常情况下 eventmanager 已经做了，这里是防御性编程）
if (ev->ev_type == APTH_EVENT_TYPE_FD) {
    apth_sched_t sched = cur_sched();
    epoll_map_remove_waiter(sched, ev->ev_args.FD.fd, self, ev);
}
注册去重问题
epoll_map_add_waiter 可能在每轮 eventmanager 循环中被重复调用。需要防止同一个 (th, ev) 对被多次添加到同一个 slot 的 waiters 链表中。

方案 1（简单）：在 event 结构体中加一个 flag

在 struct apth_event_st 中添加：


struct apth_event_st {
    // ... existing fields ...
    bool epoll_registered;  // 是否已注册到 epoll 映射表
};
prepare_ev 中初始化为 false。

然后在 epoll_map_add_waiter 开头检查：


static int epoll_map_add_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
{
    if (ev->epoll_registered)
        return 0;  // 已注册，跳过
    // ... 其余逻辑 ...
    ev->epoll_registered = true;
    return 0;
}
在 epoll_map_remove_waiter 中：


static void epoll_map_remove_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
{
    if (!ev->epoll_registered)
        return;
    // ... 其余逻辑 ...
    ev->epoll_registered = false;
}
方案 2（零开销）：只在 apth 首次进入 waiting 时注册

不在 eventmanager 的遍历循环中注册，而是在 apth_wait_event / apth_wait_event_list 中、apth 进入 WAITING 状态之前注册：


APTH_INTERNAL bool apth_wait_event(apth_event_t ev)
{
    // ... 现有代码 ...
    apth_event_list_add(&self->event_list, ev);

    // 注册 FD 事件到 epoll（在进入 WAITING 之前）
    if (ev->ev_type == APTH_EVENT_TYPE_FD) {
        apth_sched_t sched = cur_sched();
        epoll_map_add_waiter(sched, ev->ev_args.FD.fd, self, ev);
    }

    submit_desired_state_to(self, APTH_STATE_WAITING, "apth_wait_event");
    apth_yield();
    // ...
}
这样 eventmanager 的阶段 1 中就不需要调用 epoll_map_add_waiter，FD 事件已经在进入 waiting 时就注册好了。eventmanager 只需要处理 epoll_wait 的结果。

推荐方案 2，因为：

注册只发生一次，无需去重逻辑
eventmanager 遍历 waiting queue 时无需关注 FD 事件（已经在 epoll 中）
代码更清晰：注册在入口，注销在出口
SELECT 类型事件的兼容方案
用户调用 hooked select 时产生的 APTH_EVENT_TYPE_SELECT 事件比较特殊——它包含一整组 fd_set，而不是单个 fd。

推荐方案：拆解为多个 FD 事件 + 在 apth_syscall_select 中处理

修改 hooked select 的实现，不再使用 APTH_EVENT_TYPE_SELECT，而是为每个关注的 fd 创建一个 APTH_EVENT_TYPE_FD 事件：


// 在 apth_syscall_select 中，替代原来的 apth_event_select：
struct list event_list;
list_init(&event_list);

for (int fd = 0; fd < nfd; fd++) {
    unsigned long goal = APTH_EVENT_MODE_STATIC;
    if (rfds && FD_ISSET(fd, rfds)) goal |= APTH_GOAL_UNTIL_FD_READABLE;
    if (wfds && FD_ISSET(fd, wfds)) goal |= APTH_GOAL_UNTIL_FD_WRITEABLE;
    if (efds && FD_ISSET(fd, efds)) goal |= APTH_GOAL_UNTIL_FD_EXCEPTION;

    if (goal & (APTH_GOAL_UNTIL_FD_READABLE | APTH_GOAL_UNTIL_FD_WRITEABLE | APTH_GOAL_UNTIL_FD_EXCEPTION)) {
        apth_event_t ev = apth_event_fd(goal, fd);
        apth_event_list_add(&event_list, ev);
    }
}
if (timeout != NULL) {
    apth_event_t ev_timeout = apth_event_time(APTH_EVENT_MODE_STATIC,
        apth_timeout(timeout->tv_sec, timeout->tv_usec));
    apth_event_list_add(&event_list, ev_timeout);
}

apth_wait_event_list(&event_list);

// 唤醒后，根据哪些 FD 事件 OCCURRED 来重建 fd_set
if (rfds) FD_ZERO(rfds);
if (wfds) FD_ZERO(wfds);
if (efds) FD_ZERO(efds);
int count = 0;
FOR_ELEMENT_IN_LIST_REF(&event_list, e) {
    apth_event_t ev = apth_event_t_list_entry(e);
    if (ev->ev_type == APTH_EVENT_TYPE_FD && ev->ev_status == APTH_EV_STATUS_OCCURRED) {
        if ((ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE) && rfds) { FD_SET(ev->ev_args.FD.fd, rfds); count++; }
        if ((ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE) && wfds) { FD_SET(ev->ev_args.FD.fd, wfds); count++; }
        if ((ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION) && efds) { FD_SET(ev->ev_args.FD.fd, efds); count++; }
    }
}
// 清理事件
FOR_ELEMENT_IN_LIST_REF(&event_list, e2) {
    apth_event_t ev = apth_event_t_list_entry(e2);
    apth_event_free(ev);
}
return count;
这样 APTH_EVENT_TYPE_SELECT 可以逐步废弃，所有 fd 事件统一用 APTH_EVENT_TYPE_FD 走 epoll 路径。

或者保留 SELECT fallback：如上面阶段 1 代码中所示，对 SELECT 类型事件用 apth_syscall_raw(select) 做 zero-timeout 探测。这种方式改动较小，但效率不如拆解方案。

总结：方案 B 的完整数据流

1. apth 调用 read(fd) → EAGAIN
2. apth_wait_event(apth_event_fd(READABLE, fd))
   → epoll_map_add_waiter(sched, fd, self, ev)
     → 如果 fd 第一次被等待: epoll_ctl(ADD, fd, EPOLLIN)
     → 如果 fd 已有其他等待者: epoll_ctl(MOD, fd, 聚合掩码)
     → waiter 加入 slot->waiters 链表
   → apth 进入 WAITING 状态，yield 到 scheduler

3. scheduler 运行 eventmanager
   → 阶段 1: 遍历 waiting queue，处理 timer/sig/tid/func
     （FD 事件已在 epoll 中，不需处理）
   → 阶段 2: epoll_wait(epoll_fd, events, 64, timeout_ms)
     → fd 就绪，返回 ready_fd + revents
     → epoll_map_wake_fd(sched, ready_fd, revents)
       → 遍历 slot->waiters，匹配 revents vs ev_goal
       → 标记匹配的 event 为 OCCURRED
       → 从 waiters 链表移除
       → 如果 slot 无等待者: epoll_ctl(DEL, fd)
     → 找出有 OCCURRED 事件的 apth，清理其余 epoll 注册
     → transfer_th → waked_queue

4. scheduler 下一轮取出 waked apth → ready_queue → running
5. apth 从 apth_yield 返回 → apth_wait_event 返回
   → list_remove(&ev->elem)
   → 检查 ev->ev_status
   → 继续执行 read(fd) → 成功
复杂度对比：

操作	旧 select 方案	新 epoll + 映射表方案
注册 fd 等待	无（每轮重建 fd_set）	epoll_ctl 一次（增量）
事件检查	O(fdmax × n_waiting)	O(n_ready)
多 apth 等同一 fd	自动支持（fd_set 合并）	映射表查表唤醒全部
系统调用	每轮 1 次 select	每轮 1 次 epoll_wait
锁持有时间	两遍遍历 waiting queue	一遍遍历 + epoll_wait 不持锁
关键优势：epoll_wait 不需要持有 waiting_queue 的锁，因为 fd 注册信息在 epoll 内核侧维护。阶段 2 的 epoll_wait 调用在锁外执行，只有后续的唤醒操作需要短暂加锁。
