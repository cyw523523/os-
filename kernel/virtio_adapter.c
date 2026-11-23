void virtio_sleep_prepare(void *);
void virtio_sleep(void);

#define sleep_prepare virtio_sleep_prepare
#define sleep virtio_sleep
#define release release_lock
#include "../gifts/lab6/gifts/kernel/virtio_disk.c"
