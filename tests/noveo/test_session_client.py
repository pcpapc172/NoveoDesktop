"""Native response/protocol integration against a local TLS server; no live credentials."""
import asyncio
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
                        str(ROOT / "tests/noveo/session_client_probe.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/session_client.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/auth_client.cpp"), str(ROOT / "Telegram/lib_tl/tl/tl_basic_types.cpp"), str(scheme) + ".cpp",
                        str(ROOT / "Telegram/SourceFiles/data/data_peer_id.cpp"), "-include", str(tmp / "scheme.h"),
                        "-Wl,--gc-sections", "-o", str(binary), *flags], check=True)
        observed = {"contacts": 0, "messages": 0, "older": 0, "seen": 0, "reconnect": 0}
        self_user = {"userId": "test-user", "username": "Self", "handle": "self", "avatarUrl": "/self.png"}
        other_user = {"userId": "other-user", "username": "Other", "handle": "other", "avatarUrl": "/other.png", "bio": "Fixture profile"}
        def message(uuid, date, sender="other-user", text="hello"):
            return {"messageId": uuid, "timestamp": date, "senderId": sender, "content": {"text": text}}
        chats = [{"chatId": "direct-chat", "chatType": "private", "members": ["test-user", "other-user"],
                  "messages": [message("m1", 1700000001), message("m2", 1700000002)]},
                 {"chatId": "group-chat", "chatType": "group", "chatName": "Group", "members": ["test-user", "other-user"],
                  "messages": [message("g1", 1700000003)]},
                 {"chatId": "channel-chat", "chatType": "channel", "chatName": "Channel",
                  "messages": [message("c1", 1700000004)]}]
        async def contacts(request):
            assert request.headers.get("X-User-ID") == "test-user"
            assert request.headers.get("X-Auth-Token") == "test-token"
            observed["contacts"] += 1
            return web.json_response({"contacts": [other_user]})
        async def profile(request):
            assert request.query["userId"] == "other-user"
            return web.json_response({"success": True, "profile": other_user})
        async def websocket(request):
            assert request.headers.get("Origin") == "https://noveo.ir"
            ws = web.WebSocketResponse()
            await ws.prepare(request)
            auth = await ws.receive_json()
            if auth["type"] == "reconnect":
                assert auth["userId"] == "test-user" and auth["token"] == "test-token"
                assert "password" not in auth
                observed["reconnect"] += 1
            else:
                assert auth["type"] == "login_with_password" and auth["username"] == "test-user"
            await ws.send_json({"type": "login_success", "user": self_user, "token": "test-token"})
            await ws.send_json({"type": "error", "message": "Unsupported fixture operation"})
            async for incoming in ws:
                frame = json.loads(incoming.data)
                if frame["type"] == "resync_state":
                    await ws.send_json({"type": "user_list_update", "users": [self_user, other_user], "online": ["other-user"]})
                    await ws.send_json({"type": "chat_history", "chats": chats})
                elif frame["type"] == "message":
                    assert frame["chatId"] == "direct-chat" and frame["content"]["text"] == "**desktop** `test`"
                    observed["messages"] += 1
                    payload = message("sent-1", 1700000010, "test-user", "**desktop** `test`")
                    payload.update(chatId="direct-chat", clientTempId=frame["clientTempId"])
                    await ws.send_json({"type": "message_sent", "message": payload})
                elif frame["type"] == "load_older_messages":
                    assert frame["beforeTimestamp"] == 1700000001 and frame["beforeMessageId"] == "m1"
                    observed["older"] += 1
                    await ws.send_json({"type": "older_messages", "chatId": "direct-chat", "requestId": frame["requestId"],
                                        "messages": [message("old1", 1699999990, text="old1"), message("old2", 1699999990, text="old2")],
                                        "hasMoreHistory": False})
                elif frame["type"] == "message_seen":
                    assert frame["chatId"] == "direct-chat" and frame["messageId"] == "m1"
                    observed["seen"] += 1
                    if auth["type"] != "reconnect":
                        await ws.close(code=1012)
                        return ws
                else:
                    raise AssertionError(frame["type"])
            return ws
        app = web.Application()
        app.router.add_get("/ws", websocket)
        app.router.add_get("/user/contacts", contacts)
        app.router.add_get("/user/profile", profile)
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
        assert observed["contacts"] >= 1 and observed["reconnect"] == 1
        assert observed["messages"] == observed["older"] == observed["seen"] == 1, observed
        print("PASS dialogs, group/channel peers, contacts, avatars, history ordering/paging, text acknowledgement, read receipt, profiles, boxed errors, disconnect/reconnect")

if __name__ == "__main__":
    asyncio.run(main())
