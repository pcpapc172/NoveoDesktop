"""TLS/WebSocket integration checks against a local aiohttp server; no live credentials."""
import asyncio
import json
from pathlib import Path
import ssl
import subprocess
import tempfile
from aiohttp import web, WSMsgType
from socks_fixture import SocksFixture

ROOT = Path(__file__).resolve().parents[2]

async def main():
    with tempfile.TemporaryDirectory(prefix="noveo-auth-") as tmp:
        tmp = Path(tmp)
        cert, key, binary = tmp / "cert.pem", tmp / "key.pem", tmp / "probe"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "Qt6Core", "Qt6Network"], text=True).split()
        subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fPIC",
                        "-I" + str(ROOT / "Telegram/SourceFiles"), str(ROOT / "tests/noveo/auth_client_probe.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/auth_client.cpp"), "-o", str(binary), *flags], check=True)
        seen = []
        async def handler(request):
            if request.headers.get("Origin") != "https://noveo.ir":
                return web.Response(status=403, text="Origin not allowed")
            assert request.headers.get("User-Agent") == "NoveoDesktop/0.1"
            ws = web.WebSocketResponse(autoping=request.match_info["scenario"] not in ("heartbeat", "probe", "probe_error"))
            await ws.prepare(request)
            if request.match_info["scenario"].startswith("probe"):
                ping = await ws.receive()
                assert ping.type == WSMsgType.PING and ping.data == b"noveo"
                if request.match_info["scenario"] == "probe_error":
                    await ws.close()
                else:
                    await ws.pong(ping.data)
                async for _ in ws:
                    pass
                return ws
            auth = await ws.receive_json()
            seen.append(auth)
            assert auth["clientInfo"]["clientName"] == "NoveoDesktop"
            if auth["type"] == "login_with_password":
                assert auth["username"] == "test-user" and auth["password"] == "test-password"
            else:
                assert auth["type"] == "reconnect" and auth["token"] == "test-token"
                assert "password" not in auth
            scenario = request.match_info["scenario"]
            if scenario in ("invalid", "rate", "revoked"):
                await ws.close(code=4008 if scenario == "rate" else 4003)
                return ws
            if scenario == "malformed":
                await ws.send_str("not json")
                return ws
            if scenario.startswith("totp"):
                await ws.send_json({"type": "login_totp_required"})
                verify = await ws.receive_json()
                if scenario == "totp_retry":
                    assert verify == {"type": "login_totp_verify", "code": "000000"}
                    await ws.send_json({"type": "login_totp_error", "message": "Invalid code"})
                    verify = await ws.receive_json()
                assert verify == {"type": "login_totp_verify", "code": "123456"}
            payload = json.dumps({"type": "login_success", "token": "test-token",
                                  "user": {"userId": "test-user", "username": "test-user"}})
            if scenario == "fragment":
                # Raw server frames exercise fragmented text with an interleaved ping.
                transport = request.transport
                def frame(opcode, data):
                    return bytes([opcode, len(data)]) + data
                data = payload.encode()
                transport.write(frame(1, data[:80]) + frame(137, b"ping") + frame(128, data[80:]))
            else:
                await ws.send_str(payload)
            if scenario == "reconnect" and auth["type"] == "login_with_password":
                await ws.close(code=1012)
                return ws
            async for _ in ws:
                pass
            return ws
        app = web.Application()
        app.router.add_get("/{scenario}", handler)
        runner = web.AppRunner(app)
        await runner.setup()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        site = web.TCPSite(runner, "127.0.0.1", 0, ssl_context=context)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        socks = SocksFixture()
        socks_port = await socks.start()
        try:
            for scenario in ("totp", "totp_retry", "success", "invalid", "rate", "malformed", "fragment", "restore", "reconnect", "revoked", "untrusted", "heartbeat", "offline", "proxy", "proxy_switch", "proxy_disable", "probe", "probe_error"):
                proc = await asyncio.create_subprocess_exec(str(binary), f"wss://localhost:{port}/{scenario}", scenario, str(cert), str(socks_port))
                code = await proc.wait()
                assert code == 0, (scenario, code)
                print(f"PASS {scenario}", flush=True)
            assert any(msg["type"] == "reconnect" for msg in seen)
            assert any(user == "proxy-user" for _, _, user in socks.requests), socks.requests
            assert any(user == "" for _, _, user in socks.requests), socks.requests
        finally:
            await socks.close()
            await runner.cleanup()

asyncio.run(main())
