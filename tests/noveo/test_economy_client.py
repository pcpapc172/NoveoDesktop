"""Native response/protocol integration against a local TLS server; no live credentials."""
import asyncio
import json
import base64
from pathlib import Path
import ssl
import subprocess
import tempfile
from aiohttp import web

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
                        str(ROOT / "tests/noveo/economy_client_probe.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/session_client.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/session_actions.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/session_premium.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/session_stars.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/session_sync.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/sync_state.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/auth_client.cpp"), str(ROOT / "Telegram/lib_tl/tl/tl_basic_types.cpp"), str(scheme) + ".cpp",
                        str(ROOT / "Telegram/SourceFiles/data/data_peer_id.cpp"), "-include", str(tmp / "scheme.h"),
                        "-Wl,--gc-sections", "-o", str(binary), *flags], check=True)
        png = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII=")
        image = tmp / "receipt.png"
        image.write_bytes(png)
        profile = {"userId": "test-user", "username": "Fixture", "membershipTier": "premium"}
        seen = {"wallet": 0, "assets": 0, "customization": 0, "giveaway": 0, "rejected": 0, "receipts": 0}
        socket = None
        wallet = {"balanceTenths": 12345, "transactions": [
            {"transactionId": "in", "amountTenths": 150, "createdAt": 1700000010, "description": "Received Stars", "relatedUserId": "other"},
            {"transactionId": "out", "amountTenths": -25, "createdAt": 1700000009, "description": "Gift purchase"},
            {"transactionId": "tiny", "amountTenths": 1, "createdAt": 1700000008, "description": "Fractional credit"},
            {"transactionId": "reward", "amountTenths": 1, "createdAt": 1700000007, "type": "message_reward"},
            {"transactionId": "zero", "amountTenths": 0, "createdAt": 1700000006, "description": "Adjustment"},
            {"transactionId": "in", "amountTenths": 150, "createdAt": 1700000010},
            {"transactionId": "old", "amountTenths": 200, "createdAt": 1700000005000, "description": "Earlier credit"}]}
        def authorize(request):
            assert request.headers.get("X-User-ID") == "test-user"
            assert request.headers.get("X-Auth-Token") == "test-token"
        async def overview(request):
            authorize(request)
            seen["wallet"] += 1
            if seen["wallet"] == 1:
                older = dict(wallet)
                wallet["balanceTenths"] = 12346
                await socket.send_json({"type": "wallet_updated", "balanceTenths": 12346})
                await asyncio.sleep(0.05)
                return web.json_response({"success": True, "wallet": older})
            return web.json_response({"success": True, "wallet": wallet})
        async def assets(request):
            authorize(request)
            seen["assets"] += 1
            return web.json_response({"success": True, "stickers": [{"url": "/icon.png", "type": "image"}],
                                      "templates": [{"id": "fixture", "url": "/icon.tgs", "type": "tgs"}]})
        async def giveaway(request):
            authorize(request)
            body = await request.json()
            assert body["chatId"] == "dm"
            if body["amount"] == "999999":
                seen["rejected"] += 1
                return web.json_response({"error": "Not enough Stars."}, status=400)
            assert body["amount"] == "1.25"
            seen["giveaway"] += 1
            wallet["balanceTenths"] = 12220
            await socket.send_json({"type": "wallet_updated", "balanceTenths": 12220})
            return web.json_response({"success": True})
        async def receipt(request):
            authorize(request)
            reader = await request.multipart()
            parts = {}
            async for part in reader:
                assert part.name not in parts
                parts[part.name] = bytes(await part.read())
                if part.name == "receipt": assert part.filename.endswith(".png")
            assert parts == {"tier": [b"silver", b"premium_upgrade"][seen["receipts"]], "receipt": png}, parts.keys()
            seen["receipts"] += 1
            return web.json_response({"success": True, "request": {"status": "open"}})
        async def websocket(request):
            nonlocal socket
            socket = web.WebSocketResponse()
            await socket.prepare(request)
            login = await socket.receive_json()
            assert login["type"] == "login_with_password"
            await socket.send_json({"type": "login_success", "user": profile, "token": "test-token"})
            async for incoming in socket:
                frame = json.loads(incoming.data)
                if frame["type"] == "resync_state":
                    await socket.send_json({"type": "user_list_update", "users": [profile, {"userId": "other", "username": "Other"}]})
                    await socket.send_json({"type": "chat_history", "chats": [{"chatId": "dm", "chatType": "private", "members": ["test-user", "other"], "messages": []}]})
                elif frame["type"] == "update_profile":
                    fields = {k: v for k, v in frame.items() if k != "type"}
                    assert fields and set(fields) <= {"premiumStarIcon", "premiumStarIcons", "profileSkin", "nicknameFont"}
                    for key in ["premiumStarIcon", "premiumStarIcons"]:
                        if fields.get(key): assert fields[key]["url"] in ["/icon.png", "/icon.tgs"]
                    if fields.get("profileSkin"):
                        assert fields["profileSkin"] == {"mode": "gradient", "colors": ["#00897b", "#26c6da"], "angle": 135}
                    profile.update(fields)
                    seen["customization"] += 1
                    await socket.send_json({"type": "user_updated", **profile})
                else:
                    raise AssertionError(frame["type"])
            return socket
        async def empty(request):
            return web.json_response({"success": True, "contacts": [], "gifts": [], "items": [], "revision": 0})
        app = web.Application()
        app.router.add_get("/ws", websocket)
        app.router.add_get("/stars/overview", overview)
        app.router.add_post("/stars/giveaway", giveaway)
        app.router.add_post("/payment/request", receipt)
        app.router.add_get("/user/premium/profile-assets", assets)
        for route in ["/user/contacts", "/gifts/catalog", "/user/notifications", "/user/sync"]:
            app.router.add_get(route, empty)
        runner = web.AppRunner(app)
        await runner.setup()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        site = web.TCPSite(runner, "127.0.0.1", 0, ssl_context=context)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        try:
            process = await asyncio.create_subprocess_exec(str(binary), f"https://localhost:{port}", str(cert), str(image))
            result = await process.wait()
            assert result == 0, result
            assert seen["wallet"] >= 5 and seen["assets"] == 1, seen
            assert {k: seen[k] for k in ["customization", "giveaway", "rejected", "receipts"]} == {
                "customization": 5, "giveaway": 1, "rejected": 1, "receipts": 2}, seen
            print("PASS fractional wallet balance, filtered/deduplicated/paged transaction history, self-account isolation, no topups, Premium/Silver/partial membership, status icons/documents, profile colors/clears, invalid emoji rejection, Stars giveaway/insufficient balance, live balance, and exact local/portal receipt multipart")
        finally:
            await runner.cleanup()

if __name__ == "__main__":
    asyncio.run(main())
