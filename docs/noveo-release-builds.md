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
snapshot is saved for the next run. The release preparation omits Breakpad's
symbol-dumping tool; the crash-handler libraries are still built. Cold
dependency preparation is disabled by default and may be explicitly enabled
through the workflow input.

The GitHub release tag is independent of the inherited Telegram core version:
keeping that internal version preserves compatibility with existing data.
