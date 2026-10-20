#pragma once
#include <stdint.h>
struct IOLock;
struct IORecursiveLock;
IOLock *IOLockAlloc();
void IOLockFree(IOLock *);
void IOLockLock(IOLock *);
void IOLockUnlock(IOLock *);
IORecursiveLock *IORecursiveLockAlloc();
void IORecursiveLockFree(IORecursiveLock *);
void IORecursiveLockLock(IORecursiveLock *);
void IORecursiveLockUnlock(IORecursiveLock *);
bool IORecursiveLockHaveLock(IORecursiveLock *);
int IORecursiveLockSleep(IORecursiveLock *, void *, uint32_t);
int IORecursiveLockSleepDeadline(IORecursiveLock *, void *, uint64_t, uint32_t);
void IORecursiveLockWakeup(IORecursiveLock *, void *, bool);
