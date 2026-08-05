# work in progress, not in the series

Files here are NOT applied by CI. `.github/workflows/build.yml` globs
`patches/*.patch`, so anything in this directory is inert.

## 0011-ntdll-gptk4-d3dmetal-bridge.patch.wip

The GPTK 4 D3DMetal bridge, ported from rishiad/soju-wine onto wine 11.14.
It applies cleanly and both halves compile, but it **breaks the runtime**:
with it applied, `wine cmd /c echo` faults with c0000005 in a system thread
and exits 53. Without it, on an otherwise identical tree and build config,
the same command works. That A/B is the evidence -- one variable.

Fixed along the way, still not enough:
  - the registration call was placed before init_startup_info(), so it ran
    before load_main_exe() had mapped the image and passed a NULL base.
    Moving it after init_startup_info() got `wineboot -u` from exit 53 to
    exit 0, but `cmd` still faults.

Still to find: the remaining fault. Resolve the faulting address properly
(get the real ntdll.so load base rather than guessing, e.g. from a minidump
or +module trace) instead of assuming a base and reading off the nearest
symbol. Prime suspects, in order:
  - virtual.c registers every mapped PE via virtual_map_module(), which runs
    for modules mapped long before init_non_native_support() has dlopened
    libd3dshared; the guard covers a NULL function pointer but perhaps not
    every state it can be called in
  - the NtCreateThreadEx hook moved to create_server_thread(), which is also
    used by PsCreateSystemThread -- system threads are exactly what the fault
    reports ("Exception c0000005 in system thread")

That second one is the strongest lead and was not chased: the fault message
names system threads, and create_server_thread() is the one place this port
deliberately differs from soju's, because wine 11 moved the assignment there.
