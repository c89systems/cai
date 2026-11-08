# Building, testing and releases

## Build prerequisites

Use CMake 3.21+, Ninja, Make, Bash and the dependency tools reported by the build helpers.
Linux uses pinned Bootlin compiler collections; it does not silently fall back to host GCC.
The normal native build prepares the compilation database used by clangd.
CLI tests additionally require Python for data fixtures, jq and tmux.
Lua verification requires the local Lua/LuaRocks toolchain.
Hardening uses Valgrind, the pinned AFL++ tools and sanitizer-capable compiler presets.
The pinned glibc dependency SDK requires glibc 2.43 or newer; use the static musl CLI where that deployment runtime is unavailable.

## Native development

```sh
make build-cli
make test
make test-cli
make format-check
make clangd-check
```

Native library and CLI configurations share `build/debug`; see `make help` for narrower targets.
Do not edit source or reconfigure that tree while its build/tests run.

Production C sources reject variable-length arrays, `alloca`, and frames larger than 8 KiB at compile time.
This is a per-frame guard, not a call-chain budget: large scratch storage and input-dependent traversal state belong on the heap.
Keep headroom for callbacks and dependencies on musl's default 128 KiB worker stack; never enlarge it to make a test pass.
`cai_core_worker_stack`, `cai_app_worker_stack`, `cai_app_skill_stack`, and `cai_lua_tools` exercise that contract (native glibc fixtures emulate the same budget).

## Static Linux CLI

```sh
scripts/cpkt-toolchains.sh ensure x86_64-linux-gnu
scripts/cpkt-toolchains.sh ensure x86_64-linux-musl
cmake --preset libcai
cmake --build --preset libcai
cmake --preset cai
cmake --build --preset cai
cmake --build --preset cai --target cai_package_cli
build/x86_64-linux-musl-release/cai --help
build/x86_64-linux-musl-release/cai -N -C /path/to/project
```

The `cai` preset selects `build/x86_64-linux-musl-release`.
ARM musl SDK presets build the corresponding static CLI alongside the libraries.
The same source also supplies the GNU SDKs and Darwin cross-build lane; Darwin runtime verification is separate from cross compilation.

## Dependency modes

- Dependency mode defaults to `cpkt`: the build uses the official
  `github.com/c89systems/cpkt` release tarball for the selected target to
  provide the native dependency stack. cai currently consumes curl, OpenSSL,
  nghttp2, libssh2, and zlib from that archive. It also uses official
  `lonejson` and `libpslog` release artifacts for the JSON library and logger
  header. Those dependency archives ship consumer CMake and pkg-config
  metadata, and cai consumes that metadata when available. Tarball URLs and
  SHA-256 values are pinned in CMake; sibling checkout artifacts are not
  dependency inputs.
- `CAI_DEPENDENCY_MODE=host` uses already-installed host dependencies instead:
  libcurl 7.86.0 or newer, OpenSSL crypto, `lonejson.h` plus `liblonejson`,
  `pslog.h`, and cpktmisc's `CpktYaml` package with a compatible cpkt core SDK.
  The libcurl minimum is required for Responses WebSocket
  support. The discovered `liblonejson` must match cai's required ABI
  generation (`liblonejson.so.26` on Linux, `liblonejson.26.dylib` on Darwin).
  `auto` chooses host only when all required host pieces are discoverable and
  the lonejson ABI matches, otherwise it falls back to `cpkt`.
- Installed CMake and pkg-config metadata preserve that dependency mode.
  `cpkt` mode records the official `cpkt` and `cpktmisc` dependency URLs and checksums;
  `host` mode records the resolved host include/library paths. `cai` archives
  do not vendor dependency headers.

## Lifecycle and release ownership

Make exposes public commands and serializes operations.
Bash sequences readable commands.
CMake owns configuration, dependencies, compilation and install.
CTest owns registered checks, their selections and native results.
Compiler/tool collections and source inputs stay stable while one operation owns a workspace.
Changing a compiler collection requires explicit invalidation of its build root.
Opt-in live and inspector targets reject absent authorization before producers.
The focused Lua Smith live target installs the local LuaRock before execution.
Hardening preparation runs live checks and fuzz smoke once, then extends fuzzing.

Library and CLI test registration live in `cmake/CaiTests.cmake` and `cmake/CaiCliTests.cmake`.
Small shared host fixture and environment declarations live beside them.
C89, public API, ABI, presets' product separation and published targets remain product contracts.
Presets use schema 3, matching the declared CMake 3.21 minimum; they require no newer preset-only feature.

## Phase contract

| Command | Work | Cleaning / release identity |
| --- | --- | --- |
| `make test`, `make test-cli` | Selected native feedback | Incremental; no cross matrix |
| `make test-all` | Native core once, CLI, memory, fuzz, Lua, offline and local e2e | Incremental; no cross matrix |
| `make format`, `make format-check` | Source formatting | No configure/build |
| `make prerelease` | Native checks, selected matrix, artifacts and consumers | Incremental; candidate override allowed |
| `make release-matrix` | Target builds, native Release/CLI checks, complete artifacts and consumers | Incremental; no cleaning or signed/tagged identity requirement |
| `make cross-test` | Matrix builds and basic ARM GNU/musl QEMU checks | Incremental; no native suites or packaging |
| `make package` | SDK staging and binary-scope checksum inventory | Warm; preserves other identified artifacts |
| `make package-verify` | Existing complete shipped bytes and extracted consumers | Does not rebuild product producers |
| `make package-source-smoke` | Explicit clean source reconstruction | Owns only source-smoke scratch |
| `make release` | One final local artifact set | Clean committed checkout, verified final commit signature and exact lightweight tag required first |

Final release cleans local generated state once.
Its source reconstruction is the native Release producer and full non-live test lane.
Its GNU SDK is packaged from that output.
Selected cross SDKs retain their target scope and the shipped x86_64 musl CLI retains its native runtime suite.
Linux ARM GNU/musl builds register basic static/shared library and application checks under CTest's `cross` label, using the required QEMU executable and pinned sysroot.
Both preparation and final release execute each selected ARM inventory once, after matrix builds.
The three Linux musl SDK presets also produce the static `cai` executable in their target build directory.
The `cai` convenience preset shares the x86_64 musl SDK configuration and directory; it is not another producer.
ARM musl CLI builds add a bounded help/version/invalid-option QEMU smoke, without inference or the full native CLI suite.
Native feedback never triggers cross builds.
Equivalent prerequisites within one checkout share its extracted dependency cache; checkouts share only global verified archives and prepared toolchains.
Source reconstruction owns its build tree and extracted dependency cache.
Native Debug library and CLI presets share `build/debug` and retain the same library configuration.
Distinct instrumentation remains separate.
Host-only fixtures are labeled `host` and are not repeated under hardening or Lua selections.

The standalone matrix runs host fixtures in its native Release lane.
Preparation runs them in Debug instead and adds the distinct memory, fuzz, Lua, offline and local service scopes.
Neither command reconstructs the source archive or requires release identity; only the final release owns those gates.

The final explicit checksum inventory requires every selected SDK plus source, CLI and Lua delivery.
Binary-only rehearsals have their own manifest under `build/`.
No release refs are mutated by local lifecycle commands.
Failed final release stops; it does not fix, retag, rebuild or publish in the same operation.
Version and target discovery failures stop packaging before producers or checksum publication.
Empty targets and unknown checksum scopes are rejected.
Source archive staging belongs under `build/`; finalized artifacts go to `dist/`.
Released Lua rockspecs reference their public source archive, also embedded in the source rock.
Failed Git tag and ignore queries are errors, distinct from valid empty lists.
Lua staging requires its public headers and preserves existing staging when header enumeration fails.
Toolchain discovery reads the exact status field.
Missing optional Darwin tools omit that target; failed or malformed discovery stops target selection.
Explicit build and source-smoke entrypoints prepare selected collections before configuration.
CMake/try-compile and AFL++ discovery only inspect prepared tools; they never download or compile toolchains as a side effect.
Service selectors and inspector opt-in are validated before mutations.
Live pods retain their original teardown manifests; startup does not recreate a missing one.
Cleanup removes local tool roots even when an override or stale CMake entry describes them as shared.
It cannot be evaded by borrowing another checkout's state.
Formatting uses native filename-safe batches and propagates traversal failures as well as formatter failures; it does not start one process per source file.

## Lua and live checks

`make lua-test` verifies the local source rock against the installed SDK.
`make test-offline` runs deterministic integration fixtures.
`make test-integration` requires `CAI_ENABLE_INTEGRATION_TESTS=1` and suitable provider credentials; it can spend tokens and contact external services.

```sh
CAI_ENABLE_INTEGRATION_TESTS=1 make test-integration
CAI_ENABLE_INTEGRATION_TESTS=1 make test-lua-smith-e2e
```

Use the current CTest inventory and `CTEST_FLAGS` to select a particular provider or scenario rather than relying on a copied test catalog.
Provider opt-ins and credentials are declared by `tests/integration_main.c` and the registered CTest commands.
Live gates are not implied by a normal offline test request.

## Local services

`make dev-up` starts checkout-owned, rootless Podman development services and prints their loopback endpoints.
`make dev-ps`, `make dev-logs`, `make dev-down` and `make dev-reset` inspect, stop or reset that stack.
Logs are under `build/devenv/logs/`.

| Service | Useful targets | External endpoint override |
|---|---|---|
| SearXNG | `searxng-up`, `searxng-wait`, `searxng-test`, `searxng-down` | `CAI_SEARXNG_BASE_URL` |
| MCP Everything | `mcp-everything-up`, `mcp-everything-wait`, `mcp-everything-test`, `mcp-everything-down` | `CAI_MCP_EVERYTHING_BASE_URL` |

`make mcp-inspector-e2e` is an explicit opt-in service/browser gate; inspect its prerequisites before running it.
SearXNG smoke tests use an explicit engine, defaulting to Wikipedia, rather than the default multi-engine fanout.

## Benchmarks and generated files

`make benchmark` measures codec, Unix transport and presentation costs; see [the benchmark contract](benchmarks.md).
Timing is not a correctness gate.
Everything under `build/` is reproducible generated state: compiler outputs, fixture workspaces, source reconstructions and benchmark reports.
Retain reports needed for a comparison elsewhere before `make clean`, which removes all checkout-owned `build/`, `dist/` and `.cache/` state while preserving the declared global cpkt cache.
Cleaning removes any built executable reached through a user symlink; rebuild it before using that symlink again.
