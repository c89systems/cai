# cai

CAI is a C89/POSIX SDK for building agents in C and Lua, with a ready-to-use terminal coding agent named `cai`.
It supports OpenAI Responses, OpenAI-compatible Chat Completions, ChatGPT subscription authentication, streaming, local tools and MCP.

| Product | Use it for |
|---|---|
| `cai` | Interactive or batch coding work, goals, reviews and saved sessions. |
| `libcai` | Embed the client, agent runtime, tools and MCP in your own host loop. |
| `libcai_app_server` | Run the application backend without a terminal frontend. |
| `libcai_app_client` | Attach a frontend to a workspace owner over Unix sockets. |
| `libcai_app` | Embed the complete terminal application. |
| Lua 5.5 facades | Use the SDK, application owner and typed client from Lua. |

Core `libcai` does not require Softline, libmdf or liblockdc.
The terminal application uses those libraries for editing, Markdown rendering and encrypted storage.
CAI is pre-1.0; consult the current headers and guides when updating an embedding.

## Get started

Build the Linux terminal agent from a trusted checkout:

```sh
git clone https://github.com/c89systems/cai.git
cd cai
make build-cli
build/debug/cai --help
```

The build uses pinned dependency SDKs and compiler collections.
See [build prerequisites and targets](docs/development.md) for setup, the static musl binary, other architectures and SDK installation.

Start in your project directory:

```sh
cai --login                 # If ChatGPT credentials are not already available.
cai -y -C /path/to/project  # Approve this directory tree and start work.
```

If you have not installed `cai`, use the built executable's full path.
Approval is saved for the directory and its descendants; `-k` bypasses approval for one invocation.
Approval does not enable YOLO mode. Read [execution policy](docs/configuration.md#execution-policy) before changing restrictions.

```sh
cai                        # Resume this directory's newest session.
cai -N                     # Start a new session.
cai -R                     # List sessions available to resume.
cai -n -i 'Explain the failing tests'
cai --provider openai -m MODEL -n -i 'Summarize this project'
```

In the terminal, start with `/help` and `/status`.
Enter submits ordinary input; Alt-Enter submits steering at the next response/tool boundary.
Escape stops active work and pauses an active goal. Alt-Q saves the draft and detaches.
See [the terminal guide](docs/terminal.md) for commands, queues, reviews and workspace servers.

## Configure

```sh
cai --dump-config ./defaults.ini
cai -c ./config.ini
cai --home ./agent-home
```

The generated template documents every supported setting and its default.
Explicit CLI settings override environment settings, which override INI values and built-in defaults.
A unified home groups default config, state, cache, data and runtime directories; explicitly selected paths still win.
See [configuration and storage locations](docs/configuration.md) and [provider authentication](docs/providers.md).

## Documentation

[Start with the documentation index](docs/README.md).

| Task | Guide |
|---|---|
| Use the agent, commands and editor | [Terminal](docs/terminal.md) |
| Resume, trim, compact, export or delete a session | [Sessions](docs/sessions.md) |
| Add MCP apps, skills or custom tools | [MCP](docs/mcp.md), [skills](docs/skills.md), [dynamic tools](docs/plugins.md) |
| Share searchable documents and files across agents | [Context](docs/context.md) |
| Receive network prompts or observe workspace events | [Networking](docs/networking.md) |
| Embed the SDK in C or Lua | [C SDK](docs/c-sdk.md), [Lua](docs/lua.md), [runtime](docs/runtime.md) |
| Embed the complete app or attach a frontend | [Application embedding](docs/embedding.md), [client](docs/application-client.md), [server](docs/application-server.md) |
| Investigate failures or performance | [Troubleshooting](docs/troubleshooting.md), [benchmarks](docs/benchmarks.md) |
| Build, test or prepare a release | [Development](docs/development.md) |

Source examples are in the repository's `examples/` directory.
Future work is tracked in [the roadmap](https://github.com/c89systems/cai/blob/trunk/ROADMAP.md); known and resolved incidents are in [the issue tracker](https://github.com/c89systems/cai/blob/trunk/BUGS.md).

## License

[MIT](LICENSE).
