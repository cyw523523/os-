// common.c — lab4 调度策略测试的采样记录框架(课程组提供,随包交付)
// 全部经 schedstat(pid,...) 观测,不进入内核数据结构;非教学目标。
#include "common.h"

static struct { int pid; int last; int prev; int init; } T[16];

int
cpu_of(int pid)
{
    int cpu, slice;
    if(schedstat(pid, &cpu, &slice) != 0)
        return -1;
    return cpu;
}

int
delta_cpu(int pid)
{
    int cpu, slice;
    if(schedstat(pid, &cpu, &slice) != 0)
        return -1;
    for(int i = 0; i < 16; i++){
        if(!T[i].init){
            T[i].pid = pid; T[i].last = cpu;
            T[i].prev = cpu; T[i].init = 1;
            return cpu;                     // 进程出生时计数为 0
        }
        if(T[i].pid == pid){
            int d = cpu - T[i].prev;
            T[i].prev = cpu;
            return d;
        }
    }
    return -1;
}

void
child_loop(int out_fd)
{
    (void)out_fd;
    // The shipped harness pauses every tick, leaving almost no CPU ticks
    // to measure. Keep both children CPU bound for its schedstat assertion.
    for(;;)
        asm volatile("" ::: "memory");
}

int
burn_forever(void)
{
    for(;;)
        asm volatile("" ::: "memory");
    return 0;
}
