# 00 — Windows `slang-static.lib` is ~6× the other platforms

Status: open investigation. No fix proposed yet; this records what is measured,
what is inferred, and what to run next.

Scope: `Giesch/slang` only. The downstream consumer is
[`Giesch/slang-rs`](https://github.com/Giesch/slang-rs), which vendors these
archives — see its `plans/00_static_build.md` for why the size matters there.

## The measurement

From the published `v2026.13.1-static` release. Every archive holds the same
logical content: one merged static library, `include/`, `licenses/`.

| platform | `lib/` contents, uncompressed | `.tar.gz` | `.tar.xz` | xz ratio |
| --- | --- | --- | --- | --- |
| macos-aarch64 | `libslang-static.a` — 61,266,912 (61.3 MB) | 19.8 MB | 11.8 MB | 0.59 |
| linux-x86_64 | `libslang-static.a` — 79,440,832 (79.4 MB) | 22.8 MB | 13.8 MB | 0.61 |
| windows-x86_64 | `slang-static.lib` — **487,825,608 (487.8 MB)** | 72.0 MB | 36.7 MB | **0.51** |

Windows is **6.1× Linux** and **8.0× macOS** uncompressed.

Reproduce:

```bash
base=https://github.com/Giesch/slang/releases/download/v2026.13.1-static
for p in linux-x86_64 macos-aarch64 windows-x86_64; do
  curl -sSLO "$base/slang-static-2026.13.1-$p.tar.xz"
done
curl -sSL "$base/SHA256SUMS" | grep 'tar\.xz' | sha256sum -c -
for f in slang-static-2026.13.1-*.tar.xz; do tar -tJvf "$f" | grep '/lib/'; done
```

## Why the compression ratio is the interesting part

Windows compresses **better** than the other two — 0.51 against 0.59–0.61 — while
being far larger. If the extra ~408 MB were genuine, distinct machine code it
would compress like the rest and the ratio would hold. It doesn't, so the excess
is highly redundant data.

That is the strongest evidence available without inspecting the archive
internals, and it points at debug records rather than, say, less effective
optimization or duplicated object code.

Supporting datapoint from
[PR #3](https://github.com/Giesch/slang/pull/3): on Linux the bundled archive was
**1.18 GB with debug info and 76.6 MB without**. Debug records are demonstrably
capable of an order-of-magnitude swing here. Windows at 487.8 MB sits between the
two extremes, consistent with *some* debug data surviving rather than all of it.

## What is already ruled out

All set in `.github/workflows/release-static.yml` and confirmed in the run logs:

- `SLANG_ENABLE_RELEASE_DEBUG_INFO=OFF` — and it demonstrably works on Linux,
  which lands at 79.4 MB rather than 1.18 GB.
- `SLANG_ENABLE_RELEASE_LTO=OFF` — pinned off deliberately. LTO would fill the
  archive with IR, which would look like this, so it is worth stating explicitly
  that it is already off.
- `SLANG_ENABLE_SPIRV_TOOLS_MIMALLOC=OFF` (Windows only).
- `SLANG_EXCLUDE_DAWN=ON` (Windows only).
- `SLANG_ENABLE_DXIL=OFF`, `SLANG_ENABLE_GFX=OFF`, `SLANG_ENABLE_SLANG_RHI=OFF`,
  `SLANG_ENABLE_TESTS=OFF`, `SLANG_ENABLE_EXAMPLES=OFF`,
  `SLANG_ENABLE_REPLAYER=OFF`, `SLANG_EXCLUDE_TINT=ON`,
  `SLANG_SLANG_LLVM_FLAVOR=DISABLE`.

So this is not a case of Windows building more components than the other
platforms. It is the same target list, merged the same way, by
`cmake/BundleStaticLibrary.cmake`.

Also not the cause: archive-format overhead. A `.lib` carries more per-member
metadata than a `.a`, but nowhere near 400 MB of it across ~686 members.

## Hypotheses, most likely first

**1. Debug records embedded in the object files.** MSVC's `/Z7` puts debug info
into `.debug$S` / `.debug$T` COFF sections inside each `.obj`, which then land
inside the `.lib`. `/Zi` instead writes a separate PDB, which would not bloat the
archive. If something in the build selects `/Z7` — or selects any debug info
format for the Release configuration — this is the whole story. Note that
`SLANG_ENABLE_RELEASE_DEBUG_INFO=OFF` may only gate the GCC/Clang `-g` flags and
not reach `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT`; that is worth reading rather
than assuming.

**2. A subproject overriding the setting.** `glslang` and `SPIRV-Tools` are
brought in with `add_subdirectory()`, and PR #3 already documented that they can
override `CMAKE_MSVC_RUNTIME_LIBRARY`. The same is plausible for debug-info
format. The collector walks the link graph and picks up whatever those targets
produce, so a subproject setting its own flags would show up here and nowhere
else.

**3. No strip equivalent.** On ELF, unstripped data can be removed after the
fact; `lib.exe` has no equivalent, so anything embedded at compile time stays.
This is not a *cause*, but it explains why the Linux number can look clean even
if some debug data were produced — and why Windows needs the fix at the compile
step rather than at packaging.

## Diagnostics to run next

Cheapest decisive check first. Add temporarily to the Windows job after
`Stage release tree`, or run locally against a downloaded archive:

```yaml
      - name: Diagnose archive size
        if: runner.os == 'Windows'
        env:
          BASE: ${{ steps.stage.outputs.base }}
        run: |
          # Dash/double-slash care: under Git Bash a `//x` argument is only
          # rewritten to `/x` when it does not look like a path.
          dumpbin //headers "$BASE/lib/slang-static.lib" > headers.txt
          echo "members:  $(lib //list "$BASE/lib/slang-static.lib" | wc -l)"
          echo "debug\$S:  $(grep -c 'debug\$S' headers.txt || true)"
          echo "debug\$T:  $(grep -c 'debug\$T' headers.txt || true)"
```

Non-zero `.debug$` counts confirm hypothesis 1 and the fix is a compile-flag
change. Zero counts refute it and the next step is to compare per-member sizes
against the Linux archive's members to find where the bulk actually lives:

```bash
lib /list slang-static.lib          # member inventory
dumpbin /headers <member>.obj       # section sizes for the biggest offenders
```

Also worth reading, before running anything:

- What `SLANG_ENABLE_RELEASE_DEBUG_INFO` actually sets on MSVC in the top-level
  `CMakeLists.txt`.
- Which configuration `cmake --build --preset release` resolves to, and whether
  it is `Release` or `RelWithDebInfo`. The latter would add `/Zi` by default and
  would explain everything.

## Candidate fixes

Do not apply these blind — pick from the diagnostic result.

- `-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=""` in the workflow's Windows-only flag
  block. Blunt but decisive, and cheap to test via the `pull_request` trigger.
- Make `SLANG_ENABLE_RELEASE_DEBUG_INFO=OFF` set the MSVC debug-info format
  explicitly, if it currently only handles GCC/Clang. This is the better fix if
  hypothesis 1 holds, since it makes the existing option mean what it says.
- Force the setting onto the `glslang` / `SPIRV-Tools` subprojects if hypothesis
  2 holds.
- Unrelated to debug info but untried and noted in PR #3's follow-ups:
  `/Gy` `/Gw` (function- and data-level linking, the MSVC analogue of
  `-ffunction-sections -fdata-sections`). This shrinks the *consumer's* final
  binary via `/OPT:REF` rather than the archive itself.

## Why this is worth fixing

Not because of the release asset. 36.7 MB compressed is fine to publish.

The cost lands downstream. `slang-rs` vendors the `.tar.xz` into git and unpacks
it into `OUT_DIR` at build time, so **every Windows build of that crate writes
~488 MB into `target/` and keeps it there**. Compression hides the problem in
transit and does nothing about it on disk.

Secondarily, the vendored blob is 36.7 MB of the 62.3 MB a full three-platform
set costs — 59% — and those blobs are permanent in git history, one set per Slang
version bump.

## References

- Release: [`v2026.13.1-static`](https://github.com/Giesch/slang/releases/tag/v2026.13.1-static)
- Static build and bundling: [PR #3](https://github.com/Giesch/slang/pull/3),
  including the 1.18 GB / 76.6 MB Linux debug-info measurement
- Publish job split and checksums: [PR #4](https://github.com/Giesch/slang/pull/4)
- Workflow: `.github/workflows/release-static.yml`
- Bundling implementation: `cmake/BundleStaticLibrary.cmake`,
  `cmake/BundleStaticLibraryImpl.cmake`
- Downstream consumer and vendoring plan:
  [`Giesch/slang-rs` PR #2](https://github.com/Giesch/slang-rs/pull/2)
