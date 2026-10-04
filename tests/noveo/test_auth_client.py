"""TLS/WebSocket integration checks against a local aiohttp server; no live credentials."""
import asyncio
import json
from pathlib import Path
import ssl
import subprocess
import tempfile
from aiohttp import web

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
            ws = web.WebSocketResponse(autoping=request.match_info["scenario"] != "heartbeat")
            await ws.prepare(request)
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
        try:
            for scenario in ("success", "invalid", "rate", "malformed", "fragment", "restore", "reconnect", "revoked", "untrusted", "heartbeat"):
                proc = await asyncio.create_subprocess_exec(str(binary), f"wss://localhost:{port}/{scenario}", scenario, str(cert))
                code = await proc.wait()
                assert code == 0, (scenario, code)
                print(f"PASS {scenario}", flush=True)
            assert any(msg["type"] == "reconnect" for msg in seen)
        finally:
            await runner.cleanup()

asyncio.run(main())
