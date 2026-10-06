# NoveoDesktop implementation and handoff

Updated 2026-10-04. This records the implemented desktop changes and their verification limits. Older dated entries in the workspace skills folder describe earlier states, including periods when login and chat sync were not implemented.

## Repository and working rules

- Remote: https://github.com/pcpapc172/NoveoDesktop; published branch: `main`.
- Local checkout: `/home/pcpapc172/.gemini/antigravity/scratch/noveodesktop`; local branch: `noveo/bootstrap`.
- This is a separate repository preserving Telegram Desktop history. GitHub routed the original fork request to the existing AyuGramDesktop fork, so NoveoDesktop was created separately.
- Keep Telegram's native interface. The user rejected a standalone Qt prototype. Adapt existing native widgets, history loading, media players, keyboard rendering, and data models wherever possible.
- Read `AGENTS.md` and `REVIEW.md`. Conversation requests are ordinary work, not entries in the separate AI task queue.
- Pull before editing: `git pull --rebase origin main`. Publish with `git push origin HEAD:main`.
- Commit identity: `pcpapc172 <apkme11e@gmail.com>`. Do not add assistant attribution or coauthor trailers.
- Build Debug only. Keep LF line endings. Do not delete caches without explicit authorization.
- Use `gh ... -R pcpapc172/NoveoDesktop`; the local default can otherwise resolve to upstream Telegram.

## Architecture

`AuthClient` handles Noveo authentication and WebSocket reconnect. An account-owned `SessionClient` translates native Telegram request/data types into Noveo WebSocket frames and HTTP requests. Existing native data/session/history/UI code consumes those translated results.

- WebSocket: `wss://noveo.ir:8443/ws`.
- HTTP base: `https://noveo.ir:8443`.
- WebSocket Origin: `https://noveo.ir`. Missing Origin caused HTTP 403 before authentication; the matching header fixes the upgrade.
- HTTP authentication uses `X-User-ID` and `X-Auth-Token`. Do not copy real credentials into documentation, logs, or fixtures.
- The account uses `MTP::Instance::Mode::Noveo`, which suppresses Telegram network sessions/configuration requests. Native TL types and portions of inherited MTProto source remain for compatibility. The entire MTProto source tree has not been removed.
- Tokens are retained in the existing encrypted account authorization stream; passwords are not persisted.
- UUID-derived peer identifiers must respect native peer type bits. The original login assertion came from hashing into bits reserved for peer type; new IDs are masked and unsafe saved self IDs are normalized.

## Implemented features

| Area | Current implementation |
| --- | --- |
| Login | Native username/password form, native validation/errors, renamed welcome screen, Noveo password login and reconnect. |
| Connection status | Native connection indicator remains for an existing session; pre-login errors stay in the login form. |
| Chats and contacts | Native dialogs, contacts, user profiles and avatars populated through the session adapter. |
| History | Existing native history requests mapped to Noveo paging; chronological ordering, older-message loading, reply identifiers and read state maintained. |
| Text | Send acknowledgements and live message updates use native message models. |
| Media | Photos, files, video, GIFs, WebP/TGS/WebM stickers and voice mapped to native media/document types. HTTP uploads, cancellation, multipart assembly and albums have protocol regression coverage. |
| Replies and forwarding | Native IDs mapped back to server UUIDs; forwarded content and source metadata retained. |
| Read receipts | Seen events update native inbox/outbox read maxima for check marks. |
| Avatars | Avoid overwriting an existing avatar with absent photo data during routine refreshes; native avatar gallery requests adapted. |
| Thumbnails | Server thumbnail data mapped to native cached photo sizes; uploaded photo dimensions supplied. |
| Reactions and typing | Native reactions, large-reaction events, typing and interactive emoji events adapted to Noveo. Animated emoji/reaction assets come through Noveo pack endpoints. |
| Favorite stickers | Native favorite-sticker requests mapped to the Noveo sticker API. |
| Mute state | Native notification settings retained locally so routine sync does not reset manual mute/unmute. This is not verified cross-device server-side notification synchronization. |
| Gifts | Catalog, profile gift paging, native gift media, purchase, giveaway, claim and sale actions adapted. |
| Bot messages | Incoming Markdown converted into native entities; native inline URL/callback keyboards rendered and callbacks posted to Noveo. |

Implemented does not mean every live interaction has been tested on both platforms. Protocol fixtures, successful native builds, and user runtime observations are separate evidence.

## Gift and bot fixes from the latest screenshots

Commit `578e44c13c` implements the following; `b4dbcc9ed5` supplies its required Painter include.

1. Replace the native “convert to Stars” preview description with “sell for N Stars”. Profile gifts owned by the current user expose a sale button and confirmation. Successful sales notify the existing native gift list to remove the sold entry.
2. Remove the empty Stars dialog. Eligible Stars claims run directly from the existing gift card; claimed/self-created cards do not open that redundant dialog.
3. Populate both amount and Stars fields of `messageActionGiftStars`, fixing the service header displaying zero. Use Android's bundled `premium_gift.json` animation, compressed as desktop `noveo_stars_gift.tgs`, through the native Lottie icon renderer. No new custom drawing framework was introduced.
4. Suppress sender userpic badges on Noveo profile gift cards.
5. Parse Android-compatible bold, inline code and fenced code into native entities. UTF-16 offsets, code language, indentation, and unclosed delimiters have fixture coverage. Outgoing native bold/code entities are converted back to Markdown.
6. Accept `inlineKeyboard` / `inline_keyboard`, `callbackData` / `callback_data`, and URL buttons. Native callback requests post `{chatId, messageId, callbackData}` to `/bot/callback` with the current authentication headers.

Markdown support here matches the implemented Android subset: bold and code. Full Markdown nesting, italic, strike, spoiler, and Markdown links are not claimed.

Gift actions use `/gifts/purchase` with mode `own` or `gift`, `/gifts/giveaway/claim`, `/stars/giveaway/claim`, and `/gifts/sell`. The inspected backend refunds the gift's catalog `priceTenths` on sale; desktop labels convert the server's 100-unit Star amounts to displayed Stars. In-flight actions suppress duplicate requests, and failed HTTP requests report server errors.

## Important fixes and lessons

| Failure | Fix |
| --- | --- |
| Login HTTP 403 | Send Android-compatible WebSocket Origin. |
| `Unexpected: Peer id type` in `data_session.cpp` | Mask hashed identifiers to native peer ID capacity and migrate unsafe persisted self IDs. |
| Chats/contacts stuck loading with response parse errors | Return correctly boxed native response constructors from the Noveo adapter. |
| Windows access to private streaming setter | Use the public document streaming API. |
| Windows Qt Network link error | Include the required Windows DNS library. |
| `size <= _fullSize` during HTTP photo download | Skip native progressive size upgrades for plain HTTP URL locations; preserve native download invariants. |
| Language generation rejects `{count}` in a non-plural gift key | Use `{amount}` and matching `lt_amount` callsites. |
| `Painter` cannot convert to `QPainter&` for gift animation | Include `ui/painter.h` so the compiler sees `Painter`'s public QPainter base. |
| ORAS installer rejects 1.3.4 | The pinned action's embedded release table stops at 1.3.0. Supply the official 1.3.4 Windows URL and verified checksum explicitly. |

## Source map

- `Telegram/SourceFiles/noveo/auth_client.cpp/.h`: authentication, socket transport and reconnect.
- `Telegram/SourceFiles/noveo/session_client.cpp/.h`: native request translation, messages, history, media, feature APIs, Markdown and keyboards.
- `Telegram/SourceFiles/main/main_account.cpp`: account ownership and Noveo mode.
- `Telegram/SourceFiles/data/data_session.cpp`: native model/media integration and avatar preservation.
- `Telegram/SourceFiles/data/data_cloud_file.cpp`: HTTP photo download size behavior.
- `Telegram/SourceFiles/history/view/media/history_view_premium_gift.cpp/.h`: gift cards and bundled Stars animation.
- `Telegram/SourceFiles/boxes/star_gift_box.cpp/.h`: Noveo gift dialogs and actions.
- `Telegram/SourceFiles/settings/settings_credits_graphics.cpp`: native saved-gift entry routing.
- `Telegram/SourceFiles/info/peer_gifts/info_peer_gifts_widget.cpp`: profile gift sender badge suppression.
- `Telegram/Resources/langs/lang.strings`: native translation keys.
- `Telegram/Resources/animations/noveo_stars_gift.tgs` and `qrc/telegram/animations.qrc`: Android-derived animation and resource registration.
- `.github/workflows/linux.yml`, `.github/workflows/win.yml`: manual Debug builds.
- `.github/scripts/build_cache.py`, `.github/scripts/registry_cache.py`: incremental timestamps and durable Windows snapshots.

Android reference files are in the sibling `noveotg` checkout: `org/telegram/tgnet/NoveoWsClient.java`, `ui/Components/NoveoGiftUi.java`, `ui/Cells/ChatActionCell.java`, and `res/raw/premium_gift.json`. Backend behavior was inspected in sibling `backend/server_backup.py`; this work did not deploy a backend update.

## Verification

Local TLS fixtures compile the actual transport/session adapter and generated native scheme, then exercise them against an isolated aiohttp server. They do not use live credentials or spend real Stars.

```bash
python3 tests/noveo/test_auth_client.py
python3 tests/noveo/test_session_client.py
python3 tests/noveo/test_media_client.py
python3 tests/noveo/test_feature_client.py
python3 -m unittest discover -s .github/scripts -p 'test_*cache.py'
```

These require Python with aiohttp, OpenSSL, g++, pkg-config, the relevant Qt6 development packages, and initialized native support submodules. The existing local Python environment used during this session is `/tmp/noveo-tests-env/bin/python`; temporary paths may disappear between sessions.

- Authentication: nine TLS/login/reconnect/error scenarios passed earlier.
- Session, media and feature suites passed during the gift/bot work, including outgoing formatted text, incoming UTF-16 entities, bot callbacks, nonzero Stars amount, gift sale payload, thumbnail handling and prior media behavior.
- Six build-cache tests passed when Windows registry snapshots were introduced. An actual ORAS local OCI archive round trip also passed.
- The real pinned language generator accepted all current translation keys.
- The bundled gift animation's decompressed JSON exactly matches Android's source asset.
- Latest verified native Linux build: [37220942138](https://github.com/pcpapc172/NoveoDesktop/actions/runs/37220942138), successful at source `b4dbcc9ed5`.
- Latest Windows run checked while documenting: [37221990161](https://github.com/pcpapc172/NoveoDesktop/actions/runs/37221990161), source `4132d30356`, still in `Libraries.`. ORAS installation now passed. Dependency publication, app compilation and artifact completion remain unverified for this run.

## Remaining work and next-session checks

1. Inspect the latest Windows run and registry publication. If it fails, preserve useful preparation/build outputs and fix the concrete error.
2. Verify a subsequent Windows runner restores both GHCR dependencies and compiled output; do not call Windows durable caching proven until that fresh restore succeeds.
3. Test the new Linux artifact's gifts, sale list updates, Stars animation, bot callbacks and Markdown in the real app. Linux compilation passed; the latest runtime screenshots have not been supplied yet.
4. Confirm favorite stickers, avatar gallery, reactions, typing, upload indicators, thumbnails, forwarding and seen ticks across real peers/devices.
5. Investigate cross-device notification setting synchronization only against an actual supported server endpoint; local persistence is currently implemented.
6. Two-factor challenges and unsupported inherited Telegram RPCs remain gaps. Live calls, broad Telegram feature parity, and a complete MTProto source deletion are not implemented claims.
7. Measure actual warm build times. The one-line Painter fix scheduled only a small incremental Linux build, but no fixed five-minute Windows guarantee has been measured.

See [build and cache runbook](noveo-build-cache.md) for dispatch commands, registry formats and cache recovery.

## 2026-10-06 upload completion and Android gift behavior

- Media sends now acknowledge with native `updateMessageID` plus the complete new message, as albums already do. The previous shortened acknowledgement depended on `messageSentData`, which native media sends do not register; the item ID changed, but its media was not converted and its upload circle remained. Text sends retain their existing acknowledgement path.
- Noveo document conversion also clears upload state when the URL-derived document ID is unchanged, matching the existing photo handling.
- Giveaway cards match Android's Gift Giveaway / Stars Giveaway titles, gift name or Stars amount, First claim wins / You shared this gift / You shared these Stars / Already claimed states, and eligible-only Claim button. Claim runs directly without a gift purchase preview. Sender and claimed cards do not fall through to inherited Telegram gift dialogs.
- A successful claim applies a native service-message edit immediately while preserving giveaway artwork and metadata; server sync still follows. The regression fixture deliberately omits the live server edit, proving the HTTP completion updates the card's backing message.
- Gift details retain native animation and layout, show remaining/initial stock, offer Sell/Giveaway for owned profile gifts, and offer Buy for other gifts only when active stock is available. Removed the preview's inactive View button and sale wording on other people's gifts.
- Verification: media and feature TLS fixtures passed; media regression asserts a full acknowledgement with the matching random ID, message ID, caption and document. Feature regression covers successful claim without a live update and sender ownership fallback. Actual language generation and a C++ probe using generated translation types passed. Full native application builds and visual/runtime checks were not run for this change.
