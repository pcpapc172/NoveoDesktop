# Account state sync

The desktop adapter translates native pin/reorder, archive, manual unread, text draft, and ordinary folder requests to `GET/POST /user/sync`. Android uses the same item kinds and raw peer identifiers. `session_sync.cpp` handles translation; `sync_state.cpp` owns versions, tombstones, the persistent queue, and conflict backups.

Account state is stored inside the existing encrypted Noveo authorization record. Authentication checks compare user ID/token, so saving queued state does not invalidate unrelated in-flight HTTP requests. Restoring/reconnecting the same user preserves the queue; logout clears it. Queue cancellation removes the native callback, while the durable user action remains queued. Retries use item versions; revision gaps fetch the authoritative snapshot. A delayed acknowledgment uses its own response version, preserving conflicts with an intervening remote edit.

The initial native state is captured before publishing backend dialogs. Positive local pins/archive/unread settings, ordinary folders/order and plain text drafts are imported once only when no server item or tombstone exists. Existing local replies whose old numeric message ID cannot safely be resolved are not imported. Incoming draft replies resolve server UUIDs through loaded history or the existing context loader. Missing context is attempted once per connection, preventing a retry loop for a deleted message.

Richer media/effect/rich drafts, quoted/topic replies and shared chat-list folders are outside this contract and are not silently stripped on save. Conflicting edits are retained in encrypted `accountSync.conflicts`; there is no recovery UI yet. This is not full MTProto update-log parity.

The adapter also preserves pin/folder/draft fields when a new message refreshes a dialog, exposes a native archive entry, filters folder-specific dialog requests, and returns real pinned dialogs and unread marks. Native widgets/data models receive the results; there is no replacement interface.

## Gift display

Owned Noveo gifts carry their issued number into profile cards and previews. Their ribbon shows `number of total`, matching Android; owned gifts do not show catalog stock status. Catalog gifts retain limited/sold-out behavior. Saved gift handles now populate native message identifiers and point to per-owner instance metadata, keeping the selected issue distinct from catalog data. Missing issue numbers use Android's total-minus-remaining fallback. Unlimited gifts have no limited ribbon; unique gift behavior is preserved.

## Checks

Run from the repository root with Python plus aiohttp and the existing Qt6 development packages:

```sh
python3 tests/noveo/test_sync_state.py
python3 tests/noveo/test_sync_client.py
python3 tests/noveo/test_feature_client.py
```

The first test covers restart persistence, account isolation, explicit clears, in-flight edits, stale snapshots and late acknowledgment versions. The TLS/native integration runs two authenticated sessions against the deployed backend account-state module frozen in `tests/noveo/fixtures/account_sync_server.py`. It covers draft entities/replies, queued edits, a conflicting WebSocket event before the HTTP409 response, pin order, archive, unread marks, folder deletion, clears, and revision-gap refetch. The feature fixture checks gift issue numbers, stock fields, saved handles and per-instance lookup alongside its existing messaging/payment checks. No live credentials or production user data are used by these fixtures.

Full Windows/Linux builds and live Android-to-desktop user acceptance are separate checks.
