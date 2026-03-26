# 活锁风险缓解方案性能分析

## 背景

在 libapth 的 low level lock (lll) 实现中，当锁竞争激烈时，可能存在活锁风险。当前实现使用 `sched_yield()` (对于 scheduler) 和 `apth_yield()` (对于 apth) 来处理锁竞争。

## 当前实现分析

### Scheduler 使用 sched_yield()
- **场景**：scheduler (pthread) 在等待锁时
- **原因**：整个 scheduler 无法向前进行，必须让出 CPU
- **问题**：在高竞争下可能导致活锁，因为 `sched_yield()` 不保证让出 CPU 给持有锁的线程

### Apth 使用 apth_yield()
- **场景**：apth (用户态线程) 在等待锁时
- **优势**：可以在用户态切换到其他 apth，避免内核调度开销
- **TODO**：未来将加入提醒 owner apth 的 scheduler 尽快调度该 apth 的机制

## 可选缓解方案及性能代价

### 方案 1: 指数退避 (Exponential Backoff)

#### 实现概述
```c
int backoff_us = 1;  // 初始退避时间（微秒）
const int MAX_BACKOFF_US = 1000;  // 最大退避时间 1ms

while (/* lock acquisition failed */) {
    usleep(backoff_us);
    backoff_us = (backoff_us * 2 < MAX_BACKOFF_US) ? backoff_us * 2 : MAX_BACKOFF_US;
    // 重试获取锁
}
```

#### 性能代价分析

**优点：**
- ✅ 减少 CPU 自旋浪费
- ✅ 在中等竞争下表现良好
- ✅ 实现简单，不需要额外的数据结构

**缺点：**
- ❌ **延迟增加**：在低竞争场景下，即使锁很快释放，也必须等待退避时间
  - 首次失败后至少等待 1μs
  - 若持续失败，等待时间指数增长（1, 2, 4, 8, 16...μs）
- ❌ **不公平性**：后到达的线程可能在退避较少的状态下获得锁
- ❌ **需要系统调用**：`usleep()` 是系统调用，开销约 1-5μs

**适用场景：**
- 中等到高竞争的场景
- 对延迟不敏感的应用
- 锁持有时间较长的场景（> 10μs）

**性能影响估计：**
- 低竞争：增加 **1-10μs** 延迟（不必要的退避）
- 中等竞争：**降低 30-50% CPU 使用**，延迟增加 5-20μs
- 高竞争：**降低 60-80% CPU 使用**，但可能增加 50-100μs 延迟

---

### 方案 2: 自旋计数器 + 重量级同步原语

#### 实现概述
```c
#define SPIN_THRESHOLD 1000

int spin_count = 0;
while (/* lock acquisition failed */) {
    if (spin_count < SPIN_THRESHOLD) {
        // 自旋
        spin_count++;
        // CPU pause instruction (x86: _mm_pause())
        for (int i = 0; i < 10; i++) {
            __asm__ __volatile__("pause" ::: "memory");
        }
    } else {
        // 超过阈值，使用 pthread_mutex 或条件变量等待
        pthread_mutex_lock(&fallback_mutex);
        pthread_cond_wait(&fallback_cond, &fallback_mutex);
        pthread_mutex_unlock(&fallback_mutex);
        spin_count = 0;  // 重置计数器
    }
}
```

#### 性能代价分析

**优点：**
- ✅ 低竞争下保持高性能（纯自旋）
- ✅ 高竞争下避免 CPU 浪费（切换到重量级同步）
- ✅ 自适应：根据竞争程度动态调整策略

**缺点：**
- ❌ **复杂性增加**：
  - 需要额外的 mutex 和条件变量
  - 需要在 unlock 时通知等待者（broadcast/signal）
  - 可能引入新的 bug
- ❌ **内存开销**：每个 lll 需要额外的 mutex + cond (约 80-100 字节)
- ❌ **切换开销**：从自旋切换到 pthread_cond_wait 的决策需要时间
- ❌ **唤醒延迟**：pthread_cond_signal 可能需要 1-10μs 才能唤醒等待线程

**适用场景：**
- 竞争程度高度变化的场景
- 锁持有时间差异很大的场景
- 对内存开销不敏感的应用

**性能影响估计：**
- 低竞争：**几乎无影响**（纯自旋，< 0.1μs）
- 中等竞争：**略微增加延迟** 2-5μs（决策开销）
- 高竞争：**显著降低 CPU 使用**（70-90%），但增加 10-50μs 延迟

**实现挑战：**
- 🚨 **违背 LLL 设计初衷**：引入 pthread 同步原语违背了轻量级锁的设计目标
- 🚨 **竞态条件**：unlock 时的通知和 lock 时的等待之间可能存在竞态
- 🚨 **死锁风险**：如果 fallback_mutex 管理不当可能引入死锁

---

### 方案 3: Futex (Fast Userspace Mutex)

#### 实现概述
```c
#include <linux/futex.h>
#include <sys/syscall.h>

static int futex_wait(int *futex_addr, int expected_val) {
    return syscall(SYS_futex, futex_addr, FUTEX_WAIT_PRIVATE, expected_val, NULL, NULL, 0);
}

static int futex_wake(int *futex_addr, int num_to_wake) {
    return syscall(SYS_futex, futex_addr, FUTEX_WAKE_PRIVATE, num_to_wake, NULL, NULL, 0);
}

// 在 lll_lock 中：
int spin_count = 0;
while (/* lock acquisition failed */) {
    if (spin_count < SPIN_THRESHOLD) {
        spin_count++;
        // 自旋
    } else {
        // 使用 futex 等待
        futex_wait((int*)&lock->inner, oldval);
        spin_count = 0;
    }
}

// 在 lll_unlock 中：
atomic_store_release(&lock->inner, LLL_NOT_ACQUIRED);
futex_wake((int*)&lock->inner, 1);  // 唤醒一个等待者
```

#### 性能代价分析

**优点：**
- ✅ **高效**：futex 在用户态快速路径下几乎零开销
- ✅ **公平性**：内核管理等待队列，提供 FIFO 保证
- ✅ **无额外内存**：不需要额外的 mutex/cond 结构
- ✅ **系统级支持**：Linux 内核优化过，性能可靠

**缺点：**
- ❌ **平台依赖**：仅 Linux 支持（不可移植）
- ❌ **系统调用开销**：
  - futex_wait: 约 0.5-2μs（如果需要进入内核）
  - futex_wake: 约 0.3-1μs
- ❌ **语义复杂**：
  - 需要仔细处理虚假唤醒
  - 需要处理信号中断（EINTR）
- ❌ **与指针标记冲突**：
  - 当前 lll 使用指针值 + 标记位存储状态
  - futex 期望简单的整数值，可能需要重新设计

**适用场景：**
- 仅部署在 Linux 环境
- 高竞争场景
- 需要公平性保证的场景

**性能影响估计：**
- 低竞争：**几乎无影响**（快速路径，< 0.1μs）
- 中等竞争：**降低 40-60% CPU 使用**，增加 1-3μs 延迟
- 高竞争：**降低 70-90% CPU 使用**，增加 5-15μs 延迟

**实现挑战：**
- 🚨 **与现有设计冲突**：lll 当前使用指针+标记位，需要重新设计以适配 futex
- 🚨 **ROB 机制兼容性**：ROB 标记如何与 futex 等待队列交互需要仔细设计
- 🚨 **可移植性丧失**：失去在非 Linux 平台运行的能力

---

## 综合建议

### 短期方案（推荐）
**指数退避 + 优化**
- 使用**适应性自旋**：先自旋几轮，再进入指数退避
- 保持 TODO 中计划的"通知 owner scheduler 尽快调度"机制
- 在 apth_yield() 中实现优先级提升机制

```c
// 伪代码
#define INITIAL_SPIN_ROUNDS 100
#define MAX_BACKOFF_US 100  // 较小的最大退避时间

int spin = 0;
int backoff = 0;
while (!try_acquire_lock()) {
    if (spin < INITIAL_SPIN_ROUNDS) {
        spin++;
        __asm__ __volatile__("pause" ::: "memory");
    } else if (backoff < MAX_BACKOFF_US) {
        backoff = (backoff == 0) ? 1 : backoff * 2;
        usleep(backoff);
    } else {
        // TODO: 实现通知机制
        notify_owner_scheduler();
        sched_yield();
    }
}
```

**优点：**
- 简单易实现
- 低竞争下几乎无影响
- 高竞争下显著降低 CPU 使用

**缺点：**
- 中等竞争下延迟略微增加

---

### 长期方案（如果竞争成为瓶颈）
**考虑 Futex（仅限 Linux 部署）**
- 如果性能分析显示锁竞争是主要瓶颈
- 如果应用仅需要在 Linux 上运行
- 需要重新设计 lll 以适配 futex 语义

---

## 性能测试建议

在实施任何方案前，建议进行以下测试：

1. **基准测试**：测量当前实现在不同竞争级别下的性能
   - 低竞争：2-4 个线程竞争
   - 中等竞争：8-16 个线程竞争
   - 高竞争：32+ 个线程竞争

2. **关键指标**：
   - 锁获取延迟（平均、P50、P99）
   - CPU 使用率
   - 吞吐量（操作/秒）
   - 尾延迟（P999）

3. **真实负载测试**：使用实际应用场景进行测试

---

## 结论

| 方案 | 复杂度 | 低竞争性能 | 高竞争性能 | 可移植性 | 推荐度 |
|------|--------|-----------|-----------|---------|--------|
| 指数退避 | ⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐ | ✅ | ⭐⭐⭐⭐ |
| 自旋计数+重量级 | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ✅ | ⭐⭐⭐ |
| Futex | ⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ❌ | ⭐⭐ |

**综合推荐**：优先实现**指数退避**方案，并结合即将实现的"通知 owner scheduler"机制。这提供了最佳的复杂度/性能权衡。
