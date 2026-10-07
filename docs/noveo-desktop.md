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
6. Two-factor challenges and unsupported inherited Telegram RPCs remain gaps. Live calls still need artifact/device verification; broad Telegram feature parity and a complete MTProto source deletion are not implemented claims.
7. Measure actual warm build times. The one-line Painter fix scheduled only a small incremental Linux build, but no fixed five-minute Windows guarantee has been measured.

See [build and cache runbook](noveo-build-cache.md) for dispatch commands, registry formats and cache recovery.

## 2026-10-06 upload completion and Android gift behavior

- Media sends now acknowledge with native `updateMessageID` plus the complete new message, as albums already do. The previous shortened acknowledgement depended on `messageSentData`, which native media sends do not register; the item ID changed, but its media was not converted and its upload circle remained. Text sends retain their existing acknowledgement path.
- Noveo document conversion also clears upload state when the URL-derived document ID is unchanged, matching the existing photo handling.
- Giveaway cards match Android's Gift Giveaway / Stars Giveaway titles, gift name or Stars amount, First claim wins / You shared this gift / You shared these Stars / Already claimed states, and eligible-only Claim button. Claim runs directly without a gift purchase preview. Sender and claimed cards do not fall through to inherited Telegram gift dialogs.
- A successful claim applies a native service-message edit immediately while preserving giveaway artwork and metadata; server sync still follows. The regression fixture deliberately omits the live server edit, proving the HTTP completion updates the card's backing message.
- Gift details retain native animation and layout, show remaining/initial stock, offer Sell/Giveaway for owned profile gifts, and offer Buy for other gifts only when active stock is available. Removed the preview's inactive View button and sale wording on other people's gifts.
- Verification: media and feature TLS fixtures passed; media regression asserts a full acknowledgement with the matching random ID, message ID, caption and document. Feature regression covers successful claim without a live update and sender ownership fallback. Actual language generation and a C++ probe using generated translation types passed. Full native application builds and visual/runtime checks were not run for this change.

## 2026-10-06 giveaway access, connection status and proxies

The ordinary Noveo gift dialog now offers Giveaway wherever it is opened: catalog/send previews, own/other profile gifts and normal chat gift details. Selling remains restricted to owned profile gifts; buying still requires active stock. Giveaway keeps the native recipient picker and confirmation before buying a copy for the selected chat. Existing giveaway messages retain their claim-only behavior.

The native bottom connection widget now shows Connecting text without hovering and remains visible while disconnected even when an application update is ready. Account network reachability feeds the Noveo connection state and immediately resumes a saved-token session when the network returns. The user confirmed that the preexisting WebSocket heartbeat/retry already reconnects after a real internet interruption; the initial apparent failure was reported before disconnecting.

Native proxy settings are applied explicitly to the account WebSocket and HTTP manager. SOCKS5 (including credentials) and HTTP CONNECT use Qt's network proxy support; changing, enabling or disabling the selected proxy reconnects an active WebSocket without retaining login passwords. Existing global proxy settings still route native URL downloads. System/disabled settings retain their native behavior. MTProto/Web proxy protocols are not Noveo WebSocket tunnels.

For Noveo accounts, the native proxy list, imported-proxy status checks and rotation checks now probe `wss://noveo.ir:8443/ws` via the selected SOCKS5/HTTP proxy instead of Telegram DCs, sending a WebSocket control ping and reporting elapsed time or failure with a ten-second timeout. Android's checker uses `/ping`, but a live check found that endpoint returns HTTP 404; desktop uses the working socket endpoint instead. No account credentials are sent. A live direct probe received the matching pong from `/ws`; live UI and an external proxy remain unverified.

Validation: 16 local TLS transport scenarios passed (including heartbeat loss, offline/online, authenticated SOCKS5, proxy switching/disabling and proxy probe success/failure). The feature adapter suite passed with both WebSocket and HTTP routed through authenticated SOCKS5. Native proxy checker C++ syntax passed against generated protocol headers; no full desktop application compile or live GUI/proxy test was performed locally.


## 2026-10-06 Linux notifications and Noveo calls

Notification popups use `WA_ShowWithoutActivating`. On COSMIC Wayland, notification urgency no longer invokes Qt's activation-producing alert path. Explicit activation after restoring a tray-hidden window is retried on the next event-loop turn through public `QWindow::requestActivate()`. Native notification action targets survive a close-before-action ordering for five seconds, guarded against replacing an existing notification. These changes need testing on the user's actual COSMIC desktop.

The earlier Windows and Linux runs both failed because the new proxy checker used `emit` with `QT_NO_KEYWORDS`; it now uses `Q_EMIT`. Both builds restored caches before that compilation failure. AyuGram at `/home/pcpapc172/tdesktop` (origin `AyuGram/AyuGramDesktop`, revision `db3b9891cb`) was inspected read-only. Its original Linux notification source matches this checkout; its calls use Telegram signaling rather than Noveo/LiveKit.

The native call controls now use Noveo account signaling and an isolated bundled LiveKit 2.18.1 media bridge. Private calls ring and can be answered/declined; group chats expose a join/start button without requiring Telegram group-call RPCs. `/voice/token` uses the account HTTP authentication, while only the room token enters the media page. The local document has Noveo's HTTPS origin, matching Android and the reviewed server Origin checks; token responses accept HTTPS/WSS media endpoints. `voice_join` follows successful media connection, and leave/sharing actions are sent once. Canonical DM IDs, active-call snapshots, remote end, reconnect, cancellation and logout are handled. Completed/missed private call logs become native phone-call service bubbles.

Audio mute and native microphone/speaker selection feed LiveKit. Native camera/screen capture and remote video feed the existing call panel through a bounded JPEG bridge (8 fps); system audio sharing is disabled. Group calls currently use that panel with a participant count, rather than a full participant roster or tiled conference layout. Media uses WebKitGTK/WebView2 and needs their WebRTC support; its ICE/media traffic is not verified through the account proxy. No live Android/desktop call has been tested.

A build-directory overlay opts only call webviews into media permissions and autoplay and intercepts only their bundled HTTPS resource path. The pinned vendor checkout remains clean. Byte-identical overlay output preserves modification times for incremental builds; unexpected vendor patch anchors fail configuration explicitly. The LiveKit SDK license is bundled.

Validation: the actual Qt call controller and bundled JavaScript media tests pass; the authenticated SOCKS5/TLS feature fixture passes 35 assertions including room tokens, signaling and call logs. Actual CallMedia and proxy-checker syntax checks passed against Qt and pinned protocol/webview headers. Overlay regeneration preserved file mtimes, and whitespace checks passed. Native call panel compilation and real notification/media behavior remain for Debug CI and artifact testing; local protocol tests are not full application builds.

## 2026-10-07 native account/chat/message parity

`noveo/session_actions.cpp` adapts native message edits/deletions, polls/votes/public voters, pins, text/public search, unloaded reply context, profiles/contacts/avatar uploads, group/channel creation and membership/admin operations, devices/revocation, blocking and group-invite privacy to Noveo. Serialized WebSocket mutations complete on acknowledgements and match available request/chat/message IDs; disconnect/cancellation clears them instead of replaying mutations. `request_fields.h` can be regenerated with `python3 tests/noveo/generate_request_fields.py` against the pinned protocol schema.

TOTP login now presents a six-digit challenge with retry/cancellation. Chat role and permission data is hydrated from `/chat/settings` and `/chat/member_permissions`, because history alone omits administrator/manager rights. Native group default permissions and member overrides expose Noveo's actual controls, including reset to inherited defaults. Noveo security settings expose supported password changes, devices, blocked users, group invites and local passcode. Password changes persist the rotated token/session ID. Chat profile updates read the current profile and preserve the other field because the server requires both name and bio. Channel creation saves its description after multipart creation.

Poll option IDs are preserved exactly as UTF-8, including web-created IDs that resemble base64. Native rendering reads sanitized option counts and viewer choices; voter details and results honor the server's visibility flags. Text search across known dialogs is bounded by the server's per-chat result limit. Shared-media filtered search remains unsupported.

This batch was checked against a read-only copy of the running `server_optimized_giftfix.py`. That server has an independent group deletion bug: `handle_delete_message(data, deleter_id)` references undefined `user_id` in its group permission check. Desktop deletion and live cache updates are implemented, but group deletion needs that backend fix. The server also lacks self-avatar removal and poll-close endpoints; unsupported requests fail explicitly. No server deployment was performed.

Android's active proxy UI selects/saves SOCKS5; its MTProto branches are legacy. Desktop offers SOCKS5 and HTTP and rejects MTProto/web proxy imports. Removed Telegram web proxy socket/frame/transport/webview code and special config/bootstrap fetching. Native protocol types and the RPC facade remain required by the inherited UI. Linux's final copied Debug artifacts use `strip --strip-all` plus removal of nonruntime compiler comment sections; dependency/incremental caches are preserved.

Validation: 32-step native protocol/TLS parity probe, session, media and feature regressions passed; auth passed 18 transport/TOTP scenarios; call controller and bundled media fixtures passed. These compile production transport/actions against generated protocol types. Full application compilation is delegated to the normal Windows/Linux Debug workflows. Actual GUI interactions, COSMIC notification behavior and Android/desktop media interoperability still require artifact testing.
