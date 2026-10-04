# NoveoDesktop Debug builds and durable caching

Updated 2026-10-04. Feature status and verification history are in [the implementation handoff](noveo-desktop.md).

## Build workflows

Both workflows use manual dispatch, Debug builds, per-platform concurrency with `cancel-in-progress: false`, and artifacts retained for 14 days. Windows targets x64/Qt6 with Ninja Multi-Config. Packages write permission enables registry publication using the existing workflow `GITHUB_TOKEN`; no personal token or Google Drive setup is required.

```bash
# Normal Linux application build.
gh workflow run linux.yml -R pcpapc172/NoveoDesktop --ref main

# Normal Windows application build: fail quickly if all compatible dependency snapshots are missing.
gh workflow run win.yml -R pcpapc172/NoveoDesktop --ref main

# One-time Windows bootstrap or an intentional dependency/toolchain refresh.
gh workflow run win.yml -R pcpapc172/NoveoDesktop --ref main \
  -f build_missing_environment=true -f environment_only=false

# Seed Windows dependencies without compiling the app.
gh workflow run win.yml -R pcpapc172/NoveoDesktop --ref main \
  -f build_missing_environment=true -f environment_only=true

# Verify the Linux environment restore without permitting a cold build.
gh workflow run linux.yml -R pcpapc172/NoveoDesktop --ref main \
  -f build_missing_environment=false -f environment_only=true
```

Linux defaults `environment_only=false`, `build_missing_environment=true`. Windows defaults both to false. `environment_only` prepares/publishes dependencies and skips the application/artifact steps; it does not seed compiled application objects.

GitHub concurrency still allows only one pending run per group; another dispatch can replace an older pending run even when active runs are not cancelled. Preserve active preparation work unless cancellation is explicitly justified. Do not dispatch new builds for documentation-only changes.

## Why caches disappeared

Earlier successful Windows jobs restored roughly 325 MB ThirdParty, 252 MB libraries, 390 MB Qt, and 1,922 MB incremental output. Later jobs missed identical dependency keys. At investigation time the repository's listed cache entries were Linux-only and totalled roughly 11.47 GB: two large compiler cache snapshots, two output snapshots, and a prepared environment image.

Automatic eviction under the shared Actions cache allowance is the supported explanation; the exact eviction event was not audited. Do not describe it as a changed-key failure or a manual deletion. No caches were deleted during the durable registry migration.

A second problem was dependency caches saved through `actions/cache` post-job hooks only on whole-job success. An application compile failure could discard hours of completed dependency preparation. Windows now publishes dependencies before application compilation.

## Linux

The prepared Docker build environment is stored as:

```text
ghcr.io/pcpapc172/noveodesktop-linux-build:<BUILD_ENV_KEY>
```

The workflow hashes its environment inputs, logs into GHCR, pulls the compatible image first, falls back to the legacy Actions image archive if necessary, and only builds dependencies when neither is available and cold building is allowed. New/fallback-loaded images are published before the application build.

Native output and ccache remain in Actions cache. Linux packaging strips staged copies of Telegram and Updater with `strip --strip-unneeded`; original build outputs stay intact for incremental compilation. A previously tested Telegram copy shrank from 1,251,300,888 to 461,637,208 bytes. Current artifact size depends on the build.

Verified migration and fresh image-only restore:

- [37212865244](https://github.com/pcpapc172/NoveoDesktop/actions/runs/37212865244): successful initial registry publication.
- [37213128512](https://github.com/pcpapc172/NoveoDesktop/actions/runs/37213128512): successful fresh runner pull; dependency rebuilding and Actions image restore skipped.
- Verified image key: `c6ccf3458d5db8f53f67686b351b32bc680230ba5eca219330f61b530a2577a0`.
- Verified digest: `sha256:dfc616fa8f5261ea60e45fec6d6f066a572201e6f38ffa88049eb72b0e222e0d`.

## Windows

Windows snapshots use ORAS OCI artifacts, separate from the Linux Docker image:

```text
ghcr.io/pcpapc172/noveodesktop-windows-cache:deps-<DEPENDENCY_KEY>
ghcr.io/pcpapc172/noveodesktop-windows-cache:out-<BUILD_SNAPSHOT_KEY>
```

Dependency identity includes snapshot format, preparation/SDK cache key, MSVC tool version, architecture and Qt choice. Output identity hashes dependency identity plus compiler/CMake identity. Application commits do not change these identities, so ordinary source edits can reuse compatible outputs. Hashing the output identity also keeps the tag within registry tag length limits.

Restore/build/publish order:

1. Install ORAS and log into GHCR using password stdin.
2. Restore exact registry dependencies; fall back to legacy ThirdParty/library/Qt Actions caches when absent.
3. Require explicit `build_missing_environment=true` if neither a registry snapshot nor all exact legacy dependency caches exists.
4. Run the existing dependency preparation script and disk pruning, then publish missing dependency snapshots before compiling Telegram.
5. Restore the registry output snapshot; fall back to the compatible legacy Actions output prefix when absent.
6. Restore timestamps of unchanged checkout inputs using their content hashes, then configure/build Debug with sccache.
7. Record input hashes/timestamps and publish changed output, including useful objects from failed compilation, when input recording succeeds and the job is not cancelled.
8. Stage Telegram.exe and Updater.exe and upload the artifact after a successful application build.

Registry publication is restricted to `main`. Dependency snapshots contain `ThirdParty` and `Libraries/win64`; output snapshots contain `out`. They do not archive the entire checkout or its authentication configuration.

`registry_cache.py` packages compressed tar plus a version/key/path manifest. It resolves rolling tags to immutable digests before pulling, validates identity and allowed paths, rejects links/path traversal, and reports unavailable snapshots as cache misses. Registry output snapshots are outside the shared Actions cache quota, although sccache's configured backend remains Actions cache.

### ORAS installation fix

Pinned action: `oras-project/setup-oras@22ce207df3b08e061f537244349aac6ae1d214f6`. Its embedded release metadata stops at 1.3.0, so `version: 1.3.4` failed even though ORAS 1.3.4 exists.

The workflow now supplies:

```yaml
url: https://github.com/oras-project/oras/releases/download/v1.3.4/oras_1.3.4_windows_amd64.zip
checksum: ffdb6aa40267686b5d507da1f21a57fc502a9a7c86b90c54557d335644c99dbd
```

The action supports this explicit URL/checksum path, verifies the download, extracts the ZIP and adds ORAS to PATH. The downloaded archive was independently hashed and confirmed to contain `oras.exe`. Workflow lint passed. Latest run [37221990161](https://github.com/pcpapc172/NoveoDesktop/actions/runs/37221990161) passed installation and reached dependency preparation; registry seeding/fresh restore remains pending.

## Incremental timestamp preservation

`.github/scripts/build_cache.py` records tracked source hashes and modification times in the output directory. After checkout it restores old times only for unchanged content, leaving edited/new files fresh. Ninja then reuses outputs while still rebuilding actual edits. Fingerprints of Ninja logs/dependency state avoid uploading unchanged output repeatedly.

Regression checks include a real Ninja build proving unchanged inputs require no work and changed inputs rebuild. Registry archive round-trip tests also preserve useful incremental output. These tests do not establish a fixed end-to-end runtime: checkout, transfers, configuration, linking and artifact upload still cost time.

## Diagnose a run

```bash
gh run list -R pcpapc172/NoveoDesktop --limit 10
gh run view RUN_ID -R pcpapc172/NoveoDesktop --json jobs
# Direct job logs were useful when run-level logs were unavailable.
gh api repos/pcpapc172/NoveoDesktop/actions/jobs/JOB_ID/logs > /tmp/noveo-job.log
rg -n 'FAILED:|error:|Error:|snapshot|Registry|cache' /tmp/noveo-job.log
```

Read the concrete error before rebuilding dependencies. Common failures have included language code generation, private API access, missing Windows link libraries, incomplete Painter headers, ORAS metadata and artifact staging. These are source/workflow issues, not automatically cache misses.

A missing compatible dependency snapshot requires one intentional warm-up. A persistent local or self-hosted runner can retain dependencies/output directly, but no runner provisioning or Google Drive cache integration was performed. Colab GPU capacity does not directly accelerate this CPU-heavy C++ build, and temporary environments do not provide the same persistent build tree.
