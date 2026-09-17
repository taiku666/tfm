# Security Policy

TFM is a local, single-user file manager - it has no network component,
no remote input, and runs with the invoking user's own permissions. Its
main security-relevant surfaces are file-path/config handling and the
`$SHELL`/`$EDITOR` launch paths, all of which act on the user's own,
locally-typed input under the user's own privileges (the same trust model
as a terminal shell).

## Reporting a vulnerability

If you find a security issue (e.g. a symlink-following bug that could
cause data loss or unintended file exposure, a path-handling bug that
escapes the intended directory, or privilege/permission handling that
doesn't match what's documented), please open a GitHub issue describing:

- The affected file/function.
- Steps to reproduce (ideally against a disposable `/tmp` directory tree).
- What you expected vs. what happened.

There is no bug bounty; this is a personal/community project maintained
on a best-effort basis. Given TFM's single-user, no-network trust model,
public issues are fine for most reports - only file a private report if
the issue would let a *different, unprivileged local user* affect the
invoking user's files (a genuine privilege boundary violation).

## Known non-issues

- The shell command bar in both front-ends runs the user's own typed
  command via `/bin/sh -c` - this is the file manager's intentional
  command-bar feature, not injectable from any untrusted source: no
  data from a file, filename, or any other non-interactive source is
  ever spliced into the command string.
- `$EDITOR` is invoked with the user's own environment, as expected of
  any tool that shells out to the user's configured editor.
