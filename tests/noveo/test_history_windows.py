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
        subprocess.run(["g++", "-std=c++20", "-fPIC", "-O0", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-missing-field-initializers",
                        "-Wno-deprecated-declarations", "-ffunction-sections", "-fdata-sections",
                        *("-I" + str(path) for path in includes),
                        str(ROOT / "tests/noveo/history_windows_probe.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/session_client.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/session_actions.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/session_premium.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/session_stars.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/session_sync.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/sync_state.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/auth_client.cpp"), str(ROOT / "Telegram/lib_tl/tl/tl_basic_types.cpp"), str(scheme) + ".cpp",
                        str(ROOT / "Telegram/SourceFiles/data/data_peer_id.cpp"), "-include", str(tmp / "scheme.h"),
                        "-Wl,--gc-sections", "-o", str(binary), *flags], check=True)
        rows = [{"messageId":f"m{i}","serverMessageId":100+i,"senderId":"self","timestamp":1000+i,"content":{"text":str(i)}} for i in range(1,61)]
        user={"userId":"self","username":"Self"}
        group={"chatId":"g","chatType":"group","chatName":"History fixture","ownerId":"self","canManageSettings":True,"members":["self"],"messages":rows[-3:]}
        requests=[]
        async def websocket(request):
            ws=web.WebSocketResponse();await ws.prepare(request);await ws.receive_json()
            await ws.send_json({"type":"login_success","user":user,"token":"fixture"})
            async for incoming in ws:
                frame=json.loads(incoming.data)
                if frame["type"]=="resync_state":
                    await ws.send_json({"type":"user_list_update","users":[user]})
                    await ws.send_json({"type":"chat_history","historyVersion":2,"chats":[group]})
                elif frame["type"]=="load_history_page":
                    requests.append(frame);ordered=list(reversed(rows));offset=frame["offsetId"]
                    index=sum(row["serverMessageId"]>=offset for row in ordered) if offset else 0
                    index=max(0,index+frame["addOffset"])
                    await ws.send_json({"type":"history_page","chatId":"g","requestId":frame["requestId"],"historyVersion":2,"count":len(rows),"messages":ordered[index:index+frame["limit"]]})
                elif frame["type"]=="message":
                    assert frame["replyToId"]=="m1"
                    row={"messageId":"m61","serverMessageId":161,"senderId":"self","timestamp":1061,"content":frame["content"],"replyToId":"m1","replyToServerMessageId":101,"chatId":"g","clientTempId":frame["clientTempId"]}
                    rows.append(row);await ws.send_json({"type":"message_sent","message":row})
                else:raise AssertionError(frame["type"])
            return ws
        async def http(request):
            if request.path=="/user/sync":return web.json_response({"success":True,"type":"account_sync_state","revision":0,"items":[]})
            return web.json_response({"success":True,"gifts":[],"contacts":[],"type":"notification_settings_updated","settings":{},"revision":0})
        app=web.Application();app.router.add_get('/ws',websocket);app.router.add_route('*','/{path:.*}',http)
        runner=web.AppRunner(app);await runner.setup()
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key)
        site=web.TCPSite(runner,"localhost",0,ssl_context=context);await site.start();port=site._server.sockets[0].getsockname()[1]
        try:
            process=await asyncio.create_subprocess_exec(str(binary),f"https://localhost:{port}",str(cert))
            assert await process.wait()==0
            assert len(requests)==6,requests
            print("PASS contiguous native history after old context, old scroll and reply; stable IDs; forward/around paging")
        finally:await runner.cleanup()

if __name__=="__main__":asyncio.run(main())
