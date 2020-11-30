(module
    ;; $t0 is just the type of vmMain
    ;; NOTE: MAX_VMMAIN_ARGS parameters
    (type $t0
        (func (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)
                (result i32)))
    (; $t1 is just the type of syscall 1 function ;)
    (type $t1 (func (param i32) (result i32)))
    (; similarly to other syscalls ;)

    (; now for each syscall, define an entry point ;)
    ;;(import "env" "__syscall1" (func $__syscall1 (type $t1)))

    (; the very first ENTER opcode signifies the start of vmMain ;)
    (func $main
        (export "vmMain")
        (type $t0)
        (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)
        (result i32)
        (i32.const 0)
        return)
    (; rest of functions (every function is anything between ENTER/LEAVE opcodes) ;)

    ;; the whole memory of the VM (all of the data,lit,bss combined)
    (memory $data (export "memory") 1024) ;; 1024: ALL of the memory size, including bss segment (we might also put an upper bound there since it's defined in QVM)
    ;; this means: starting at offset 0, put a string to initialize the memory
    ;; this is where we initialize the data+lit segments, but keep BSS at all zeroes
    (data $memory (i32.const 0) "hello, world!\00") ;; this is hex-encoded string, e.g. "Hello World\00\10\04\00\00"
)
