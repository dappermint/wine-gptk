# sync tests

`macsync-test.c` exercises the state machine in `include/wine/macsync.h` on its
own, with no wine around it. It stubs out the generated protocol header and
supplies the enum, so it builds against the header directly:

    clang -arch x86_64 -O2 -Wall -I<wine>/include -I<wine>/include/wine \
        -o macsync-test tests/macsync-test.c && ./macsync-test

Covers auto and manual events, semaphore limits, mutex recursion / non-owner
release / abandonment, rollback, then 8 threads hammering one semaphore, 8
threads on one mutex, and a cross-process blocking wake.

`syncbench.c` is a PE binary that times the operations msync is meant to make
cheap. Build it with the mingw toolchain and run it under wine twice:

    x86_64-w64-mingw32-gcc -O2 -o syncbench.exe tests/syncbench.c
    WINEMSYNC=0 wine syncbench.exe
    WINEMSYNC=1 wine syncbench.exe
