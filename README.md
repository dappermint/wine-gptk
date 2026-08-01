# wine-gptk

Apple's Game Porting Toolkit (D3DMetal) on **stock wine 11**, instead of on
CrossOver-derived wine 10.

## why

The winecx-25.1.0 route (wine 10.0 base) runs D3DMetal, but Steam never logs
in on it: `CCMInterface::ScheduleConnectionInit` returns `k_EResultPending`
and `YieldingConnect` is never entered. Measured 10 winecx runs, 10 stalls;
4 wine 11 runs, 4 logons in 2-3s. Wine 11 fixes the login, so the question is
whether D3DMetal can be carried to it.

It probably can. D3DMetal does not need CrossOver internals: it binds to a
single exported `macdrv_functions` symbol from a shim that CodeWeavers keeps
ABI-stable via two `C_ASSERT`s. The shim is LGPL wine code.

## patches

- `0001` export `__wine_unix_call` (CrossOver's CW HACK 22435) so D3DMetal's
  PE DLLs can import it
- `0002` initialise `module` in `virtual_unwind()`, still unfixed in 11.14
- `0003` the D3DMetal driver hooks, adapted for wine 11

## known unknown

wine 11 replaced `client_cocoa_view` with `client_view`, which is owned by the
client surface and NULL until one attaches. If D3DMetal expects a view to
already exist, `0003` must force one via `macdrv_CreateClientSurface()`.
Untested; the first build will say.

## what is deliberately NOT ported

The full CrossOver delta is 306 files / ~31k lines against wine 10.0. Against
11.14: 19 new files, 123 apply clean, **113 conflict** (excluding 50 `po/`
translation files). 20 of the conflicts are msync/esync, including
`server/protocol.def` - a wineserver protocol port across 11 releases.

None of that is known to be needed for D3DMetal. Adding it also risks
reintroducing whatever breaks the Steam CM connection on winecx, which is the
entire reason for moving to wine 11.
