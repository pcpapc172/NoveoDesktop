"""Local SOCKS5 tunnel with optional credentials for transport integration tests."""
import asyncio
import contextlib


class SocksFixture:
    def __init__(self):
        self.requests = []
        self.tasks = set()

    async def start(self):
        self.server = await asyncio.start_server(self.handle, "127.0.0.1", 0)
        return self.server.sockets[0].getsockname()[1]

    async def close(self):
        self.server.close()
        await self.server.wait_closed()
        tasks = list(self.tasks)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

    async def handle(self, reader, writer):
        task = asyncio.current_task()
        self.tasks.add(task)
        upstream = None
        pumps = []
        try:
            version, count = await reader.readexactly(2)
            assert version == 5
            methods = await reader.readexactly(count)
            method = 2 if 2 in methods else 0
            writer.write(bytes([5, method]))
            await writer.drain()
            username = b""
            if method == 2:
                version, count = await reader.readexactly(2)
                assert version == 1
                username = await reader.readexactly(count)
                count = (await reader.readexactly(1))[0]
                password = await reader.readexactly(count)
                valid = username == b"proxy-user" and password == b"proxy-password"
                writer.write(bytes([1, 0 if valid else 1]))
                await writer.drain()
                if not valid:
                    return
            version, command, _, address_type = await reader.readexactly(4)
            assert version == 5 and command == 1
            if address_type == 3:
                count = (await reader.readexactly(1))[0]
                host = (await reader.readexactly(count)).decode()
            elif address_type == 1:
                host = ".".join(str(b) for b in await reader.readexactly(4))
            else:
                raise AssertionError(address_type)
            port = int.from_bytes(await reader.readexactly(2), "big")
            self.requests.append((host, port, username.decode()))
            remote, upstream = await asyncio.open_connection(host, port)
            writer.write(b"\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00")
            await writer.drain()

            async def pump(source, target):
                while data := await source.read(65536):
                    target.write(data)
                    await target.drain()
            pumps = [asyncio.create_task(pump(reader, upstream)), asyncio.create_task(pump(remote, writer))]
            await asyncio.wait(pumps, return_when=asyncio.FIRST_COMPLETED)
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            for pump in pumps:
                pump.cancel()
            await asyncio.gather(*pumps, return_exceptions=True)
            for stream in (writer, upstream):
                if stream:
                    stream.close()
                    with contextlib.suppress(ConnectionError):
                        await stream.wait_closed()
            self.tasks.discard(task)
