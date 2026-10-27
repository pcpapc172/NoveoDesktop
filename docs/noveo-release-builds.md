# Noveo Desktop release builds

Run the **NoveoDesktop Release** GitHub Actions workflow from the default branch.
It builds Linux and Windows, publishes their archives and SHA-256 checksums as
the requested GitHub release, and records compressed and unpacked sizes. The
100 MB archive size is a target: a larger archive produces a warning but does
not discard an otherwise working build.

Linux packages use `tar.xz` and contain stripped executables. Windows packages
use ZIP and contain optimized executables without separate debugging symbols.
Both include the updater. Linux uses function and data sections with linker
garbage collection; Windows uses `/O2`, `/OPT:REF` and `/OPT:ICF`.

The existing prepared Linux image, Windows dependency snapshot and compiler
caches are reused. Optimized objects and Windows release dependencies have
separate snapshot names so release builds do not overwrite Debug build trees.
The first release compiles optimized objects and may prepare Windows release
libraries. Subsequent release runs restore those release snapshots and reuse
unchanged objects. If Windows dependency preparation fails, its partial
snapshot is saved for the next run after discarding any incomplete Qt build.
The release preparation builds only optimized Qt without debug symbols and
omits Breakpad's symbol-dumping tool; the crash-handler libraries are still built. Cold
dependency preparation is disabled by default and may be explicitly enabled
through the workflow input.

To build Linux on a self-hosted x64 runner, register it with the `noveo-build`
label and select `linux_self_hosted` when dispatching the release workflow.
The runner needs Docker access and `xvfb-run`; its service user keeps a persistent
compiler cache in `~/.cache/noveo-tdesktop-ccache`. The workflow does not run
the GitHub-hosted machine cleanup or install packages on the self-hosted server.
It uses the prepared registry image, which must already exist. The Windows job
still runs on a GitHub-hosted Windows machine. Use an isolated runner account
and machine for trusted code, especially because the repository is public.

The GitHub release tag is independent of the inherited Telegram core version:
keeping that internal version preserves compatibility with existing data.
