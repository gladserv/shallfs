cmd_fs/shallfs/shallfs.ko := ld -r -m elf_x86_64 -T ./scripts/module-common.lds --build-id  -o fs/shallfs/shallfs.ko fs/shallfs/shallfs.o fs/shallfs/shallfs.mod.o
