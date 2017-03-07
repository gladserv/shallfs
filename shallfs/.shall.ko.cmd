cmd_fs/shallfs/shall.ko := ld -r -m elf_x86_64 -T ./scripts/module-common.lds --build-id  -o fs/shallfs/shall.ko fs/shallfs/shall.o fs/shallfs/shall.mod.o
