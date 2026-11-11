/** @file Executable-only raw stdin-to-TCP mode, before application startup. */
#ifndef CAI_CLI_FEED_TCP_H
#define CAI_CLI_FEED_TCP_H
/** Return -1 for ordinary startup, otherwise the utility's process exit code.
 */
int cai_cli_feed_tcp(int argc, char **argv);
#endif
