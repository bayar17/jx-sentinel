# JX Sentinel Guard

JX Sentinel Guard is a Linux-native protected-folder permission layer for JX/OS.
It uses fanotify permission events in a root-owned daemon and delegates user
prompts to a GNOME session agent over `/run/jx-sentinel/guard.sock`.

## Components

- `jx-sentinel`: existing non-blocking creation monitor.
- `jx-sentinel-guard`: root enforcement daemon using `fanotify_init`,
  `fanotify_mark`, permission-event reads, and `struct fanotify_response`.
- `jx-permission-agent`: GTK user-session agent that shows Zenity prompts.
- `jx-sentinel-control`: GTK control panel with Guard status wiring.

## v1 Identity Model

Linux does not provide the same universal application identity model as macOS
TCC. Version 1 matches persistent rules by resolved executable path, UID, and
protected root. Future versions should add executable SHA-256, package owner,
desktop file ID, parent process, and command-line fingerprinting.

## Policy Store

The v1 policy store is file-backed at:

`/opt/jx/var/lib/jx-sentinel/permissions.db`

Each line contains:

`executable<TAB>uid<TAB>protected_root<TAB>decision<TAB>scope`

SQLite can replace this format later without changing the guard/agent protocol.

## Allowlists

`Allowlist=` entries match exact resolved executable paths, such as
`/usr/bin/nautilus`.

`ProcessAllowlist=` entries match an executable basename or the first command
line token, such as `nautilus`. Use this for trusted apps where any PID for that
app should access protected roots without prompting.

## Manual Tests

1. Install and start the guard service.
2. Start the GNOME permission agent with
   `systemctl --user enable --now jx-permission-agent.service`, or run
   `/opt/jx/bin/jx-permission-agent` in the user session.
3. Protect `/home/jackson/Desktop`.
4. Run `cat /home/jackson/Desktop/test.txt`.
5. Choose `Deny Once`; expect the guarded open to fail before the file access completes.
6. Repeat and choose `Allow Once`; expect later accesses under the same
   protected root to use the session cache briefly.
7. Choose `Always Allow`; future accesses by the same executable and UID should
   succeed without prompting.
8. Stop the agent and retry; unknown apps should follow `DefaultDecision`.
9. Check logs with `journalctl -u jx-sentinel-guard.service -f`.

`Allow Once` and `Deny Once` decisions are cached in memory for 60 seconds by
executable path, UID, and protected root. That means one decision for a process
touching `/home/jackson/Desktop` also covers existing subfolders under Desktop
during that short window, instead of prompting once per file.

The prompt path is healthy only when both of these exist:

- `/run/jx-sentinel/guard.sock`
- `/run/jx-sentinel/agent.connected`

If only `guard.sock` exists, Guard is running but the user prompt agent is not
connected, so no Zenity prompt can open.

If an XDG folder is a symlink, for example `/home/jackson/Desktop` pointing to
`/media/psf/Home/Desktop`, Guard resolves and marks the real directory. If that
target filesystem does not support fanotify permission events, startup logs will
show `fanotify_mark failed` for the resolved path.

Commands run through `sudo` are evaluated as the root-owned process that opens
the file. They can still prompt, but persistent rules are separate because v1
matches executable path plus UID.

## Limitations

The daemon recursively marks existing protected directories and their existing
subdirectories. File creation that goes through `open(O_CREAT)` is guarded by
`FAN_OPEN_PERM`, so the process waits for the Guard decision before the open
completes. Linux fanotify does not provide `FAN_CREATE_PERM`, so pure
directory-entry creation notifications such as `FAN_CREATE` are observable but
not deniable after the kernel has created the entry. New subdirectories created
after Guard startup should be added to the protected path list or covered after a
Guard restart in this v1 implementation.
