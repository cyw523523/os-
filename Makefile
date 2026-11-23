# lab1 骨架 Makefile(课程提供;新增源文件时把它加进 OBJS)
.DEFAULT_GOAL := all

CC   = riscv64-unknown-elf-gcc
LD   = riscv64-unknown-elf-ld
CFLAGS = -Wall -Werror -O -std=gnu99 -mcmodel=medany -ffreestanding \
         -nostdlib -fno-common -ggdb -march=rv64gc -fno-stack-protector -fno-pie
QEMU = qemu-system-riscv64
OBJCOPY ?= riscv64-unknown-elf-objcopy

UPROGS = sh hi spin badecall bufstorm badaccess memcap noop vmstress
UPROGS += bench lottery_test stride_test
UPROGS += smpstress
UPROGS += zerocopy cowpair
UPROGS += argcheck exectest cowstress cowoom
UPROGS += zeroraw
UPROGS += init fsstress fscheck fscrash fsverify
UFLAGS = -Wall -Werror -O -std=gnu99 -mcmodel=medany -ffreestanding \
         -nostdlib -fno-common -ggdb -march=rv64gc \
         -fno-stack-protector -fno-pie -I. -DSID=2024302111375

OBJS = kernel/entry.o kernel/start.o kernel/console.o kernel/printf.o kernel/main.o \
       kernel/vm.o kernel/proc.o kernel/trap.o kernel/syscall.o \
       kernel/kernelvec.o kernel/trampoline.o kernel/swtch.o kernel/spinlock.o \
       kernel/pipe.o kernel/fsutil.o kernel/virtio_adapter.o kernel/crash_board.o \
       kernel/bio.o kernel/log.o kernel/fs.o kernel/file.o
LIBGCC = $(shell $(CC) -print-libgcc-file-name)

kernel/kernel: $(OBJS) kernel/kernel.ld
	$(LD) -T kernel/kernel.ld -o $@ $(OBJS)

user/usys.S: user/usys.pl
	perl user/usys.pl > user/usys.S

user-flat/ulib.o: user/ulib.c user/user.h
	mkdir -p user-flat
	$(CC) $(UFLAGS) -Iuser -c $< -o $@

user-flat/printf.o: user/printf.c user/user.h
	mkdir -p user-flat
	$(CC) $(UFLAGS) -Iuser -c $< -o $@

user-flat/usys.o: user/usys.S kernel/syscall.h
	mkdir -p user-flat
	$(CC) $(UFLAGS) -Iuser -c $< -o $@

user-flat/common.o: user/common.c user/common.h user/user.h
	mkdir -p user-flat
	$(CC) $(UFLAGS) -Iuser -c $< -o $@

user-flat/%.elf: user/%.c user/user.h user/lab5test.h kernel/lab5.h kernel/syscall.h support/user-lab5.ld user-flat/ulib.o user-flat/printf.o user-flat/usys.o user-flat/common.o
	mkdir -p user-flat
	$(CC) $(UFLAGS) -Iuser -c $< -o user-flat/$*.o
	$(LD) -T support/user-lab5.ld -o $@ user-flat/$*.o user-flat/ulib.o user-flat/printf.o user-flat/usys.o user-flat/common.o $(LIBGCC)

user/zerocopy.c: support/tests/zerocopy.c
	cp $< $@

user/cowpair.c: support/tests/cowpair.c
	cp $< $@

user/badecall.c: support/tests/badecall.c
	cp $< $@

user/bufstorm.c: support/tests/bufstorm.c
	cp $< $@

user/badaccess.c: support/tests/badaccess.c
	cp $< $@

user/memcap.c: support/tests/memcap.c
	cp $< $@

user/noop.c: support/tests/noop.c
	cp $< $@

user/vmstress.c: support/tests/vmstress.c
	cp $< $@

LAB6_LOG_BLOCKS := $(shell awk '/^#define LAB6_LOG_BLOCKS / { print $$3 }' kernel/course_sid.h)
HOSTCC = cc
MKFS_SRC = gifts/lab6/gifts/mkfs/mkfs.c
USER_ELFS = $(addprefix user-flat/,$(UPROGS:=.elf))

.PHONY: all fsimg qemu clean
all: kernel/kernel fsimg

mkfs/mkfs: $(MKFS_SRC) kernel/fs.h kernel/param.h kernel/types.h kernel/stat.h kernel/course_sid.h Makefile
	mkdir -p mkfs
	$(HOSTCC) -DLAB6_LOG_BLOCKS=$(LAB6_LOG_BLOCKS) -I. -o $@ $(MKFS_SRC)

fs.img: mkfs/mkfs $(USER_ELFS) Makefile
	mkdir -p user-flat/image
	for p in $(UPROGS); do cp user-flat/$$p.elf user-flat/image/$$p; done
	cd user-flat/image && ../../mkfs/mkfs ../../fs.img.new $(UPROGS)
	mv fs.img.new fs.img

fsimg: fs.img

user/fsstress.c: gifts/lab6/support/tests/fsstress.c
	cp $< $@

kernel/virtio_adapter.o: gifts/lab6/gifts/kernel/virtio_disk.c
# The SiFive test MMIO register accepts a 32-bit store on QEMU virt.
kernel/crash_board.c: gifts/lab6/support/crash.c Makefile
	sed 's/volatile uint64 \*)TEST_DEVICE_PA/volatile uint32 *)TEST_DEVICE_PA/' $< > $@

kernel/crash_board.o: kernel/crash_board.c

# The supplied linker script only collects .text.* while the supplied
# trampoline uses an orphan section named trampsec. Rename it at build time
# so both read-only course files can remain unchanged.
kernel/trampoline.o: kernel/trampoline.S
	$(CC) $(CFLAGS) -Ikernel -c -o $@.tmp $<
	$(OBJCOPY) --rename-section trampsec=.text.trampoline,alloc,load,readonly,code,contents $@.tmp $@
	rm -f $@.tmp

%.o: %.c kernel/riscv.h kernel/course_sid.h kernel/proc.h kernel/lab5.h kernel/syscall.h $(wildcard kernel/*.h)
	$(CC) $(CFLAGS) -Ikernel -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -Ikernel -c -o $@ $<

CPUS ?= 1
qemu: all
	$(QEMU) -machine virt -bios none -kernel kernel/kernel -m 128M -smp $(CPUS) -nographic \
	    -global virtio-mmio.force-legacy=false -drive file=fs.img,if=none,format=raw,id=x0,cache=directsync \
	    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

clean:
	rm -f kernel/*.o kernel/kernel user/usys.S user/fsstress.c kernel/crash_board.c mkfs/mkfs fs.img fs.img.new
	rm -rf user-flat
