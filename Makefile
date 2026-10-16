# lab1 骨架 Makefile(课程提供;新增源文件时把它加进 OBJS)
CC   = riscv64-unknown-elf-gcc
LD   = riscv64-unknown-elf-ld
CFLAGS = -Wall -Werror -O -std=gnu99 -mcmodel=medany -ffreestanding \
         -nostdlib -fno-common -ggdb -march=rv64gc -fno-stack-protector -fno-pie
QEMU = qemu-system-riscv64
OBJCOPY ?= riscv64-unknown-elf-objcopy

UPROGS = sh hi spin badecall bufstorm badaccess memcap noop vmstress
UFLAGS = -Wall -Werror -O -std=gnu99 -mcmodel=medany -ffreestanding \
         -nostdlib -fno-common -ggdb -march=rv64gc \
         -fno-stack-protector -fno-pie -I.

OBJS = kernel/entry.o kernel/start.o kernel/console.o kernel/printf.o kernel/main.o \
       kernel/vm.o kernel/proc.o kernel/trap.o kernel/syscall.o \
       kernel/kernelvec.o kernel/trampoline.o kernel/userimg.o

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

user-flat/usys.o: user/usys.S
	mkdir -p user-flat
	$(CC) $(UFLAGS) -Iuser -c $< -o $@

user-flat/%.bin: user/%.c user/user.h user-flat/ulib.o user-flat/printf.o user-flat/usys.o
	mkdir -p user-flat
	$(CC) $(UFLAGS) -Iuser -c $< -o user-flat/$*.o
	$(LD) -T user/user.ld -Ttext 0x1000 -o user-flat/$*.elf user-flat/$*.o user-flat/ulib.o user-flat/printf.o user-flat/usys.o
	$(OBJCOPY) -O binary user-flat/$*.elf $@

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

kernel/userimg.S: Makefile $(addprefix user-flat/,$(UPROGS:=.bin))
	printf '.section .rodata\n .global _uprog_table\n_uprog_table:\n' > $@.tmp
	for p in $(UPROGS); do \
		printf ' .quad _uprog_%s_start\n .quad _uprog_%s_end\n .asciz "%s"\n .balign 8\n' $$p $$p $$p >> $@.tmp; \
		printf ' .global _uprog_%s_start\n_uprog_%s_start:\n .incbin "user-flat/%s.bin"\n .global _uprog_%s_end\n_uprog_%s_end:\n .balign 8\n' $$p $$p $$p $$p $$p >> $@.tmp; \
	done
	printf ' .quad 0\n .quad 0\n' >> $@.tmp
	mv $@.tmp $@

kernel/userimg.o: kernel/userimg.S
	$(CC) $(CFLAGS) -Ikernel -c -o $@ $<

# The supplied linker script only collects .text.* while the supplied
# trampoline uses an orphan section named trampsec. Rename it at build time
# so both read-only course files can remain unchanged.
kernel/trampoline.o: kernel/trampoline.S
	$(CC) $(CFLAGS) -Ikernel -c -o $@.tmp $<
	$(OBJCOPY) --rename-section trampsec=.text.trampoline,alloc,load,readonly,code,contents $@.tmp $@
	rm -f $@.tmp

%.o: %.c kernel/riscv.h kernel/course_sid.h
	$(CC) $(CFLAGS) -Ikernel -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -Ikernel -c -o $@ $<

# 验收环境固定(排雷环节禁止改动本行以下内容)
qemu: kernel/kernel
	$(QEMU) -machine virt -bios none -kernel kernel/kernel -nographic

clean:
	rm -f kernel/*.o kernel/kernel user/usys.S kernel/userimg.S
	rm -rf user-flat
