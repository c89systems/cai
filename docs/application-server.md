# Application workspace server SDK

`libcai_app_server` owns the application runtime, storage, accepted queue,
commands and goals without linking Softline or libmdf. Enable
`CAI_BUILD_APP_SERVER=ON` with `CAI_BUILD_APP=OFF` and `CAI_BUILD_CLI=OFF` to
build it independently. Static/shared selection follows `CAI_BUILD_STATIC` and
`CAI_BUILD_SHARED`. Lua tools remain an explicit optional executor; the standalone
server does not select Lua by default.

Include `cai/app_server.h`. `cai_app_server_open` copies and validates shared
`cai_app_config` settings, and `cai_app_server_start` asynchronously starts the
workspace owner. Its context supplies captured directory/environment views;
arguments exclude the program name. An explicit attachment lifetime overrides
configured persistence. Set `persist = 1` when ownership should continue
without clients. No process is forked and no terminal or signal state is
installed. Runtime and control callbacks run on the owner's default-stack worker.

Wait for READY through `cai_app_server_observe_status` and its descriptor before
attaching a client. Native supervisory controls use `cai_app_server_control`.
STOP, join and close are explicit. Join blocks, and must run outside an owner
callback. Closing an active owner does not free or stop it. After join the same
object can start another owner. Socket clients use the independent
[client SDK](application-client.md); they open no writable storage context.

Install core libcai plus component `cai-app-server`. CMake consumers use
`find_package(cai_app_server CONFIG REQUIRED)` and `cai::app_server_static` or
`cai::app_server_shared`. pkg-config uses `cai-app-server`. The component carries
its public headers and dependency manifest; liblockdc/liblql remain backend
dependencies. The application types shared with the convenience facade live in
`cai/app_types.h`. Existing facade source and structure layouts are unchanged.

The combined application depends on the matching server target, rather than
embedding a second backend. Static and shared mixed-API tests each verify one
Pouch open across concurrent contexts and successful work after one closes.
Private composition services are not SDK declarations; the combined facade
requires the same server release through CMake/pkg-config. Public consumers use
the documented server/client APIs.

Configuration loading, parser/help metadata and semantic command documents are
backend services. Theme validation and listing belong to the frontend/utility
adapter. A standalone owner can retain frontend preferences without loading a
renderer. The common runtime implementation and single store registry are used
by both standalone owners and the existing synchronous application composition.
