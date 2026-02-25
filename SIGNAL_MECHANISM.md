
## 一、当前信号抽象机制的全面问题分析

经过完整阅读所有相关源码，我将当前信号机制的问题分为 **架构缺陷**、**竞态条件**、**缺失功能** 三大类。

---

### 1. 架构缺陷

#### 1.1 `pthread_kill` 信号传递时机不可控（最核心问题）

在 `scheduler_routine` 中（调度 apth 之前）：

```c
if (th->sigpendcnt > 0) {
    sigpending(&sched->apth_sigpending);
    for (int sig = 1; sig < APTH_NSIG; sig++) {
        if (sigismember(&th->sigpending, sig) && !sigismember(&sched->apth_sigpending, sig))
            apth_syscall_raw(pthread_kill)(apth_syscall_raw(pthread_self)(), sig);
    }
}
```

**问题**：`pthread_kill` 向本 worker pthread 发送信号后，信号**不一定**在 `apth_ctx_switch` 切换到 apth 上下文之后才被交付。信号可能在以下任何时刻被 deliver：
- 在 `pthread_kill` 返回后、`apth_ctx_switch` 调用前（此时还在 scheduler 上下文）
- 在 `swapcontext` 内部
- 在 apth 上下文恢复后

如果信号在 scheduler 上下文中被处理，用户注册的 signal handler 将在 scheduler 的栈帧中运行，而非 apth 的栈帧。这会破坏 scheduler 的状态，而且 handler 中如果调用 `apth_yield` 等库函数会导致灾难性后果。

#### 1.2 信号掩码管理混乱

- scheduler 在循环开始时 `sigfillset(&sigs); pthread_sigmask(SIG_SETMASK, &sigs, NULL)` 阻塞了所有信号。
- event manager 中临时解除某些信号阻塞来 catch 信号事件，然后恢复。
- 但在 `apth_ctx_switch` 到 apth 之前/之后，**没有根据该 apth 的 `CTX_SIGMASK_OF(th->ctx)` 设置 pthread 级信号掩码**。这意味着：
  - apth 运行期间 pthread 层面仍然是 `sigfillset` 状态，所有信号都被阻塞
  - apth 的 `apth_sigmask` 只修改了 `ucontext_t.uc_sigmask` 字段，但这个字段的语义在 `swapcontext` 中与直接 `pthread_sigmask` 不同步
  - 实际上**所有 apth 运行期间都不会收到任何信号**（除了通过 `pthread_kill` 显式发送且未被阻塞的）

#### 1.3 event manager 的 `sigaction` 竞态

event manager 中：
```c
for (int sig = 1; sig < APTH_NSIG; sig++) {
    if (sigismember(&sched->apth_sigcatch, sig)) {
        sa.sa_sigaction = apth_sched_eventmanager_sighandler;
        sigaction(sig, &sa, &osa[sig]);
    }
}
```
`sigaction` 是**进程级全局**操作。当有多个 worker pthread 同时运行 event manager 时，它们会互相覆盖彼此的 signal action，导致：
- 保存的 `osa[sig]` 可能是另一个 worker 临时设置的 handler 而非用户的
- 恢复时把错误的 handler 写回去
- `apth_sched_eventmanager_sighandler` 收到信号后写入了某个 scheduler 的 `apth_sigpipe`，但信号可能被另一个 worker 接收

#### 1.4 `apth_sched_eventmanager_sighandler` 中的 `sched` 指针来源不安全

```c
static void apth_sched_eventmanager_sighandler(int sig, siginfo_t *_dummy_info, void *arg)
{
    apth_sched_t sched = (struct apth_perpthr_scheduler *)arg;
```
但 `sigaction` 的 `SA_SIGINFO` handler 的第三个参数是 `ucontext_t *`（保存的上下文），**不是** `sigaction` 调用者传入的自定义数据。这里把 `ucontext_t *` 错误地解释为 `apth_sched_t`，会导致**直接的内存损坏**。这是一个严重 bug。

#### 1.5 `apth_util_sigdelete` 的副作用

```c
APTH_INTERNAL int apth_util_sigdelete(int sig)
```
这个函数通过临时替换 `sigaction`、`sigsuspend` 来"消耗"一个 pending signal。但 `sigaction` 是进程全局的，在多 worker 环境下会覆盖用户设置的 handler。并且 `sigsuspend` 会阻塞当前 pthread 直到信号交付，如果信号恰好在此前被另一个 worker 消耗了，就会无限阻塞。

### 2. 竞态条件

#### 2.1 `th->sigpending` / `th->sigpendcnt` 的非原子访问

`apth_kill` 中：
```c
if (!sigismember(&t->sigpending, sig)) {
    sigaddset(&t->sigpending, sig);
    t->sigpendcnt++;
}
```
如果目标 apth 和发送方在不同 worker 上（跨 scheduler），则对 `sigpending`/`sigpendcnt` 的读写没有任何同步保护。event manager 和 scheduler 也同时读写这些字段。

#### 2.2 scheduler 回来后的信号清理竞态

```c
// Handle signals (after ctx_switch returns)
if (th->sigpendcnt > 0) {
    sigset_t sigstillpending;
    sigpending(&sigstillpending);
    for (int sig = 1; sig < APTH_NSIG; sig++) {
        if (sigismember(&th->sigpending, sig)) {
            if (!sigismember(&sigstillpending, sig)) {
                sigdelset(&th->sigpending, sig);
                th->sigpendcnt--;
            }
```
这里假设如果一个信号不在 `sigpending` 中了就说明已被 apth 处理——但内核的 `sigpending` 只反映 pthread 级别的 pending 状态。标准信号（非 RT）不排队，多次 `pthread_kill` 同一信号只保留一个 pending。因此这个推断不可靠。

#### 2.3 `apth_kill` 中的 `apth_yield` 问题

```c
int apth_kill(apth_t t, int sig)
{
    // ...
    sigaddset(&t->sigpending, sig);
    t->sigpendcnt++;
    apth_yield(); // NOTE: containing a cancelation point
    return 0;
}
```
调用 `apth_yield` 会让当前 apth 交出控制权。如果 `t` 在不同 worker 上，yield 对信号传递没有帮助。如果 `t == cur`（自身发信号），当前代码直接 return EINVAL，不允许自发信号，但 POSIX `pthread_kill(pthread_self(), sig)` 应当是合法的且要求信号在函数返回前被处理。

### 3. 缺失的功能（libc 钩子）

目前以下关键的 POSIX 信号 API 完全没有 hook：

| 函数 | 状态 | 重要性 |
|------|------|--------|
| `sigaction()` | ❌ 未 hook | **关键** — 用户设置信号处理函数 |
| `signal()` | ❌ 未 hook | **关键** — 简化版 sigaction |
| `sigprocmask()` / `pthread_sigmask()` | ❌ 未 hook | **关键** — 用户线程信号掩码 |
| `sigpending()` | ❌ 未 hook | **重要** — 查询 pending 信号 |
| `sigsuspend()` | ❌ 未 hook | **重要** — 原子性地等待信号 |
| `sigtimedwait()` / `sigwaitinfo()` | ❌ 未 hook | 中等 |
| `sigaltstack()` | ❌ 未 hook | 中等 — 信号备用栈 |
| `raise()` | ❌ 未 hook | 中等 |
| `kill()` | ❌ 未 hook（对进程级信号） | 中等 |

---

## 二、完整的信号抽象机制设计

### 设计原则

1. **信号完全虚拟化**：所有信号在 libapth 层面软件模拟，不依赖内核将信号 deliver 到特定 apth 上下文
2. **全局信号 action 表由 libapth 管理**：用户调用 `sigaction`/`signal` 修改的是 libapth 内部表，不直接修改内核的 `sigaction`
3. **信号在 scheduler 调度点检查和分发**：不在任意时刻中断 apth 执行（除非实现了 preemption），而是在每次调度决策时检查 pending 信号
4. **信号处理函数在目标 apth 的栈帧和上下文中运行**

### 架构总览

```
┌────────────────────────────────────────────────────────┐
│                   User Application                      │
│  sigaction() / signal() / pthread_sigmask() / kill()    │
│  sigpending() / sigsuspend() / raise() / sigwait()      │
└──────────────────────┬─────────────────────────────────┘
                       │ LD_PRELOAD hook
┌──────────────────────▼─────────────────────────────────┐
│              libapth Signal Virtualization Layer         │
│                                                         │
│  ┌─────────────────────┐  ┌──────────────────────────┐ │
│  │ Global Signal Action │  │  Per-apth Signal State   │ │
│  │ Table (process-wide) │  │  - sigmask               │ │
│  │ apth_sigaction[NSIG] │  │  - sigpending            │ │
│  │                      │  │  - sigaltstack           │ │
│  │ Protected by rwlock  │  │  - sigpendcnt            │ │
│  └─────────────────────┘  │  Protected by per-apth   │ │
│                            │  spinlock                 │ │
│                            └──────────────────────────┘ │
│                                                         │
│  ┌─────────────────────────────────────────────────────┐│
│  │           Signal Delivery Engine                     ││
│  │  Called at: scheduler dispatch point                  ││
│  │            event manager wakeup                       ││
│  │            apth_yield return                          ││
│  │  Does: check pending & ~masked → run handler          ││
│  │        on apth's stack (or sigaltstack)               ││
│  └─────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────┘
```

### 详细设计

#### A. 新增数据结构

**1. 全局信号 Action 表**

```c
// 进程级全局信号配置表
struct apth_global_sigaction {
    struct sigaction actions[APTH_NSIG];  // 用户注册的 handler
    lll_t lock;                            // 读写保护
};
extern struct apth_global_sigaction APTH_GLOBAL_SIGACTIONS;
```

在 `apth_init` 时初始化：所有 action 设为 `SIG_DFL`。

**2. Per-apth 信号状态扩展（修改 `struct apth_st`）**

```c
struct apth_st {
    // ... existing fields ...

    /* per-thread signal handling (扩展) */
    sigset_t sigpending;          // 已有 — pending 信号集
    int sigpendcnt;               // 已有 — pending 计数
    sigset_t sigmask;             // 新增 — 本 apth 的信号掩码（取代 CTX_SIGMASK_OF）
    lll_t siglock;                // 新增 — 保护信号状态的自旋锁
    stack_t sigaltstack;          // 新增 — 备用信号栈
    bool sigaltstack_set;         // 新增 — 是否设置了 sigaltstack
    volatile bool in_sighandler;  // 新增 — 是否正在执行信号处理函数
};
```

**3. 信号队列（对 RT 信号的扩展，可选）**

对于标准信号（1-31），保持 pending set 语义（不排队）。对于实时信号（SIGRTMIN-SIGRTMAX），如果将来需要，可以增加信号队列。当前可以只实现标准信号语义。

#### B. libc 函数钩子实现

**1. `sigaction` 钩子**

```c
APTH_DEFINE_SYSCALL(int, sigaction,
    (int sig, const struct sigaction *act, struct sigaction *oact),
    (sig, act, oact))
{
    if (sig <= 0 || sig >= APTH_NSIG || sig == SIGKILL || sig == SIGSTOP)
        return apth_error(-1, EINVAL);

    lll_lock(&APTH_GLOBAL_SIGACTIONS.lock, "sigaction");

    // 返回旧 action
    if (oact != NULL)
        *oact = APTH_GLOBAL_SIGACTIONS.actions[sig];

    // 设置新 action
    if (act != NULL)
        APTH_GLOBAL_SIGACTIONS.actions[sig] = *act;

    lll_unlock(&APTH_GLOBAL_SIGACTIONS.lock, "sigaction");
    return 0;
}
```

**注意**：对于 `SIGKILL`、`SIGSTOP`、`SIGSEGV`、`SIGBUS`、`SIGFPE`、`SIGILL` 等硬件/不可忽略信号，应当将 `sigaction` 同时注册到**内核级**（通过 `apth_syscall_raw(sigaction)`），因为这些信号是硬件产生、不可纯软件模拟的。对于这些信号，libapth 在内核级注册一个 **trampoline handler**，在其中设置 apth 的 pending bit，然后从 handler 返回（而非直接执行用户 handler）。

**2. `signal` 钩子**

```c
APTH_DEFINE_SYSCALL(sighandler_t, signal, (int sig, sighandler_t handler), (sig, handler))
{
    struct sigaction act, oact;
    act.sa_handler = handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    if (apth_syscall(sigaction)(sig, &act, &oact) < 0)
        return SIG_ERR;
    return oact.sa_handler;
}
```

**3. `pthread_sigmask` / `sigprocmask` 钩子**

```c
APTH_DEFINE_SYSCALL(int, pthread_sigmask,
    (int how, const sigset_t *set, sigset_t *oset),
    (how, set, oset))
{
    // 重定向到 apth_sigmask
    return apth_sigmask(how, set, oset);
}

APTH_DEFINE_SYSCALL(int, sigprocmask,
    (int how, const sigset_t *set, sigset_t *oset),
    (how, set, oset))
{
    return apth_sigmask(how, set, oset);
}
```

同时修改 `apth_sigmask` 使用新的 `th->sigmask` 而非 `CTX_SIGMASK_OF`，并在修改完掩码后检查是否有新的可递送信号（unmasked pending）。

**4. `sigpending` 钩子**

```c
APTH_DEFINE_SYSCALL(int, sigpending, (sigset_t *set), (set))
{
    if (set == NULL) return apth_error(-1, EFAULT);
    apth_t cur = cur_apth();
    if (APTH_IS_FAKE_SCHED(cur))
        return apth_syscall_raw(sigpending)(set);

    lll_lock(&cur->siglock, "sigpending");
    *set = cur->sigpending;
    lll_unlock(&cur->siglock, "sigpending");
    return 0;
}
```

**5. `sigsuspend` 钩子**

```c
APTH_DEFINE_SYSCALL(int, sigsuspend, (const sigset_t *mask), (mask))
{
    apth_t self = cur_apth();
    // 临时替换信号掩码
    sigset_t oldmask = self->sigmask;
    self->sigmask = *mask;

    // 等待一个未被 mask 的信号到达
    // 构造一个 custom event 来检查
    apth_event_t ev = apth_event_func(
        APTH_EVENT_MODE_STATIC,
        __apth_sigsuspend_check, self,
        apth_time(0, 50000) // 50ms 轮询间隔
    );
    apth_wait_event(ev);

    // 恢复信号掩码
    self->sigmask = oldmask;
    // 递送信号
    __apth_deliver_pending_signals(self);
    errno = EINTR;
    return -1;
}
```

**6. `raise` 钩子**

```c
APTH_DEFINE_SYSCALL(int, raise, (int sig), (sig))
{
    apth_t self = cur_apth();
    if (APTH_IS_FAKE_SCHED(self))
        return apth_syscall_raw(raise)(sig);
    return apth_kill(self, sig);  // 需要先修复 apth_kill 允许自发信号
}
```

**7. `sigaltstack` 钩子**

```c
APTH_DEFINE_SYSCALL(int, sigaltstack,
    (const stack_t *ss, stack_t *oss),
    (ss, oss))
{
    apth_t cur = cur_apth();
    if (oss != NULL) {
        if (cur->sigaltstack_set)
            *oss = cur->sigaltstack;
        else {
            oss->ss_sp = NULL;
            oss->ss_size = 0;
            oss->ss_flags = SS_DISABLE;
        }
    }
    if (ss != NULL) {
        if (ss->ss_flags & SS_DISABLE) {
            cur->sigaltstack_set = false;
        } else {
            cur->sigaltstack = *ss;
            cur->sigaltstack_set = true;
        }
    }
    return 0;
}
```

#### C. 信号递送引擎（核心）

**设计关键**：信号不通过 `pthread_kill` 交付，而是在 scheduler 将控制权切给 apth 之前（或 apth 从 yield 返回时），通过**软件方式**检查和执行 signal handler。

```c
// 在 scheduler_routine 中，在 apth_ctx_switch 之前调用
APTH_INTERNAL void apth_deliver_pending_signals(apth_t th)
{
    // 取出 pending & ~sigmask 中的信号
    for (int sig = 1; sig < APTH_NSIG; sig++) {
        if (!sigismember(&th->sigpending, sig))
            continue;
        if (sigismember(&th->sigmask, sig))
            continue;  // 被掩码阻塞，跳过

        // 从 pending 中移除
        sigdelset(&th->sigpending, sig);
        th->sigpendcnt--;

        // 查找 handler
        struct sigaction sa = APTH_GLOBAL_SIGACTIONS.actions[sig];

        if (sa.sa_handler == SIG_IGN)
            continue;
        if (sa.sa_handler == SIG_DFL) {
            // 执行默认行为
            __apth_sig_default_action(th, sig);
            continue;
        }

        // 有用户 handler：需要在 apth 的上下文中执行
        // 方法：修改 apth 的 ucontext，让它先跳转到一个 trampoline，
        // trampoline 调用用户 handler 后返回到原来的 PC
        __apth_inject_signal_handler(th, sig, &sa);
    }
}
```

**`__apth_inject_signal_handler` 的实现策略**：

有两种方式：

**方案 A（推荐）：在切换到 apth 上下文之前，在 scheduler 中直接调用 handler**

这种方式最简单：scheduler 在切换到 apth 之前，暂时"借用" apth 的身份：
```c
static void __apth_inject_signal_handler(apth_t th, int sig, struct sigaction *sa)
{
    // 在调度到 th 之前，以 th 的身份执行 handler
    // 此时 cur_apth() 已经被设为 th
    // handler 在 scheduler 的栈上运行，但逻辑上属于 th

    // 临时阻塞 sa->sa_mask 指定的信号
    sigset_t old_mask = th->sigmask;
    for (int s = 1; s < APTH_NSIG; s++)
        if (sigismember(&sa->sa_mask, s))
            sigaddset(&th->sigmask, s);
    // 如果 SA_NODEFER 未设置，也阻塞当前信号
    if (!(sa->sa_flags & SA_NODEFER))
        sigaddset(&th->sigmask, sig);

    th->in_sighandler = true;
    if (sa->sa_flags & SA_SIGINFO)
        sa->sa_sigaction(sig, NULL, NULL);
    else
        sa->sa_handler(sig);
    th->in_sighandler = false;

    th->sigmask = old_mask;

    // 如果 SA_RESETHAND，恢复为默认
    if (sa->sa_flags & SA_RESETHAND) {
        struct sigaction dfl = { .sa_handler = SIG_DFL };
        APTH_GLOBAL_SIGACTIONS.actions[sig] = dfl;
    }
}
```

**方案 B（更精确但复杂）：修改 apth 的 ucontext 使其先执行 handler**

通过操纵 `th->ctx->uc` 的寄存器（PC/SP），让 apth 恢复执行时先进入一个 trampoline：
```c
// 保存原 PC
// 修改 uc 的 instruction pointer 指向 signal_trampoline
// signal_trampoline 调用 handler(sig)，然后跳回原 PC
```
这更接近内核的做法，但需要平台相关的寄存器操作代码。

**推荐方案 A**，因为在协作式调度模型中，信号只在调度点递送，在 scheduler 上下文中调用 handler 是安全的——只要确保 handler 中可以调用 apth API（如 `apth_sigmask`）且不会破坏 scheduler 状态。为此，需要在调用 handler 前将 `cur_apth` 设为 `th`。

#### D. 修改 scheduler_routine

```c
// 在 scheduler_routine 的主循环中，替换现有的信号处理代码：

// 旧代码（删除）：
// if (th->sigpendcnt > 0) { pthread_kill(...) ... }

// 新代码：
set_cur_apth(th);
if (th->sigpendcnt > 0)
    apth_deliver_pending_signals(th);

// 然后正常 ctx_switch
apth_ctx_switch(sched->sched_ctx, th->ctx);

// 返回后的信号清理代码也需要删除旧的 pthread_kill 相关逻辑
```

#### E. 修改 apth_kill

```c
int apth_kill(apth_t t, int sig)
{
    if (t == NULL || sig < 0 || sig >= APTH_NSIG)
        return EINVAL;

    if (sig == 0)
        return apth_apth_exists(t) ? 0 : ESRCH;

    // 检查全局 action
    struct sigaction sa = APTH_GLOBAL_SIGACTIONS.actions[sig];
    if (sa.sa_handler == SIG_IGN)
        return 0;

    // 原子地添加到目标 apth 的 pending 集合
    lll_lock(&t->siglock, "apth_kill");
    if (!sigismember(&t->sigpending, sig)) {
        sigaddset(&t->sigpending, sig);
        t->sigpendcnt++;
    }
    lll_unlock(&t->siglock, "apth_kill");

    // 允许自发信号：如果 t == self，立即检查递送
    apth_t self = cur_apth();
    if (t == self && !APTH_IS_FAKE_SCHED(self)) {
        apth_deliver_pending_signals(self);
    }
    // 如果目标在 waiting 队列中且信号未被掩码，可以唤醒它
    // （通过设置一个标志让 event manager 将其移到 waked 队列）

    return 0;
}
```

#### F. 修改 event manager

1. **删除** `apth_sched_eventmanager_sighandler` 和相关的 `sigaction` 临时替换逻辑
2. 对于 `APTH_EVENT_TYPE_SIGS` 事件，只检查 apth 级别的 `sigpending`，不再操作 pthread 级别的信号
3. 删除 `apth_sigpipe` 机制（不再需要，因为不通过内核信号唤醒 select）
4. 如果进程收到了一个需要分发给 apth 的信号（如 `SIGINT`），通过一个全局的内核级 trampoline handler 将其设置到某个合适的 apth 的 pending 中

#### G. 进程级信号处理

对于外部信号（如用户按 Ctrl-C 产生的 `SIGINT`、`kill` 命令发送的信号），需要：

1. 在 `apth_init` 时为需要处理的信号注册一个**内核级 catch-all handler**：
```c
static void apth_kernel_signal_catcher(int sig, siginfo_t *info, void *uctx)
{
    // 将信号路由到某个 apth
    // 策略：找到第一个不阻塞此信号的 apth，设置其 pending bit
    // 如果所有 apth 都阻塞了此信号，设为进程级 pending
    (void)info; (void)uctx;
    apth_route_process_signal(sig);
}
```

2. `apth_route_process_signal` 遍历所有 scheduler 的所有 apth，找到第一个未屏蔽该信号的 apth，设置其 pending。这模拟了 POSIX 中进程级信号路由到未阻塞该信号的线程的行为。

#### H. `sigwait` 的修改

当前的 `sigwait` 实现大致正确，但需要修改为只查看 apth 级别的 pending：

```c
APTH_DEFINE_SYSCALL(int, sigwait, (const sigset_t *set, int *sigp), (set, sigp))
{
    apth_t self = cur_apth();
    if (set == NULL || sigp == NULL)
        return EINVAL;

    // 检查是否有信号已经 pending
    lll_lock(&self->siglock, "sigwait");
    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        if (sigismember(set, sig) && sigismember(&self->sigpending, sig))
        {
            sigdelset(&self->sigpending, sig);
            self->sigpendcnt--;
            lll_unlock(&self->siglock, "sigwait");
            *sigp = sig;
            return 0;
        }
    }
    lll_unlock(&self->siglock, "sigwait");

    // 没有信号已经 pending，需要等待
    // 创建一个 APTH_EVENT_TYPE_SIGS 事件并等待
    apth_event_t ev = apth_event_sigs(APTH_EVENT_MODE_STATIC, set, sigp);
    if (ev == NULL)
        return errno;
    apth_wait_event(ev);

    // 当事件被 event manager 标记为 OCCURRED 时，*sigp 已经被设置好了
    apth_event_free(ev);
    return 0;
}
```

**关键变更**：
- 不再调用内核的 `sigpending()`，只查看 `self->sigpending`（apth 虚拟级别的 pending）
- 不再调用 `apth_util_sigdelete()`（不再需要从内核消耗信号）
- event manager 中对 `APTH_EVENT_TYPE_SIGS` 的处理也简化为只检查 `th->sigpending`

#### I. 修改 event manager 中的 SIGS 事件处理

event manager 的 `__first_loop` 中关于 `APTH_EVENT_TYPE_SIGS` 的处理需要简化——只需检查 apth 的虚拟 `sigpending`，不再涉及任何 pthread 级信号操作：

```c
case APTH_EVENT_TYPE_SIGS:
    // Signal Set — 只查看 apth 级别的 sigpending
    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        if (sigismember(event->ev_args.SIGS.sigs, sig))
        {
            // 检查 apth 的 sigpending（而非 pthread 的）
            lll_lock(&th->siglock, "event_sigs");
            if (sigismember(&th->sigpending, sig))
            {
                // 信号匹配：从 pending 中移除，标记事件为 occurred
                if (event->ev_args.SIGS.sig != NULL)
                    *(event->ev_args.SIGS.sig) = sig;
                sigdelset(&th->sigpending, sig);
                th->sigpendcnt--;
                lll_unlock(&th->siglock, "event_sigs");
                this_ev_occurred = true;
                break; // 一个信号匹配即可
            }
            lll_unlock(&th->siglock, "event_sigs");
        }
    }
    // 不再操作 sched->apth_sigblock / apth_sigcatch
    break;
```

`__second_loop` 中的 `APTH_EVENT_TYPE_SIGS` 分支也要相应简化——不再检查 `sched->apth_sigraised`，因为进程级信号已经在内核级 catch-all handler 中被路由到了某个 apth 的 `sigpending` 中：

```c
case APTH_EVENT_TYPE_SIGS:
    // 再次检查 apth 的 sigpending（可能在第一轮和第二轮之间有新信号到达）
    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        if (sigismember(event->ev_args.SIGS.sigs, sig))
        {
            lll_lock(&th->siglock, "event_sigs_2nd");
            if (sigismember(&th->sigpending, sig))
            {
                if (event->ev_args.SIGS.sig != NULL)
                    *(event->ev_args.SIGS.sig) = sig;
                sigdelset(&th->sigpending, sig);
                th->sigpendcnt--;
                lll_unlock(&th->siglock, "event_sigs_2nd");
                event->ev_status = APTH_EV_STATUS_OCCURRED;
                break;
            }
            lll_unlock(&th->siglock, "event_sigs_2nd");
        }
    }
    break;
```

#### J. 删除不再需要的基础设施

新设计完成后，以下组件应当**删除或废弃**：

| 组件 | 原因 |
|------|------|
| `sched->apth_sigpipe[2]` | 不再通过内核信号唤醒 select |
| `sched->apth_sigpending` | 不再读取 pthread 级 sigpending |
| `sched->apth_sigblock` | 不再操作 pthread 级信号掩码来 catch 信号 |
| `sched->apth_sigcatch` | 同上 |
| `sched->apth_sigraised` | 同上 |
| `apth_sched_eventmanager_sighandler()` | 不再用内核 sigaction 临时 catch 信号 |
| `apth_util_sigdelete()` | 不再需要从内核消耗 pending signal |
| `CTX_SIGMASK_OF(ctx)` 宏 | 信号掩码改为存储在 `th->sigmask` 中 |
| scheduler 中 `pthread_kill` 信号投递逻辑 | 改为软件递送 |
| scheduler 中 ctx_switch 返回后的信号清理逻辑 | 同上 |

#### K. `SIG_DFL` 默认行为的实现

```c
static void __apth_sig_default_action(apth_t th, int sig)
{
    switch (sig)
    {
    // 终止进程的信号
    case SIGTERM:
    case SIGINT:
    case SIGHUP:
    case SIGPIPE:
    case SIGALRM:
    case SIGUSR1:
    case SIGUSR2:
    case SIGPROF:
    case SIGVTALRM:
        // 默认行为：终止整个进程
        // 恢复内核默认处理并重新发送信号
        {
            struct sigaction dfl = { .sa_handler = SIG_DFL };
            apth_syscall_raw(sigaction)(sig, &dfl, NULL);
            apth_syscall_raw(raise)(sig);
        }
        break;

    // 生成 core dump 的信号
    case SIGQUIT:
    case SIGABRT:
    case SIGSEGV:
    case SIGBUS:
    case SIGFPE:
    case SIGILL:
    case SIGSYS:
    case SIGTRAP:
    case SIGXCPU:
    case SIGXFSZ:
        {
            struct sigaction dfl = { .sa_handler = SIG_DFL };
            apth_syscall_raw(sigaction)(sig, &dfl, NULL);
            apth_syscall_raw(raise)(sig);
        }
        break;

    // 忽略的信号
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
        // 默认行为是忽略
        break;

    // 停止信号
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        // 停止进程
        apth_syscall_raw(raise)(sig);
        break;

    // 继续信号
    case SIGCONT:
        // 如果进程被停止则继续，否则忽略
        break;

    default:
        break;
    }
}
```

#### L. 进程级信号路由的详细实现

```c
// 进程级 pending 信号（当所有 apth 都阻塞了某信号时暂存）
static sigset_t APTH_PROCESS_SIGPENDING;
static lll_t APTH_PROCESS_SIGPENDING_LOCK;

// 在内核级 signal handler 中调用（signal-safe context）
// 注意：此函数在信号处理上下文中运行，只能使用 async-signal-safe 操作
static void apth_route_process_signal(int sig)
{
    // 策略：遍历所有 worker 的 scheduler，找到第一个有 apth 未阻塞此信号的
    // 如果找不到，存入进程级 pending

    // 注意：在 signal handler 中不能使用 lll_lock（可能死锁）
    // 使用 atomic 操作或 sigatomic_t 来安全地设置 pending bit

    // 简单策略：设置到进程级 pending，让各 scheduler 在调度点去检查
    sigaddset(&APTH_PROCESS_SIGPENDING, sig);

    // 可选：写入某个 eventfd / pipe 来唤醒正在 select() 中阻塞的 scheduler
    // 但需要注意 signal-safe 约束（write 是 signal-safe 的）
}

// 在 scheduler 调度点检查进程级 pending 信号
APTH_INTERNAL void apth_check_process_signals(apth_sched_t sched)
{
    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        if (!sigismember(&APTH_PROCESS_SIGPENDING, sig))
            continue;

        // 尝试将此信号分配给本 scheduler 上某个未阻塞该信号的 apth
        // 遍历 ready queue, waked queue, new queue, running(cur) 中的 apth
        apth_t target = NULL;

        // 首先检查当前将要调度的 apth
        if (sched->cur != NULL && !APTH_IS_FAKE_SCHED(sched->cur))
        {
            if (!sigismember(&sched->cur->sigmask, sig))
                target = sched->cur;
        }

        // 如果没找到，检查 ready queue 中的（需要遍历）
        // ...（可以使用 visit_thqueue 的简化版本）

        if (target != NULL)
        {
            // 原子地从进程 pending 移除，加入到 apth pending
            sigdelset(&APTH_PROCESS_SIGPENDING, sig);
            lll_lock(&target->siglock, "route_sig");
            if (!sigismember(&target->sigpending, sig))
            {
                sigaddset(&target->sigpending, sig);
                target->sigpendcnt++;
            }
            lll_unlock(&target->siglock, "route_sig");
        }
        // 如果本 scheduler 没有合适的 apth，留给其他 scheduler 处理
    }
}
```

此函数应在 `scheduler_routine` 主循环中、移动 new/waked 到 ready 队列之后、选择下一个 apth 之前调用。

#### M. apth_sigmask 的修改

原来的 `apth_sigmask` 操作 `CTX_SIGMASK_OF(cur->ctx)` 即 `ucontext_t.uc_sigmask`，需要改为操作新的 `th->sigmask` 字段：

```c
int apth_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    apth_t cur = cur_apth();

    // 如果是 scheduler 上下文，直接操作 pthread 级掩码
    if (APTH_IS_FAKE_SCHED(cur))
        return apth_syscall_raw(pthread_sigmask)(how, set, oldset);

    if (oldset != NULL)
        *oldset = cur->sigmask;

    if (set == NULL)
        return 0;

    switch (how)
    {
    case SIG_BLOCK:
        for (int sig = 1; sig < APTH_NSIG; sig++)
            if (sigismember(set, sig))
                sigaddset(&cur->sigmask, sig);
        break;
    case SIG_UNBLOCK:
        for (int sig = 1; sig < APTH_NSIG; sig++)
            if (sigismember(set, sig))
                sigdelset(&cur->sigmask, sig);
        break;
    case SIG_SETMASK:
        cur->sigmask = *set;
        break;
    default:
        return EINVAL;
    }

    // SIGKILL 和 SIGSTOP 不能被阻塞
    sigdelset(&cur->sigmask, SIGKILL);
    sigdelset(&cur->sigmask, SIGSTOP);

    // 修改掩码后，检查是否有新的可递送信号
    // （解除阻塞某个信号后，该信号可能已经 pending）
    if (how == SIG_UNBLOCK || how == SIG_SETMASK)
    {
        if (cur->sigpendcnt > 0)
            apth_deliver_pending_signals(cur);
    }

    return 0;
}
```

#### N. apth_create 的修改

在创建新 apth 时，需要初始化新的信号相关字段：

```c
// 在 apth_create 中，初始化信号字段处添加：

// 信号
sigemptyset(&t->sigpending);
t->sigpendcnt = 0;
lll_init(&t->siglock);
t->in_sighandler = false;
t->sigaltstack_set = false;

// 信号掩码：继承创建者的掩码，或使用 attr 中指定的
if (iattr->sigmask_set)
    t->sigmask = iattr->sigmask;
else
{
    apth_t creator = cur_apth();
    if (!APTH_IS_FAKE_SCHED(creator))
        t->sigmask = creator->sigmask;
    else
        sigemptyset(&t->sigmask);
}
```

---

## 三、实现优先级建议

按照依赖关系和影响程度，推荐的实现顺序：

### 第一阶段：核心基础设施
1. **添加全局信号 Action 表** `APTH_GLOBAL_SIGACTIONS` 和初始化代码
2. **添加 `th->sigmask`、`th->siglock`** 等新字段到 TCB
3. **实现 `apth_deliver_pending_signals()`** 信号递送引擎
4. **修改 `scheduler_routine`**：删除 `pthread_kill` 逻辑，改用新递送引擎
5. **修改 `apth_sigmask`**：使用 `th->sigmask`
6. **修改 `apth_kill`**：添加 `siglock` 保护，允许自发信号
7. **修改 event manager**：简化 SIGS 事件处理，删除 pthread 级信号操作

### 第二阶段：libc 钩子
8. **Hook `sigaction()`** — 最关键的钩子
9. **Hook `signal()`** — 基于 sigaction 钩子
10. **Hook `pthread_sigmask()` / `sigprocmask()`** — 重定向到 apth_sigmask
11. **Hook `sigpending()`** — 读取 apth 级 pending
12. **修改 `sigwait()` 钩子** — 使用纯 apth 级别逻辑

### 第三阶段：完善功能
13. **Hook `sigsuspend()`**
14. **Hook `raise()`**
15. **Hook `sigaltstack()`**
16. **实现进程级信号路由** (`apth_route_process_signal`)
17. **实现 `SIG_DFL` 默认行为**

### 第四阶段：清理
18. **删除废弃组件**：`apth_sigpipe`、`apth_sigblock`、`apth_sigcatch`、`apth_sigraised`、`apth_util_sigdelete` 等
19. **更新 `apth_scheduler_init`**：移除信号管道初始化
20. **更新 `apth_scheduler_kill`**：移除信号管道清理

---

## 四、设计决策总结

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 信号存储位置 | apth TCB 中（`sigmask`, `sigpending`） | 每个 apth 独立的信号状态 |
| 信号掩码存储 | `th->sigmask`（独立字段） | `ucontext_t.uc_sigmask` 语义不清且与 swapcontext 交互复杂 |
| handler 注册表 | 进程全局表 + lll 保护 | POSIX 语义要求 sigaction 是进程级 |
| 信号递送时机 | 调度点（ctx_switch 前/yield 返回后） | 协作式调度天然的递送点，无需 preemption |
| handler 执行位置 | scheduler 栈上、以 apth 身份 | 方案 A：简单可靠，适合协作式模型 |
| 进程外部信号 | 内核级 catch-all handler → 路由到 apth pending | 必须处理 Ctrl-C 等场景 |
| 跨 worker 信号 | `siglock` 保护 + 原子写入 pending | 解决竞态条件 |
| `apth_util_sigdelete` | 废弃 | 不再需要从内核消耗信号 |
| `apth_sigpipe` | 废弃 | 不再需要唤醒 select |
