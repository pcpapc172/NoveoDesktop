# Google Drive build-cache credentials

| Setting | Purpose |
| --- | --- |
| Repository secret `NOVEO_RCLONE_CONFIG` | Dedicated rclone configuration and OAuth credentials for the shared Noveo Google Drive build-cache account. |
| Repository variable `NOVEO_DRIVE_CACHE_INFO` | Visible, non-secret description of these credentials in GitHub Actions settings. |
| rclone remote | `noveocache` |
| Google scope | `drive.file`: files created or explicitly opened by the rclone application. |
| Reserved folder | `noveocache:noveo-build-cache/desktop/` |

The secret is present for future build-cache integration. **It is not currently used by
this repository's workflows.** Adding it does not change the existing caches or trigger a build.
The same configuration is also stored in the other Noveo client repositories.

Use a distinct subfolder for each operating system, CPU architecture, build configuration,
and compatible toolchain/dependency version. Do not restore Android, desktop, and iOS caches
into one another or delete another client's cache directory. Android should use this
repository's reserved folder; desktop should further separate Windows, Linux, and macOS.

Example for a future workflow (not enabled by this document):

```yaml
# Google Drive build-cache credentials. Never echo or upload this config.
env:
  NOVEO_RCLONE_CONFIG: ${{ secrets.NOVEO_RCLONE_CONFIG }}
  RCLONE_CONFIG: ${{ runner.temp }}/noveo-rclone.conf
# Write the secret to RCLONE_CONFIG with mode 0600, use the reserved folder above,
# and remove the temporary credential file in an always() cleanup step.
```

Only trusted workflows should receive this OAuth credential. Keep it out of fork PR jobs,
logs, artifacts, source control, and caches. Google handles the interactive authorization;
repository collaborators see the secret's name, not its value. Refresh or revoke the shared
credential consistently across all Noveo repositories when rotating it.
