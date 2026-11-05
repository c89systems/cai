# Application socket client

`libcai_app_client` implements the typed application protocol over core libcai's
workspace transport. It opens no agent runtime, authentication store or Pouch
client. It has no Softline, libmdf, liblockdc or Lua dependency. The combined
`libcai_app` facade reuses these same compiled client/codec objects; consumers
choose the standalone client or the combined application.

Build it without the terminal application with `CAI_BUILD_APP_CLIENT=ON`,
`CAI_BUILD_APP=OFF` and `CAI_BUILD_CLI=OFF`. Static/shared selection follows
`CAI_BUILD_STATIC` and `CAI_BUILD_SHARED`. Install core libcai and the
`cai-app-client` component. CMake consumers use
`find_package(cai_app_client CONFIG REQUIRED)` and
`cai::app_client_static` or `cai::app_client_shared`; pkg-config consumers use
`cai-app-client`.

Include `cai/app_client.h`. Open an explicit workspace target and drive
`cai_app_client_prepare`/`cai_app_client_pump` from the frontend's event loop.
Callbacks and borrowed observations remain on that owner thread. The client
provides typed admission, queue, role/takeover, cancellation, status, transcript,
execution, prompt recall and catalog channels. It creates no server process.
Frontend attachment lifetime, rendering, editor state and network exposure
belong to the caller. An HTML adapter can use this same client against the
workspace owner without linking a terminal UI into its process.

Existing socket tests now link the actual standalone archive. Compile-boundary
and shared-export checks guard the product boundary. The install test relocates
only core/client components and builds/runs public-header consumers through both
CMake and pkg-config, without discovering frontend/storage packages.
