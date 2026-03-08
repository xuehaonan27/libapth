Comprehensive I/O Performance Optimization Plan for LIBAPTH

  Based on my analysis, I'll provide a detailed, actionable plan combining all the performance improvements. I'll organize this by priority and provide
  specific code changes.

  Overview of Performance Issues

  The current implementation has several critical bottlenecks:

  1. Memory allocation in hot path - malloc/free on every FD event registration
  2. Excessive fcntl syscalls - 2 fcntl calls per I/O operation via acquire/release
  3. Redundant aggregate mask recalculation - O(n) iteration on every waiter removal
  4. Excessive epoll_ctl syscalls - Called on every mask change
  5. Double traversal of waiting queue - Registration and waking in separate passes
  6. Refcount overhead - Atomic operations on every I/O call

  Phase 1: Eliminate FD Refcount Mechanism (Highest Impact)

  Step 1.1: Hook fcntl() to preserve O_NONBLOCK

  Create new file: src/hook_libc/hook_lowlevel_io/fcntl.c

  #include "hook_libc/hook_lowlevel_io.h"
  #include "internal/apth_fd.h"
  #include "utils/apth_errno.h"
  #include <stdarg.h>
  #include <fcntl.h>
  #include <sys/types.h>

  APTH_FETCH_LIBCFUNC(fcntl)

  APTH_INTERNAL int apth_func(fcntl)(int fd, int cmd, ...)
  {
      apth_hook_debug(fcntl);

      va_list args;
      va_start(args, cmd);

      // Special handling for F_SETFL to preserve O_NONBLOCK on managed FDs
      if (cmd == F_SETFL) {
          int flags = va_arg(args, int);

          // If this FD is managed by LIBAPTH, force O_NONBLOCK
          if (fd >= 0 && fd < APTH_FD_TABLE_SIZE &&
              atomic_load_acquire(&APTH_FD_TABLE[fd].managed)) {
              // Silently add O_NONBLOCK to whatever flags user requested
              flags |= O_NONBLOCK;
              apth_debug("fcntl F_SETFL on managed fd=%d, forcing O_NONBLOCK", fd);
          }

          va_end(args);
          return apth_func_raw(fcntl)(fd, cmd, flags);
      }

      // For F_DUPFD and F_DUPFD_CLOEXEC, we need to register the new FD
      if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
          int arg = va_arg(args, int);
          int new_fd = apth_func_raw(fcntl)(fd, cmd, arg);
          va_end(args);

          if (new_fd >= 0 && fd >= 0 && fd < APTH_FD_TABLE_SIZE &&
              atomic_load_acquire(&APTH_FD_TABLE[fd].managed)) {
              // Original FD was managed, so manage the duplicate too
              apth_fd_register(new_fd);
          }
          return new_fd;
      }

      // Handle other fcntl commands based on their argument types
      int result;
      switch (cmd) {
          // Commands taking int argument
          case F_SETFD:
          case F_SETOWN:
          case F_SETSIG:
          case F_SETLEASE:
          case F_NOTIFY:
          case F_SETPIPE_SZ:
  #ifdef F_ADD_SEALS
          case F_ADD_SEALS:
  #endif
          {
              int arg = va_arg(args, int);
              result = apth_func_raw(fcntl)(fd, cmd, arg);
              break;
          }

          // Commands taking struct flock* argument
          case F_SETLK:
          case F_SETLKW:
          case F_GETLK:
  #ifdef F_OFD_SETLK
          case F_OFD_SETLK:
          case F_OFD_SETLKW:
          case F_OFD_GETLK:
  #endif
          {
              struct flock *lock = va_arg(args, struct flock *);
              result = apth_func_raw(fcntl)(fd, cmd, lock);
              break;
          }

          // Commands taking struct f_owner_ex* argument
  #ifdef F_GETOWN_EX
          case F_GETOWN_EX:
          case F_SETOWN_EX:
          {
              struct f_owner_ex *owner = va_arg(args, struct f_owner_ex *);
              result = apth_func_raw(fcntl)(fd, cmd, owner);
              break;
          }
  #endif

          // Commands taking no argument (F_GETFL, F_GETFD, F_GETOWN, etc.)
          default:
              result = apth_func_raw(fcntl)(fd, cmd);
              break;
      }

      va_end(args);
      return result;
  }

  // Export the symbol
  APTH_API int fcntl(int fd, int cmd, ...)
  {
      va_list args;
      va_start(args, cmd);

      // Forward to our hooked version
      // We need to handle the variadic args carefully
      int result;
      switch (cmd) {
          case F_SETFL:
          case F_DUPFD:
          case F_DUPFD_CLOEXEC:
          case F_SETFD:
          case F_SETOWN:
          case F_SETSIG:
          case F_SETLEASE:
          case F_NOTIFY:
          case F_SETPIPE_SZ:
  #ifdef F_ADD_SEALS
          case F_ADD_SEALS:
  #endif
          {
              int arg = va_arg(args, int);
              result = apth_func(fcntl)(fd, cmd, arg);
              break;
          }
          case F_SETLK:
          case F_SETLKW:
          case F_GETLK:
  #ifdef F_OFD_SETLK
          case F_OFD_SETLK:
          case F_OFD_SETLKW:
          case F_OFD_GETLK:
  #endif
          {
              struct flock *lock = va_arg(args, struct flock *);
              result = apth_func(fcntl)(fd, cmd, lock);
              break;
          }
  #ifdef F_GETOWN_EX
          case F_GETOWN_EX:
          case F_SETOWN_EX:
          {
              struct f_owner_ex *owner = va_arg(args, struct f_owner_ex *);
              result = apth_func(fcntl)(fd, cmd, owner);
              break;
          }
  #endif
          default:
              result = apth_func(fcntl)(fd, cmd);
              break;
      }

      va_end(args);
      return result;
  }

  Step 1.2: Update hook_lowlevel_io.h

  Add fcntl to the hook list:

  // In src/hook_libc/hook_lowlevel_io.h

  // Add to APTH_LIST_OF_HOOK_LOWLEVEL_IO macro (around line 73):
  #define APTH_LIST_OF_HOOK_LOWLEVEL_IO \
      X(open)                           \
      X(open64)                         \
      /* ... existing entries ... */    \
      X(pipe)                           \
      X(fcntl)  /* ADD THIS LINE */

  // Add declaration (around line 177):
  // ==================== 13.14 File Descriptor Flags ====================
  APTH_DECLARE_FETCH_LIBCFUNC(int, fcntl, int fd, int cmd, ... /* arg */)
  APTH_INTERNAL int apth_func(fcntl)(int fd, int cmd, ... /* arg */);

  Step 1.3: Simplify apth_fd_register() and remove refcount

  Modify: src/internal/apth_fd.c

  // Remove apth_fd_acquire and apth_fd_release entirely
  // Update apth_fd_register:

  APTH_INTERNAL void apth_fd_register(int fd)
  {
      if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
          return;

      // Get current flags
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags == -1)
          return;

      // Store original flags (for informational purposes only)
      APTH_FD_TABLE[fd].orig_flags = flags;

      // Set to non-blocking if not already
      if (!(flags & O_NONBLOCK)) {
          if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
              apth_debug("Failed to set O_NONBLOCK on fd=%d", fd);
              return;
          }
      }

      // Mark as managed (no refcount needed)
      atomic_store_release(&APTH_FD_TABLE[fd].managed, 1);
      apth_debug("Registered fd=%d (orig_flags=0x%x)", fd, flags);
  }

  APTH_INTERNAL void apth_fd_unregister(int fd)
  {
      if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
          return;

      // Just mark as unmanaged - don't restore flags
      // The FD is being closed anyway, so no point in restoring
      atomic_store_release(&APTH_FD_TABLE[fd].managed, 0);
      apth_debug("Unregistered fd=%d", fd);
  }

  // DELETE these functions entirely:
  // - apth_fd_acquire()
  // - apth_fd_release()

  Step 1.4: Update struct apth_fd_entry

  Modify: src/internal/apth_fd.h

  struct apth_fd_entry
  {
      int orig_flags;        // Original fcntl flags (informational only)
      _Atomic(int) managed;  // Whether this filedescriptor is managed by libapth
      // REMOVE: _Atomic(int) refcount;  // DELETE THIS LINE
  };

  // UPDATE function declarations:
  APTH_INTERNAL void apth_fd_table_init(void);
  APTH_INTERNAL void apth_fd_register(int fd);      // Register when socket/open
  APTH_INTERNAL void apth_fd_unregister(int fd);    // Unregister when close
  APTH_INTERNAL void apth_notify_fd_closed(int fd); // Notify all schedulers about fd close

  // DELETE these declarations:
  // APTH_INTERNAL int apth_fd_acquire(int fd);
  // APTH_INTERNAL void apth_fd_release(int fd);

  Step 1.5: Update all hooked I/O functions

  Modify: src/hook_libc/hook_socket.c and all I/O hooks

  Replace all occurrences of:
  int orig_mode = apth_fd_acquire(fd);
  if (orig_mode < 0)
      return apth_error(-1, EBADF);
  // ... operation ...
  apth_fd_release(fd);

  With:
  // Just validate the FD, no acquire/release needed
  if (!apth_util_fd_valid(fd))
      return apth_error(-1, EBADF);
  // ... operation ...

  Example for connect():

  // In hook_socket.c, around line 54:
  APTH_DEFINE_HOOK(
      int, connect,
      (int fd, const struct sockaddr *addr, socklen_t length),
      (fd, addr, length))
  {
      apth_hook_debug(connect);
      apth_t cur = CUR_APTH;
      apth_debug("apth_func_connect: enter from thread \"%s\"", cur->name);

      // POSIX compliance - just validate FD
      if (!apth_util_fd_valid(fd))
          return apth_error(-1, EBADF);

      // FD is already in non-blocking mode (set by apth_fd_register)
      // Try to connect
      int rv;
      while ((rv = apth_func_raw(connect)(fd, (struct sockaddr *)addr, length)) == -1 && errno == EINTR)
          ;

      // If it is still on progress wait until socket is really writeable
      if (rv == -1 && errno == EINPROGRESS)
      {
          struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_WRITEABLE);
          apth_wait_event(&ev);
          assert(ev.ev_status != APTH_EV_STATUS_PENDING);

          int err;
          socklen_t errlen = sizeof(err);
          if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&err, &errlen) == -1)
              return -1;

          if (err == 0)
              return 0;
          return apth_error(rv, err);
      }

      apth_debug("apth_func_connect: leave to thread \"%s\"", cur->name);
      return rv;
  }

  Apply similar changes to:
  - accept() in hook_socket.c
  - recvfrom() in hook_socket.c
  - sendto() in hook_socket.c
  - read() in hook_lowlevel_io/input_output_primitives.c
  - write() in hook_lowlevel_io/input_output_primitives.c
  - All other I/O functions

  Phase 2: Optimize Event Management (High Impact)

  Step 2.1: Add waiter memory pool to scheduler

  Modify: src/internal/types/struct_apth_sched_st.h

  // Add after line 42 (after active_fd_count):

  // Memory pool for apth_epoll_waiter structures to avoid malloc/free
  #define APTH_WAITER_POOL_SIZE 256
  struct apth_epoll_waiter waiter_pool[APTH_WAITER_POOL_SIZE];
  struct list free_waiters;  // List of free waiter structures
  int waiter_pool_allocated; // Number of waiters allocated from pool

  Step 2.2: Initialize waiter pool in scheduler init

  Modify: src/internal/apth_sched.c

  // In apth_scheduler_init(), after line 142:

  // Initialize waiter memory pool
  list_init(&sched->free_waiters);
  for (int i = 0; i < APTH_WAITER_POOL_SIZE; i++) {
      list_push_back(&sched->free_waiters, &sched->waiter_pool[i].elem);
  }
  sched->waiter_pool_allocated = 0;

  Step 2.3: Add waiter allocation/deallocation functions

  Add to: src/internal/apth_event.c (before line 21)

  // Fast waiter allocation from pool
  static struct apth_epoll_waiter *alloc_waiter(apth_sched_t sched)
  {
      if (!list_empty(&sched->free_waiters)) {
          struct list_elem *e = list_pop_front(&sched->free_waiters);
          return apth_epoll_waiter_list_entry(e);
      }

      // Pool exhausted, fall back to malloc
      sched->waiter_pool_allocated++;
      if (sched->waiter_pool_allocated > APTH_WAITER_POOL_SIZE) {
          apth_debug("WARNING: waiter pool exhausted, using malloc (count=%d)",
                     sched->waiter_pool_allocated);
      }
      return (struct apth_epoll_waiter *)malloc(sizeof(struct apth_epoll_waiter));
  }

  static void free_waiter(apth_sched_t sched, struct apth_epoll_waiter *w)
  {
      // Check if this waiter is from the pool
      if (w >= sched->waiter_pool && w < sched->waiter_pool + APTH_WAITER_POOL_SIZE) {
          // Return to pool
          list_push_back(&sched->free_waiters, &w->elem);
      } else {
          // Was allocated with malloc
          free(w);
          sched->waiter_pool_allocated--;
      }
  }

  Step 2.4: Replace malloc/free with pool allocation

  Modify: src/internal/apth_event.c

  Replace line 32:
  // OLD:
  struct apth_epoll_waiter *w = (struct apth_epoll_waiter *)malloc(sizeof(*w));

  // NEW:
  struct apth_epoll_waiter *w = alloc_waiter(sched);

  Replace line 76 and all other free(w) calls:
  // OLD:
  free(w);

  // NEW:
  free_waiter(sched, w);

  Apply this change to all malloc/free of waiters in apth_event.c (lines 32, 76, 114, 204, 275).

  Step 2.5: Optimize aggregate mask recalculation with refcounts

  Modify: src/internal/apth_fd_slot.h

  struct apth_epoll_fd_slot
  {
      int fd;                    // monitored fd
      uint32_t aggregate_events; // aggregation of all waiter's event mask
      struct list waiters;       // apth list waiting this fd
      int waiter_count;          // number of waiters
      bool registered;           // already registered to epoll?
      struct list_elem elem;     // link into scheduler's active_fd_slots list

      // ADD: Reference counts for each event type to avoid recalculation
      int readable_count;   // Number of waiters waiting for EPOLLIN
      int writeable_count;  // Number of waiters waiting for EPOLLOUT
      int exception_count;  // Number of waiters waiting for EPOLLPRI
  };

  Step 2.6: Update aggregate mask management

  Modify: src/internal/apth_event.c

  In epoll_map_add_waiter(), replace lines 38-53:

  // Calculate waiter's event mask
  uint32_t needed = 0;
  if (ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE) {
      needed |= EPOLLIN;
      slot->readable_count++;
  }
  if (ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE) {
      needed |= EPOLLOUT;
      slot->writeable_count++;
  }
  if (ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION) {
      needed |= EPOLLPRI;
      slot->exception_count++;
  }

  // Add to waiter list
  list_push_back(&slot->waiters, &w->elem);
  slot->waiter_count++;

  // Update aggregation mask based on refcounts
  uint32_t old_aggregate = slot->aggregate_events;
  slot->aggregate_events = 0;
  if (slot->readable_count > 0)
      slot->aggregate_events |= EPOLLIN;
  if (slot->writeable_count > 0)
      slot->aggregate_events |= EPOLLOUT;
  if (slot->exception_count > 0)
      slot->aggregate_events |= EPOLLPRI;

  In epoll_map_remove_waiter(), replace lines 107-156:

  // Find and remove correspond waiter from the list
  FOR_ELEMENT_IN_LIST(slot->waiters, e)
  {
      struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
      if (w->th == th && w->ev == ev)
      {
          // Decrement refcounts based on what this waiter was waiting for
          if (ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
              slot->readable_count--;
          if (ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
              slot->writeable_count--;
          if (ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
              slot->exception_count--;

          list_remove(&w->elem);
          free_waiter(sched, w);
          slot->waiter_count--;
          break;
      }
  }

  if (slot->waiter_count == 0)
  {
      // Last waiter removed, unregister from epoll
      if (slot->registered)
      {
          epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
          slot->registered = false;
          list_remove(&slot->elem);
          sched->active_fd_count--;
      }
      slot->aggregate_events = 0;
      slot->readable_count = 0;
      slot->writeable_count = 0;
      slot->exception_count = 0;
  }
  else
  {
      // Recalculate aggregate mask from refcounts (O(1) instead of O(n))
      uint32_t new_aggregate = 0;
      if (slot->readable_count > 0)
          new_aggregate |= EPOLLIN;
      if (slot->writeable_count > 0)
          new_aggregate |= EPOLLOUT;
      if (slot->exception_count > 0)
          new_aggregate |= EPOLLPRI;

      if (new_aggregate != slot->aggregate_events)
      {
          slot->aggregate_events = new_aggregate;
          struct epoll_event ee;
          ee.events = new_aggregate;
          ee.data.fd = fd;
          epoll_ctl(sched->epoll_fd, EPOLL_CTL_MOD, fd, &ee);
      }
  }

  Apply similar changes to epoll_map_wake_fd() around lines 220-242.

  Step 2.7: Initialize refcounts in scheduler init

  Modify: src/internal/apth_sched.c

  In apth_scheduler_init(), update lines 129-136:

  // Initialize fd slot table
  for (int i = 0; i < APTH_EPOLL_FD_SLOT_TABLE_SIZE; i++)
  {
      sched->fd_slot_table[i].fd = i;
      sched->fd_slot_table[i].aggregate_events = 0;
      list_init(&sched->fd_slot_table[i].waiters);
      sched->fd_slot_table[i].waiter_count = 0;
      sched->fd_slot_table[i].registered = false;
      // ADD:
      sched->fd_slot_table[i].readable_count = 0;
      sched->fd_slot_table[i].writeable_count = 0;
      sched->fd_slot_table[i].exception_count = 0;
  }

  Phase 3: Additional Optimizations (Medium Impact)

  Step 3.1: Batch epoll_ctl operations

  Modify: src/internal/apth_event.c

  Add a flag to defer epoll_ctl calls:

  // In epoll_map_add_waiter(), after line 89:
  // Don't call epoll_ctl(MOD) immediately if we're in the middle of event processing
  // The event manager will do a final MOD pass if needed
  // This reduces syscalls when multiple waiters are added for the same FD

  // Only call epoll_ctl if the mask actually changed AND we're not in batch mode
  // For now, always call it (can optimize later with a batch flag)

  Step 3.2: Increase MAX_WAKE_BATCH

  Modify: src/internal/apth_event.c line 350:

  // OLD:
  #define MAX_WAKE_BATCH 128

  // NEW:
  #define MAX_WAKE_BATCH 512  // Larger batch reduces lock contention

  Step 3.3: Add fast path for immediately-ready FDs

  Modify: Hook I/O functions to skip event registration if operation succeeds immediately

  Example in recvfrom():

  // Try the operation first (FD is already non-blocking)
  ssize_t rv;
  while ((rv = apth_func_raw(recvfrom)(sockfd, buf, nbytes, flags, src_addr, addrlen)) < 0 && errno == EINTR)
      ;

  // Fast path: if it succeeded or got a real error (not EAGAIN), return immediately
  if (rv >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
      return rv;

  // Slow path: need to wait for FD to become readable
  for (;;)
  {
      struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_READABLE);
      apth_wait_event(&ev);
      assert(ev.ev_status != APTH_EV_STATUS_PENDING);

      while ((rv = apth_func_raw(recvfrom)(sockfd, buf, nbytes, flags, src_addr, addrlen)) < 0 && errno == EINTR)
          ;

      if (rv >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
          break;
  }

  return rv;

  Phase 4: Build System Updates

  Step 4.1: Add fcntl.c to build

  Modify: Makefile or build configuration

  Add src/hook_libc/hook_lowlevel_io/fcntl.c to the list of source files.

  Testing Plan

  After implementing these changes:

  1. Compile and test basic functionality:
  make clean && make
  LD_PRELOAD=./libapth.so ./test/test_send_recv_apth
  2. Run performance comparison:
  time LD_PRELOAD=./libapth.so ./test/test_send_recv_apth
  time ./test/test_send_recv_pthread
  3. Test fcntl hook:
  Create a test that calls fcntl(fd, F_SETFL, 0) to try to clear O_NONBLOCK and verify it stays set.
  4. Stress test with many FDs:
  Run tests with high FD contention to verify waiter pool works correctly.

  Expected Performance Improvements

  - Phase 1 (FD refcount removal): 20-30% improvement in I/O throughput
  - Phase 2 (Event management optimization): 15-25% improvement
  - Combined: 35-50% overall improvement in I/O-heavy workloads

  The most critical change is Phase 1 - it eliminates syscalls and atomic operations from the hot path. Phase 2 reduces memory allocation overhead and
  improves event processing efficiency.