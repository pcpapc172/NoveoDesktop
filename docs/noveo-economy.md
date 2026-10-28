# Noveo Stars and Premium

The desktop adapter uses the same wallet, giveaway, Premium Bot and manual receipt-review routes as Android. It keeps native desktop settings, history lists, profile colors, emoji status rendering and dialogs.

`session_stars.cpp` translates native Stars status/history/subscription requests to `/stars/overview`, exposes first-claim giveaways through `/stars/giveaway`, and uploads receipt images through `/payment/request`. The ledger uses hundredths despite the `Tenths` field names. Whole and fractional values are converted to native amount/nanos without floating-point rounding. History deduplicates IDs, preserves descriptions, filters incoming/outgoing amounts and omits message rewards. Pagination covers the history window returned by the overview endpoint; the server does not expose older pages. Balances update from `wallet_updated`, and an older in-flight overview cannot overwrite a newer live balance.

The Stars settings entry is available for free accounts. Its share action chooses an existing chat, asks for an amount and confirms the recipient before submitting. The first recipient to claim receives the Stars, matching Android. Stars topups and Telegram subscription packages are not provided by this backend; their purchase controls are hidden.

Premium settings and upsells open the fixed Noveo Premium Bot and send `/start` through the native send pipeline. Only that bot's `pb:pay` callback opens checkout. The checkout shows Android's existing monthly price/payment instructions, requires acceptance, and submits a local or portal-provided receipt image for manual review. A successful upload reports pending review, never immediate membership activation. Price and destination information currently match Android's checkout constants; both clients must remain aligned with server payment configuration.

`session_premium.cpp` translates profile color and emoji-status requests. It exposes the same 20 palettes as Android, maps Noveo skins to their nearest native palette, renders saved/template icons as native image/TGS custom emoji documents, and saves or clears them through acknowledged `update_profile` actions. Premium and Silver both map to the native Premium badge; the server still enforces their different customization permissions. Partial user updates preserve known membership state. Unsupported temporary/collectible status and name-color forms return explicit errors.

Custom nickname-font rendering and the broader Telegram Premium feature set are not implemented by this adapter. Noveo's existing bot remains responsible for plan management and cancellation.

## Verification

`python3 tests/noveo/test_economy_client.py` compiles the production adapter and tests it against isolated TLS/HTTP/WebSocket fixtures. It covers signed fractional balances, newer-live/older-HTTP ordering, reentrant wallet requests, filtered/deduplicated/paged history, account isolation, member-state merges, status document encoding, profile colors and clears, invalid status rejection, Stars success/failure, live balances and exact receipt multipart from local files and portal bytes. It makes no production purchases or payment submissions.

The existing feature fixture covers gifts, sales, claiming, reactions, bot keyboards, notifications and calls. Full platform builds and user acceptance remain separate checks.


Message uploads now use the same plan boundaries as the server: Free 50 MiB,
Silver 100 MiB, Premium 500 MiB. Oversized prepared attachments are rejected
before upload, and the transport checks again before posting the multipart body.
Receipt images retain their separate 15 MiB cap.

Premium Bot checkout accepts Silver, Premium, and Silver-to-Premium upgrades,
with the requested tier preserved through receipt submission. Settings → Name
font exposes the shared Noveo catalog for Silver/Premium. Name fonts render in
chat-list rows, search rows, conversation headers, and group sender labels, and
profile updates invalidate the native name caches. Bitmap-only catalog fonts
use the system monospace family, matching Android's fallback.

Login device information reports the build version and actual operating system.
