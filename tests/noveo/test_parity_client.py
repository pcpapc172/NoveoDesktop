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
                        str(ROOT / "tests/noveo/parity_client_probe.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/session_client.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/session_actions.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/auth_client.cpp"), str(ROOT / "Telegram/lib_tl/tl/tl_basic_types.cpp"), str(scheme) + ".cpp",
                        str(ROOT / "Telegram/SourceFiles/data/data_peer_id.cpp"), "-include", str(tmp / "scheme.h"),
                        "-Wl,--gc-sections", "-o", str(binary), *flags], check=True)
        user = {"userId": "test-user", "username": "Self"}
        other = {"userId": "other-user", "username": "Other"}
        third = {"userId": "third-user", "username": "Third"}
        saved_contacts = [other, third]
        def message(uuid, text, date=1700000010):
            return {"messageId": uuid, "senderId": "test-user", "timestamp": date, "content": {"text": text}}
        poll = {"question": "Choose", "options": [{"id": "o1", "text": "First"}, {"id": "YQ", "text": "Second"}], "anonymous": False, "viewerChoiceIds": [], "totalVotes": 0, "canSeeResults": True, "canViewVotes": True}
        editable = message("editable", "original")
        poll_message = message("poll", "", 1700000011); poll_message["content"] = {"poll": poll}
        child = {**message("child", "reply", 1700000012), "replyToId": "parent"}
        group = {"chatId": "group", "chatType": "group", "chatName": "Owned", "ownerId": "test-user", "members": ["test-user", "other-user"], "canViewMembers": True, "bio": "Original description", "messages": [editable, poll_message, child]}
        chats = [group,
            {"chatId": "admin-group", "chatType": "group", "chatName": "Admin", "ownerId": "other-user", "adminIds": ["test-user"], "messages": []},
            {"chatId": "member-group", "chatType": "group", "chatName": "Member", "ownerId": "other-user", "permissions": {"canSendMessages": False}, "messages": []},
            {"chatId": "channel", "chatType": "channel", "chatName": "Channel", "ownerId": "test-user", "members": ["test-user", "other-user"], "canViewMembers": True, "messages": []}]
        expected_token = "test-token"
        observed = {"edit": 0, "vote": 0, "context": 0, "delete": 0, "privacy": 0, "revoke": 0, "rotated_http": 0, "search": 0, "member_save": 0, "contact_add": 0, "contact_remove": 0, "create_group": 0, "create_channel": 0, "avatar": 0, "join": 0, "leave": 0, "chat_profile": 0}
        def authorize(request):
            assert request.headers.get("X-User-ID") == "test-user"
            assert request.headers.get("X-Auth-Token") == expected_token
        async def contacts(request):
            authorize(request)
            if request.method == "POST":
                body = await request.json()
                if body["action"] == "add": assert body["saveAs"] == "Friend"; observed["contact_add"] += 1
                else:
                    assert body["action"] == "remove"; observed["contact_remove"] += 1
                    saved_contacts[:] = [value for value in saved_contacts if value["userId"] != body["userId"]]
                return web.json_response({"success": True})
            return web.json_response({"contacts": saved_contacts})
        async def sessions(request):
            authorize(request)
            if expected_token == "rotated-token": observed["rotated_http"] += 1
            return web.json_response({"success": True, "sessions": [
                {"sessionId": "current", "isCurrent": True, "deviceModel": "Desktop", "osName": "Linux", "clientName": "NoveoDesktop", "issuedAt": 1700000000, "lastSeenAt": 1700000010},
                {"sessionId": "remote", "isCurrent": False, "deviceModel": "Phone", "osName": "Android", "clientName": "Noveo", "issuedAt": 1700000001, "lastSeenAt": 1700000020}]})
        async def revoke(request):
            authorize(request); assert await request.json() == {"sessionId": "remote"}; observed["revoke"] += 1
            return web.json_response({"success": True})
        async def privacy(request):
            authorize(request); assert await request.json() == {"blockGroupInvites": True}; observed["privacy"] += 1
            return web.json_response({"success": True})
        async def settings(request):
            authorize(request); body = await request.json()
            if body["action"] == "leave_chat":
                assert body["chatId"] == "public-group"; observed["leave"] += 1
                return web.json_response({"success": True})
            if body["action"] == "update_profile":
                profile = next(chat for chat in chats if chat["chatId"] == body["chatId"])
                expected = ("New channel", "Channel description") if body["chatId"] == "created-channel" else (("Renamed", "Original description") if observed["chat_profile"] == 1 else ("Renamed", "Changed description"))
                assert (body["chatName"], body["bio"]) == expected, body
                profile.update(chatName=body["chatName"], bio=body["bio"]); observed["chat_profile"] += 1
                return web.json_response({"success": True})
            assert body["action"] == "get_profile", body
            return web.json_response({"success": True, "profile": next(chat for chat in chats if chat["chatId"] == body["chatId"])})
        async def member_permissions(request):
            authorize(request)
            if request.method == "POST":
                assert await request.json() == {"chatId": "group", "memberId": "other-user", "canSendMessages": False, "canSendFiles": False, "canAddMembers": None}; observed["member_save"] += 1
            return web.json_response({"success": True, "role": "member", "effectivePermissions": {"canSendMessages": True, "canSendFiles": True, "canAddMembers": True}})
        async def create_group(request):
            authorize(request); assert await request.json() == {"name": "New group", "members": ["other-user"]}; observed["create_group"] += 1
            created = {**group, "chatId": "created-group", "chatName": "New group", "messages": []}; chats.append(created)
            return web.json_response({"success": True, "group": created})
        async def create_channel(request):
            authorize(request); reader = await request.multipart(); body = {}
            async for part in reader: body[part.name] = (await part.read()).decode()
            assert body["name"] == "New channel" and body["handle"].startswith("@channel_"); observed["create_channel"] += 1
            created = {"chatId": "created-channel", "chatType": "channel", "ownerId": "test-user", "chatName": "New channel", "members": ["test-user"], "canViewMembers": True, "messages": []}; chats.append(created)
            return web.json_response({"success": True, "channel": created})
        async def avatar(request):
            authorize(request); reader = await request.multipart(); part = await reader.next()
            assert part.name == "file" and part.filename == "avatar.png" and (await part.read()).startswith(b"\x89PNG"); observed["avatar"] += 1
            return web.json_response({"success": True, "url": f"https://localhost:{port}/avatar.png"})
        async def public_search(request):
            authorize(request); assert request.query["q"] == "publicgroup"
            public = {"chatId": "public-group", "chatType": "group", "ownerId": "other-user", "chatName": "Public group", "isMember": False, "members": ["other-user"], "resultType": "chat"}; chats.append(public)
            return web.json_response({"success": True, "results": [public]})
        async def websocket(request):
            nonlocal expected_token
            ws = web.WebSocketResponse(); await ws.prepare(request)
            login = await ws.receive_json(); assert login["type"] == "login_with_password"
            await ws.send_json({"type": "login_success", "user": user, "token": "test-token"})
            async for incoming in ws:
                frame = json.loads(incoming.data); kind = frame["type"]
                if kind == "resync_state":
                    await ws.send_json({"type": "user_list_update", "users": [user, other, third]})
                    await ws.send_json({"type": "chat_history", "chats": chats})
                elif kind == "load_message_context":
                    assert frame["messageId"] == "parent" and frame["chatId"] == "group"; observed["context"] += 1
                    await ws.send_json({"type": "message_context", **{key: frame[key] for key in ("requestId", "chatId", "messageId")}, "messages": [message("parent", "unloaded parent", 1699999990)]})
                elif kind == "edit_message":
                    assert frame["messageId"] == "editable" and frame["chatId"] == "group"; observed["edit"] += 1
                    if frame["newContent"] == "denied":
                        await ws.send_json({"type": "error", "message": "No permission"}); continue
                    assert frame["newContent"] == "edited"
                    editable["content"]["text"] = "edited"; editable["editedAt"] = 1700000030
                    await asyncio.sleep(0.05)
                    await ws.send_json({"type": "message_updated", "chatId": "group", "messageId": "editable", "newContent": json.dumps(editable["content"]), "editedAt": editable["editedAt"]})
                elif kind == "vote_poll":
                    assert frame["optionIds"] == ["o1"], frame; observed["vote"] += 1
                    poll["viewerChoiceIds"] = ["o1"]; poll["totalVotes"] = 1; poll["options"][0]["voteCount"] = 1
                    await ws.send_json({"type": "message_updated", "chatId": "group", "messageId": "poll", "newContent": json.dumps({"poll": poll}), "editedAt": 1700000031})
                elif kind == "pin_message":
                    assert frame["messageId"] == "editable"
                    await ws.send_json({"type": "message_pinned", "chatId": "group", "message": editable})
                elif kind == "change_password":
                    assert frame["oldPassword"] == "current" and frame["newPassword"] == "new-password"
                    expected_token = "rotated-token"
                    await ws.send_json({"type": "password_changed", "success": True, "token": expected_token, "sessionId": "rotated"})
                elif kind == "search_chat_messages":
                    assert frame["query"] == "needle"; observed["search"] += 1
                    await ws.send_json({"type": "chat_message_search_results", "chatId": frame["chatId"], "requestId": frame["requestId"], "messages": [message("search-result", "needle older", 1699999980)] if frame["chatId"] == "group" else []})
                elif kind == "update_profile":
                    assert frame["username"] == "New name" and frame["bio"] == "New bio"
                    await ws.send_json({"type": "user_updated", "userId": "other-user", "username": "Unrelated"})
                    user.update(username=frame["username"], bio=frame["bio"])
                    await ws.send_json({"type": "user_updated", **user})
                elif kind == "join_channel":
                    assert frame["chatId"] == "public-group"; observed["join"] += 1
                    public = next(chat for chat in chats if chat["chatId"] == "public-group"); public["isMember"] = True; public["members"].append("test-user")
                    await ws.send_json({"type": "chat_joined", "chat": public})
                elif kind == "delete_message":
                    assert frame["messageId"] == "editable" and frame["scope"] == "everyone"; observed["delete"] += 1
                    group["messages"].remove(editable)
                    await ws.send_json({"type": "message_deleted", "chatId": "group", "messageId": "editable"})
                else: raise AssertionError(frame)
            return ws
        app = web.Application()
        app.router.add_get("/ws", websocket)
        app.router.add_get("/user/contacts", contacts)
        app.router.add_post("/user/contacts", contacts)
        app.router.add_get("/chat/member_permissions", member_permissions)
        app.router.add_post("/chat/member_permissions", member_permissions)
        app.router.add_post("/create_group", create_group)
        app.router.add_post("/create_channel", create_channel)
        app.router.add_post("/upload/avatar", avatar)
        app.router.add_get("/user/public-search", public_search)
        app.router.add_get("/user/sessions", sessions)
        app.router.add_post("/user/sessions/revoke", revoke)
        app.router.add_post("/user/privacy", privacy)
        app.router.add_post("/chat/settings", settings)
        app.router.add_get("/gifts/catalog", lambda _: web.json_response({"gifts": []}))
        runner = web.AppRunner(app); await runner.setup()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); context.load_cert_chain(cert, key)
        site = web.TCPSite(runner, "localhost", 0, ssl_context=context); await site.start()
        port = site._server.sockets[0].getsockname()[1]
        try:
            proc = await asyncio.create_subprocess_exec(str(binary), f"https://localhost:{port}", str(cert))
            code = await proc.wait(); assert code == 0, code
            assert observed == {"edit": 2, "vote": 1, "context": 1, "delete": 1, "privacy": 1, "revoke": 1, "rotated_http": 1, "search": 4, "member_save": 1, "contact_add": 1, "contact_remove": 2, "create_group": 1, "create_channel": 1, "avatar": 1, "join": 1, "leave": 1, "chat_profile": 3}, observed
            print("PASS permissions, unloaded replies, acknowledged edits/deletes, denied edit rollback, exact poll option IDs, pinning, members, sessions, privacy, password token rotation, safe group history rejection, chat/global search, member overrides, contact batches, profile edits, group/channel creation, multipart avatar upload, and group join/leave")
        finally: await runner.cleanup()

asyncio.run(main())
