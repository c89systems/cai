# Skills

Smith runtimes discover skill packages under `${XDG_CONFIG_HOME:-$HOME/.config}/cai/skills`, or `--config-dir DIR`'s `skills` directory.
`--skills-dir DIR` and `[paths] skills_dir` override that root.
The catalog's names and descriptions enter developer instructions; the model calls `read_skill` to load a selected `SKILL.md` and its referenced text resources.
Skill selection happens when requested by name or when the task matches the description.
Global skill files do not become workspace files.

`/skills` shows the active runtime's loaded skill names and descriptions in a Markdown table.
Name one in your prompt, for example `Use the my-skill skill for this task.` The command also works during a turn and does not rescan files or make model requests.
It reports disabled or empty catalogs explicitly.
SDK hosts can visit the same catalog with `cai_agent_runtime_visit_skills`; Lua exposes copied `{name, description}` entries through `runtime:skills()`.

```sh
cai --install-skill ./my-skill
cai --install-skill ./my-skill --force
cai --skills-dir ./skills --install-skill ./my-skill
```

Installation validates the YAML front matter and copies the complete package, including nested/hidden regular files and executable scripts, without running them.
The installed directory is named from skill metadata.
Existing skills require explicit `--force` or `--yes`; publication is atomic and a failed copy preserves the previous package.
Links, devices and FIFOs are rejected.
Examples containing their own `SKILL.md` inside a package remain artifacts, not additional catalog entries.
Skill changes are discovered when the runtime is opened again.

YAML names/descriptions support quoting, folded/literal scalars and aliases.
Unknown metadata is ignored; catalog descriptions collapse whitespace.
`cai_skill_metadata_open` and its receiver getters provide the same validation to SDK hosts.
Front matter is buffered up to 1 MiB; the Markdown body is not loaded during catalog discovery.
`read_skill` limits each text resource to 1 MiB.
Lua uses `cai.skill_metadata(text_or_chunk_reader, fallback_name)` and `metadata:name()`, `metadata:description()`, `metadata:close()`.

In the terminal, `$` completion selects a catalog name and removes the trigger before submission.
See [terminal input](terminal.md#input-and-queue) and [the current completion limitation](troubleshooting.md#completion-triggers).
