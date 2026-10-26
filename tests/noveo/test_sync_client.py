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
                        str(ROOT / "tests/noveo/sync_client_probe.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/session_client.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/session_actions.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/session_sync.cpp"), str(ROOT / "Telegram/SourceFiles/noveo/sync_state.cpp"),
                        str(ROOT / "Telegram/SourceFiles/noveo/auth_client.cpp"), str(ROOT / "Telegram/lib_tl/tl/tl_basic_types.cpp"), str(scheme) + ".cpp",
                        str(ROOT / "Telegram/SourceFiles/data/data_peer_id.cpp"), "-include", str(tmp / "scheme.h"),
                        "-Wl,--gc-sections", "-o", str(binary), *flags], check=True)
        import sqlite3, contextlib
        from fixtures import account_sync_server as sync
        dbpath=tmp/'state.db'
        @contextlib.contextmanager
        def connect():
            db=sqlite3.connect(dbpath)
            try: yield db
            finally: db.close()
        def mutate(kind,key,value):
            with connect() as db:
                row=db.execute('SELECT version FROM account_sync_items WHERE user_id=? AND kind=? AND item_key=?',('self',kind,key)).fetchone()
                state=sync.update(db,'self',dict(kind=kind,key=key,value=value,expectedVersion=row[0] if row else 0));db.commit()
                return state
        with connect() as db: sync.ensure_schema(db);db.commit()
        mutate('dialog','user:other',{'pinnedRank':1})
        mutate('dialog','chat:group',{'archived':True})
        mutate('draft','user:other',{'text':'😀Hi','replyTo':'root','entities':[{'type':'bold','offset':2,'length':2}]})
        mutate('folder','2',{'title':'Work','flags':{'contacts':True},'includePeers':['user:other','chat:group']})
        mutate('folderOrder','all',[2,0])
        sockets=[];observed={'post':0,'get':0,'conflict':0}
        async def authorize(request):
            assert request.headers.get('X-User-ID')=='self'
            assert request.headers.get('X-Auth-Token') in ('first-token','second-token')
            return 'self'
        async def broadcast(user,frame):
            for ws in list(sockets):
                if not ws.closed: await ws.send_json(frame)
        def event(state,kind,key):
            return dict(type='account_sync_update',revision=state['revision'],item=next(i for i in state['items'] if i['kind']==kind and i['key']==key))
        async def contacts(request):
            await authorize(request);return web.json_response({'contacts':[]})
        async def notifications(request):
            await authorize(request);return web.json_response({'success':True,'revision':0,'settings':{}})
        async def websocket(request):
            ws=web.WebSocketResponse();await ws.prepare(request);sockets.append(ws)
            login=await ws.receive_json()
            await ws.send_json({'type':'login_success','user':{'userId':'self','username':'Self'},'token':login['username']+'-token'})
            async for incoming in ws:
                frame=json.loads(incoming.data)
                if frame['type']=='resync_state':
                    await ws.send_json({'type':'user_list_update','users':[{'userId':'self','username':'Self'},{'userId':'other','username':'Other'}]})
                    await ws.send_json({'type':'chat_history','chats':[{'chatId':'dm','chatType':'private','members':['self','other'],'messages':[{'messageId':'root','senderId':'other','timestamp':1700000000,'content':{'text':'reply target'}}]},{'chatId':'group','chatType':'group','chatName':'Group','members':['self','other'],'messages':[]}]})
                    with connect() as db: state=sync.snapshot(db,'self')
                    await ws.send_json(state)
                    await ws.send_json({'type':'fixture_ready'})
                elif frame['type']=='fixture_gap':
                    mutate('folder','3',{'title':'New Android folder','flags':{'groups':True}})
                    state=mutate('folderOrder','all',[3,0,2])
                    await broadcast('self',event(state,'folderOrder','all'))
                elif frame['type']=='ping':await ws.send_json({'type':'pong'})
            sockets.remove(ws)
            return ws
        app=web.Application()
        sync.install(app,connect,authorize,broadcast)
        @web.middleware
        async def conflicting(request,handler):
            if request.path=='/user/sync':
                if request.method=='GET':observed['get']+=1
                else:
                    observed['post']+=1
                    data=await request.json()
                    if data['kind']=='draft' and isinstance(data['value'],dict) and data['value'].get('text')=='stale draft':
                        state=mutate('draft','user:other',{'text':'Android conflict'});observed['conflict']+=1
                        await broadcast('self',event(state,'draft','user:other'))
                    await asyncio.sleep(0.03)
            return await handler(request)
        app.middlewares.append(conflicting)
        app.router.add_get('/ws',websocket)
        app.router.add_get('/user/contacts',contacts)
        app.router.add_get('/user/notifications',notifications)
        runner=web.AppRunner(app)
        await runner.setup()
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert,key)
        site=web.TCPSite(runner,'127.0.0.1',0,ssl_context=context)
        await site.start()
        port=next(sock.getsockname()[1] for sock in site._server.sockets if len(sock.getsockname())==2)
        process=await asyncio.create_subprocess_exec(str(binary),f'https://localhost:{port}',str(cert))
        result=await process.wait()
        await runner.cleanup()
        assert result==0,f'Native sync probe exited {result}'
        assert observed['post']>=10 and observed['get']>=3 and observed['conflict']==1,observed
        print('PASS native sync: TWO live sessions, authenticated persisted API, draft entities/replies, in-flight edits, conflict-before-HTTP, pins/order, archive, unread, folder deletion, explicit clears, revision-gap refetch')

if __name__=='__main__':asyncio.run(main())
