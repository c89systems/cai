# Startup identity requirements

Workspace transports accept application-defined named fingerprints through
`workspace_claims.h`. The server copies an immutable inventory at open; clients
copy only their requested subset. Core transport assigns no configuration
meaning to those names. Native hosts can reuse this admission boundary for a
different application, and the typed application client forwards it unchanged.

HELLO is a definite `[workspace, schema, expected_instance, requirements]`.
PROBE is a definite `[workspace, schema, requirements]`. Requirements are a
definite array of definite `[name, digest]` pairs, strictly increasing by name.
Names are ASCII letters/digits, dots, hyphens and underscores, 1–80 bytes;
digests are exactly 32 bytes. At most 64 pairs are accepted. Empty requirements
are the default. This is one current schema, with no legacy greeting parser or
protocol-version negotiation.

The producer derives the frame length directly from known field lengths and
emits tokens through the normal bounded writer. The receiver retains only its
current/previous name, first mismatched name and comparison offsets, comparing
digest fragments directly against the copied inventory. It does not collect
the encoded greeting or a CBOR value. Metadata inventory copies are bounded;
they contain no transcript, tool output or model context.

A missing or unequal requested fingerprint is provisional until protocol
COMPLETE. The server then emits REJECT with a byte-string payload containing
only the first mismatched name, zero envelope metadata, and closes after its
bounded pending output drains. The ordinary greeting deadline bounds an
unresponsive rejected peer. No attachment, generation, controller, producer,
first-attachment grace or last-detach transition is acquired or changed.
Malformed/truncated greetings are never admitted. STARTING owners can reject
incompatibility before their runtime becomes ready.

Clients publish the rejection only after the complete record validates. The
diagnostic identifies the canonical workspace and setting and advises using
matching settings or stopping that server. It does not reveal the configured
value. Discovery neither publishes nor gathers the inventory. Existing owner
instance pinning remains independent and is checked before attachment.

## Application projection

`cli/workspace_fields.h` is the shared startup inventory used by the primary
parser, INI/environment/host configuration and fingerprint producer. It covers
the configured server name/persistence, config-file identity, private roots,
namespaces, selected stores and keys, provider/endpoint/auth sources,
instructions, integrations, plugins and tool policy. Explicit values equal to
defaults are still requirements. Another option's value cannot become one:
the primary parser records the actual consumed member instead of rescanning
argv. Derived automatic paths do not become explicit requirements.

The application hashes the field name and resolved startup value with SHA-256,
separating absent and present values. The selected config filename is canonical;
`--no-config` requests absence. Store selections also hash their configured
endpoint/key/certificate definition, including resolved default-store paths.
Config identity uses `cai_config_origin`, the actual successfully loaded source,
so changing configured workspace/instruction roots never invents another loaded
filename. Failed loads and later scalar/path/default updates preserve that
origin; the config API exposes it without another allocation or file read.
Named-store key/certificate paths resolve once in the shared configuration
constructor, before either receiver computes identity. Relative paths use the
loaded source's directory, including when configuration redirects the workspace.
Thus the same store label or INI filename cannot conceal a different selected
store definition. Digests are not an authorization mechanism: Unix peer UID,
private endpoints and controller generations still supply authority. Never use
this inventory to publish credentials or grant access based on a hash.

Only explicitly selected identity requirements constrain a running owner.
Frontend theme/layout preferences remain local; the existing model/effort
startup controls retain their acknowledged queue path. Incompatible settings
fail before prompt history, replay, startup instructions or producer binding.
Both the executable and `cai_app_terminal_run` use the same projection, merging
explicit requirements supplied by an embedding host. No configuration file or
running server configuration is rewritten.
Requirements are scoped to that initial target. A later explicit `/workspace`
selection verifies its own directory/instance and does not inherit the first
workspace's store/tool configuration or startup instructions.

Full startup-control parity and extended native configuration outside this
shared inventory remain part of the client/server cutover. In particular,
this boundary does not claim that every runtime-setting flag has already been
mapped to its live backend control.

Verification covers fragmented names/digests, duplicate/invalid names, inventory
limits and owned copies; STARTING/READY mismatch, probes, unknown settings,
nonpersistent grace and existing-controller invariants; actual CLI/native
terminal attachment; explicit-default flags; instruction values resembling
flags; matching config/theme settings; and changed named-store definitions.
