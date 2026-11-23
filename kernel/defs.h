#ifndef DEFS_H
#define DEFS_H

struct spinlock;
void initlock(struct spinlock *, const char *);
void *kalloc(void);
void *memset(void *, int, unsigned long);
void *memmove(void *, const void *, unsigned long);
void *memcpy(void *, const void *, unsigned long);
void panic(const char *) __attribute__((noreturn));
void printf(const char *, ...);
void wakeup(void *);
int argint(int, int *);

#endif
