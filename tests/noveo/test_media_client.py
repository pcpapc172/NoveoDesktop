"""Native response/protocol integration against a local TLS server; no live credentials."""
import asyncio
import base64
import json
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
                        str(ROOT / "tests/noveo/media_client_probe.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/session_client.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/auth_client.cpp"), str(ROOT / "Telegram/lib_tl/tl/tl_basic_types.cpp"), str(scheme) + ".cpp",
                        str(ROOT / "Telegram/SourceFiles/data/data_peer_id.cpp"), "-include", str(tmp / "scheme.h"),
                        "-Wl,--gc-sections", "-o", str(binary), *flags], check=True)
        observed = {"sync": 0, "uploads": 0, "forwards": 0, "albums": 0}
        user = {"userId": "test-user", "username": "Self"}
        def message(uuid, text, file=None, **extra):
            content = {"text": text, "file": file, "poll": None}
            return {"messageId": uuid, "timestamp": 1700000000 + len(uuid), "senderId": "test-user", "content": content, **extra}
        def file(kind, mime, **extra):
            return {"url": f"https://localhost/assets/{kind}", "name": kind, "type": mime, "size": 100, "width": 320, "height": 240, **extra}
        messages = [message("text", "text"), message("photo", "photo", file("photo.jpg", "image/jpeg")),
                    message("file", "file", file("file.pdf", "application/pdf", asDocument=True)),
                    message("video", "video", file("video.mp4", "video/mp4", duration=5)),
                    message("gif", "gif", file("animation.gif", "image/gif")),
                    message("sticker", "sticker", file("sticker.png", "image/png", sticker=True)),
                    message("tgs", "tgs", file("sticker.tgs", "application/octet-stream", stickerType="tgs")),
                    message("webm", "webm", file("sticker.webm", "video/webm", sticker=True)),
                    message("voice", "voice", file("voice.ogg", "audio/ogg", voice=True)),
                    message("gift", "[gift](https://web.noveo.ir/gift/1)"),
                    message("reply", "reply", replyToId="text")]
        chats = [{"chatId": "group", "chatType": "group", "chatName": "Group", "members": ["test-user"], "messages": messages}]
        async def contacts(request):
            return web.json_response({"contacts": []})
        async def catalog(request):
            return web.json_response({"gifts": [{"giftId": "gift-one", "giftNumber": 1, "name": "Fixture gift", "imageUrl": "https://localhost/gift.tgs", "priceTenths": 1000}]})
        async def upload(request):
            assert request.headers["X-User-ID"] == "test-user"
            assert request.headers["X-Auth-Token"] == "test-token"
            assert request.headers["X-Upload-ID"] and request.headers["Origin"] == "https://noveo.ir"
            reader = await request.multipart()
            part = await reader.next()
            assert part.name == "file"
            data = await part.read()
            assert await reader.next() is None
            observed["uploads"] += 1
            if part.filename == "tiny.png":
                assert data == base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII=")
                return web.json_response({"file": file("tiny.png", "image/png", size=len(data))})
            assert data == b"headtail"
            if part.filename == "cancel.bin":
                await asyncio.sleep(0.5)
                return web.json_response({"file": file("cancel.bin", "application/octet-stream", size=8)})
            if part.filename == "fail.bin":
                return web.json_response({"error": "fixture failure"}, status=413)
            return web.json_response({"file": file("fixture.bin", "application/octet-stream", size=8)})
        async def websocket(request):
            ws = web.WebSocketResponse()
            await ws.prepare(request)
            await ws.receive_json()
            await ws.send_json({"type": "login_success", "user": user, "token": "test-token"})
            sent = 0
            async for incoming in ws:
                frame = json.loads(incoming.data)
                if frame["type"] == "resync_state":
                    observed["sync"] += 1
                    await ws.send_json({"type": "user_list_update", "users": [user]})
                    await ws.send_json({"type": "chat_history", "chats": chats})
                elif frame["type"] == "message":
                    assert frame["chatId"] == "group"
                    content = frame["content"]
                    if "forwardedInfo" in content:
                        observed["forwards"] += 1
                        assert content["forwardedInfo"]["from"] == "Self"
                    elif content["text"].startswith("album"):
                        observed["albums"] += 1
                    else:
                        assert content["text"] == "uploaded" and content["file"]["size"] == 8
                        assert content["file"]["asDocument"] is True
                        assert frame["replyToId"] == "text"
                    sent += 1
                    payload = message(f"sent-{sent}", content["text"])
                    payload["content"] = content
                    payload.update(chatId="group", clientTempId=frame["clientTempId"])
                    await ws.send_json({"type": "message_sent", "message": payload})
                    await ws.send_json({"type": "message_seen_update", "chatId": "group", "messageId": payload["messageId"], "userId": "other-user"})
                else:
                    raise AssertionError(frame)
            return ws
        app = web.Application()
        app.router.add_get("/ws", websocket)
        app.router.add_get("/user/contacts", contacts)
        app.router.add_get("/gifts/catalog", catalog)
        app.router.add_post("/upload/file", upload)
        runner = web.AppRunner(app)
        await runner.setup()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        site = web.TCPSite(runner, "127.0.0.1", 0, ssl_context=context)
        await site.start()
        port = next(sock.getsockname()[1] for sock in site._server.sockets if len(sock.getsockname()) == 2)
        process = await asyncio.create_subprocess_exec(str(binary), f"https://localhost:{port}", str(cert))
        result = await process.wait()
        await runner.cleanup()
        assert result == 0, f"Native bridge probe exited {result}"
        assert observed == {"sync": 1, "uploads": 4, "forwards": 2, "albums": 2}, observed
        print("PASS text with null media, photos, files, video, GIF, WebP/TGS/WebM stickers, voice, native gifts, replies, forwarding, out-of-order upload parts, HTTP upload failure/cancellation, photo upload dimensions, albums, read receipts, no per-message resync")


if __name__ == "__main__":
    asyncio.run(main())
