"""Native response/protocol integration against a local TLS server; no live credentials."""
import asyncio
import base64
import json
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
from aiohttp import web
from socks_fixture import SocksFixture

ROOT = Path(__file__).resolve().parents[2]

async def main():
    with tempfile.TemporaryDirectory(prefix="noveo-session-") as directory:
        tmp = Path(directory)
        cert, key = tmp / "cert.pem", tmp / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
        scheme = tmp / "scheme"
        subprocess.run(["python3", str(ROOT / "Telegram/SourceFiles/codegen/scheme/codegen_scheme.py"),
                        "-o", str(scheme), str(ROOT / "Telegram/SourceFiles/mtproto/scheme/mtproto.tl"),
                        str(ROOT / "Telegram/SourceFiles/mtproto/scheme/api.tl")], check=True)
        includes = [tmp, ROOT / "Telegram/SourceFiles", ROOT / "Telegram/lib_base", ROOT / "Telegram/lib_tl",
                    ROOT / "Telegram/lib_rpl", ROOT / "Telegram/lib_crl", ROOT / "Telegram/ThirdParty/GSL/include",
                    ROOT / "Telegram/ThirdParty/range-v3/include"]
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "Qt6Core", "Qt6Network", "Qt6Gui"], text=True).split()
        binary = tmp / "probe"
        subprocess.run(["g++", "-std=c++20", "-fPIC", "-O0", "-ffunction-sections", "-fdata-sections",
                        *("-I" + str(path) for path in includes),
                        str(ROOT / "tests/noveo/feature_client_probe.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/session_client.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/auth_client.cpp"), str(ROOT / "Telegram/lib_tl/tl/tl_basic_types.cpp"), str(scheme) + ".cpp",
                        str(ROOT / "Telegram/SourceFiles/data/data_peer_id.cpp"), "-include", str(tmp / "scheme.h"),
                        "-Wl,--gc-sections", "-o", str(binary), *flags], check=True)
        observed = {"reaction": [], "typing": [], "purchase": [], "claim": 0, "fave": 0, "sell": 0, "callback": 0, "voice": [], "voice_token": 0}
        user = {"userId": "test-user", "username": "Self", "avatarUrl": "https://localhost/avatar.png"}
        other = {"userId": "other-user", "username": "Other"}
        gift = {"giftId": "gift-one", "giftNumber": 1, "name": "Fixture gift", "imageUrl": "https://localhost/gift.gif", "priceTenths": 1000}
        giveaway = {"giveawayId": "giveaway", "giverUserId": "other-user", "status": "open", "gift": gift}
        thumb = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII="
        messages = [{"messageId": "emoji", "senderId": "test-user", "timestamp": 1700000000, "content": {"text": "❤️"}},
                    {"messageId": "photo", "senderId": "other-user", "timestamp": 1700000001, "content": {"file": {
                        "url": "https://localhost/photo.png", "type": "image/png", "size": 100, "thumb": thumb}}},
                    {"messageId": "gift", "senderId": "other-user", "timestamp": 1700000002, "content": {"giftGiveaway": giveaway}}]
        messages.extend([
            {"messageId": "stars", "senderId": "test-user", "timestamp": 1700000004,
             "content": {"starGiveaway": {"giveawayId": "stars-giveaway", "amountTenths": 100000, "status": "claimed"}}},
            {"messageId": "bot", "senderId": "other-user", "timestamp": 1700000005,
             "content": {"text": "😀 **A new login** `inline`\n```cpp\n  code\n```\n**unclosed",
                         "inlineKeyboard": [[{"text": "It was me", "callbackData": "ack:session"}, {"text": "Open", "url": "https://noveo.ir"}]]}}])
        chats = [{"chatId": "group", "chatType": "group", "chatName": "Group", "members": ["test-user", "other-user"], "messages": messages},
                 {"chatId": "channel", "chatType": "channel", "chatName": "Gifts!", "messages": [
                    {"messageId": "channel-gift", "senderId": "system", "timestamp": 1700000003,
                     "content": {"text": "[gift](https://web.noveo.ir/gift/gift-one)"}}]}]
        chats.append({"chatId": "dm", "chatType": "private", "members": ["test-user", "other-user"], "messages": [
            {"messageId": "completed-call", "senderId": "system", "timestamp": 1700000100,
             "content": {"text": "Call", "callLog": {"callId": "completed", "status": "completed", "startedByUserId": "test-user", "durationSeconds": 42}}},
            {"messageId": "missed-call", "senderId": "system", "timestamp": 1700000200,
             "content": {"text": "Call", "callLog": {"callId": "missed", "status": "missed", "startedByUserId": "other-user", "durationSeconds": 0}}},
        ]})
        sockets = []
        def authorize(request):
            assert request.headers["X-User-ID"] == "test-user" and request.headers["X-Auth-Token"] == "test-token"
        async def contacts(request):
            return web.json_response({"contacts": [other]})
        async def catalog(request):
            return web.json_response({"gifts": [gift]})
        async def profile(request):
            authorize(request)
            assert request.query["userId"] == "test-user"
            return web.json_response({"success": True, "profile": {**user, "gifts": [{**gift, "quantity": 2}]}})
        async def pack(request):
            authorize(request)
            animations = request.path.endswith("interactions")
            return web.json_response({"success": True, "set": {"id": "123", "title": "Effects" if animations else "Emoji",
                "shortName": "EmojiAnimations" if animations else "AnimatedEmojies", "hash": 1},
                "documents": [{"id": "456", "file": "/static/reactions/2764_effect_animation.tgs", "alt": "❤️", "size": 100, "w": 512, "h": 512}],
                "packs": {"❤️": ["456"]}})
        async def stickers(request):
            authorize(request)
            if request.method == "POST":
                payload = await request.json()
                assert payload == {"action": "remove", "url": "https://localhost/favorite.gif", "sticker": {"url": "https://localhost/favorite.gif", "type": "image"}}
                observed["fave"] += 1
                return web.json_response({"success": True})
            return web.json_response({"success": True, "stickers": [{"url": "https://localhost/favorite.gif", "type": "image"},
                {"url": "https://localhost/favorite.tgs", "type": "tgs"}]})
        async def purchase(request):
            authorize(request)
            payload = await request.json()
            observed["purchase"].append(payload)
            if payload["giftId"] == "fail":
                await asyncio.sleep(0.1)
                return web.json_response({"error": "Not enough Stars."}, status=400)
            assert payload == {"giftId": "gift-one", "mode": "gift", "chatId": "group"}
            return web.json_response({"success": True})
        async def claim(request):
            authorize(request)
            assert await request.json() == {"giveawayId": "giveaway"}
            observed["claim"] += 1
            giveaway["status"] = "claimed"
            return web.json_response({"success": True})
        async def sell(request):
            authorize(request)
            assert await request.json() == {"giftId": "gift-one"}
            observed["sell"] += 1
            return web.json_response({"success": True})
        async def callback(request):
            authorize(request)
            assert await request.json() == {"chatId": "group", "messageId": "bot", "callbackData": "ack:session"}
            observed["callback"] += 1
            return web.json_response({"success": True, "message": "Confirmed"})
        async def voice_token(request):
            authorize(request)
            assert await request.json() == {"chatId": "group", "callId": "fixture-call"}
            observed["voice_token"] += 1
            return web.json_response({"success": True, "serverUrl": "wss://voice.noveo.ir", "callId": "fixture-call",
                "roomName": "fixture-room", "participantToken": "room-only-token", "participantIdentity": "test-user"})
        async def websocket(request):
            ws = web.WebSocketResponse()
            await ws.prepare(request)
            await ws.receive_json()
            sockets.append(ws)
            await ws.send_json({"type": "login_success", "user": user, "token": "test-token"})
            async for incoming in ws:
                frame = json.loads(incoming.data)
                kind = frame["type"]
                if kind == "resync_state":
                    await ws.send_json({"type": "user_list_update", "users": [user, other]})
                    await ws.send_json({"type": "chat_history", "chats": chats})
                elif kind in ("voice_start", "voice_join", "voice_leave"):
                    assert frame["chatId"] == "group" and frame["callId"] == "fixture-call"
                    observed["voice"].append(kind)
                    if kind == "voice_leave":
                        await ws.send_json({"type": "voice_chat_update", "activeVoiceChats": {"group": {
                            "callId": "fixture-call", "participants": ["test-user", "other-user"]}}})
                elif kind in ("typing", "emoji_interaction", "emoji_interaction_seen"):
                    observed["typing"].append(kind)
                    assert frame["chatId"] == "group"
                    if kind == "emoji_interaction":
                        assert frame["messageId"] == "emoji" and frame["interaction"] == {"v": 1, "a": [{"i": 1, "t": 0}]}
                    await ws.send_json({**frame, "senderId": "other-user"})
                elif kind == "toggle_reaction":
                    assert frame["chatId"] == "group" and frame["messageId"] == "emoji" and frame["reaction"] == "❤️"
                    observed["reaction"].append(frame["big"])
                    reactions = [] if len(observed["reaction"]) == 3 else [{"emoji": "❤️", "count": 1, "userIds": ["test-user"]}]
                    await ws.send_json({"type": "message_reactions_update", "chatId": "group", "messageId": "emoji", "reactions": reactions})
                    if len(observed["reaction"]) == 3:
                        await ws.send_json({"type": "message_reactions_update", "chatId": "group", "messageId": "emoji",
                            "reactions": [{"emoji": "🔥", "count": 1, "userIds": ["other-user"]}],
                            "interaction": {"userId": "other-user", "reaction": "🔥", "big": True}})
                else:
                    raise AssertionError(frame)
            return ws
        app = web.Application()
        app.router.add_post("/voice/token", voice_token)
        app.router.add_post("/gifts/sell", sell)
        app.router.add_post("/bot/callback", callback)
        app.router.add_get("/ws", websocket)
        app.router.add_get("/user/contacts", contacts)
        app.router.add_get("/user/profile", profile)
        app.router.add_get("/gifts/catalog", catalog)
        app.router.add_get("/emoji/animated", pack)
        app.router.add_get("/emoji/interactions", pack)
        app.router.add_route("*", "/user/stickers", stickers)
        app.router.add_post("/gifts/purchase", purchase)
        app.router.add_post("/gifts/giveaway/claim", claim)
        runner = web.AppRunner(app)
        await runner.setup()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        site = web.TCPSite(runner, "127.0.0.1", 0, ssl_context=context)
        await site.start()
        port = next(sock.getsockname()[1] for sock in site._server.sockets if len(sock.getsockname()) == 2)
        socks = SocksFixture()
        socks_port = await socks.start()
        process = await asyncio.create_subprocess_exec(str(binary), f"https://localhost:{port}", str(cert), str(socks_port), env={**os.environ, "XDG_CONFIG_HOME": str(tmp / "config")})
        result = await process.wait()
        await socks.close()
        await runner.cleanup()
        assert len(socks.requests) > 5 and all(user == "proxy-user" for _, _, user in socks.requests), socks.requests
        assert result == 0, f"Native bridge probe exited {result}"
        assert observed["reaction"] == [False, True, False], observed
        assert observed["typing"] == ["typing", "emoji_interaction", "emoji_interaction_seen"], observed
        assert observed["claim"] == observed["fave"] == 1 and len(observed["purchase"]) == 2, observed
        assert observed["sell"] == observed["callback"] == 1, observed
        assert observed["voice_token"] == 1 and observed["voice"] == ["voice_start", "voice_join", "voice_leave"], observed
        print("PASS voice token authentication, signaling, call history bubbles, authenticated SOCKS5 WebSocket/HTTP routing, Markdown UTF-16 entities, bot keyboards/callbacks, Stars header amount, gift sales, thumbnails, channel gift identity, native gift cards/profile paging, reactions and live effects, typing/tap batches, animation packs, favorites, mute/unmute, avatar gallery, gift HTTP failure/duplicate suppression/claim updates")

if __name__ == "__main__":
    asyncio.run(main())
