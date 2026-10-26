"""Persistent account state. Versions are per item; cleared items remain tombstones."""
import json
import time
from aiohttp import web

KINDS = {'dialog', 'draft', 'folder', 'folderOrder'}
FLAGS = {'contacts', 'nonContacts', 'groups', 'broadcasts', 'bots', 'excludeMuted', 'excludeRead', 'excludeArchived'}


class Conflict(Exception):
    pass


def ensure_schema(db):
    db.execute('CREATE TABLE IF NOT EXISTS account_sync_items ('
               'user_id TEXT NOT NULL, kind TEXT NOT NULL, item_key TEXT NOT NULL, '
               'value TEXT, version INTEGER NOT NULL, updated_at INTEGER NOT NULL, '
               'PRIMARY KEY(user_id, kind, item_key))')
    db.execute('CREATE TABLE IF NOT EXISTS account_sync_revisions ('
               'user_id TEXT PRIMARY KEY, revision INTEGER NOT NULL)')


def peer_key(key):
    return isinstance(key, str) and len(key) <= 240 and key.startswith(('user:', 'chat:')) and bool(key.split(':', 1)[1])


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def peers(value):
    return isinstance(value, list) and len(value) <= 200 and all(peer_key(k) for k in value) and len(value) == len(set(value))


def normalize(payload):
    if not isinstance(payload, dict) or 'value' not in payload or set(payload) - {'kind', 'key', 'value', 'expectedVersion'}:
        raise ValueError('Invalid state mutation')
    kind, key, value = payload.get('kind'), payload.get('key'), payload.get('value')
    if kind not in KINDS or not isinstance(key, str) or not integer(payload.get('expectedVersion'), 0, 2**53 - 1):
        raise ValueError('Kind, key and expectedVersion are required')
    if kind in ('dialog', 'draft') and not peer_key(key):
        raise ValueError('Invalid peer key')
    if kind == 'folder' and (not key.isascii() or not key.isdigit() or not 2 <= int(key) <= 10000 or str(int(key)) != key):
        raise ValueError('Folder IDs start at 2; 0 and 1 are reserved')
    if kind == 'folderOrder' and key != 'all':
        raise ValueError('Invalid folder order key')
    if value is None:
        return kind, key, None, payload['expectedVersion']
    if kind == 'folderOrder':
        if not isinstance(value, list) or len(value) > 100 or not all(integer(x, 0, 10000) and x != 1 for x in value) or len(set(value)) != len(value):
            raise ValueError('Invalid folder order')
    elif not isinstance(value, dict):
        raise ValueError('Expected state object')
    elif kind == 'dialog':
        if set(value) - {'pinnedRank', 'archived', 'unread'} or not value:
            raise ValueError('Invalid dialog fields')
        if 'pinnedRank' in value and not integer(value['pinnedRank'], 0, 200):
            raise ValueError('Invalid pin rank')
        if any(k in value and type(value[k]) is not bool for k in ('archived', 'unread')):
            raise ValueError('Invalid dialog boolean')
    elif kind == 'draft':
        if set(value) - {'text', 'replyTo', 'entities', 'noWebpage'} or not isinstance(value.get('text'), str) or len(value['text']) > 16384:
            raise ValueError('Invalid draft')
        if 'replyTo' in value and (not isinstance(value['replyTo'], str) or len(value['replyTo']) > 240):
            raise ValueError('Invalid reply')
        if 'noWebpage' in value and type(value['noWebpage']) is not bool:
            raise ValueError('Invalid preview setting')
        entities = value.get('entities', [])
        if not isinstance(entities, list) or len(entities) > 100:
            raise ValueError('Invalid draft entities')
        utf16_length = len(value['text'].encode('utf-16-le')) // 2
        for entity in entities:
            if not isinstance(entity, dict) or set(entity) - {'type', 'offset', 'length', 'url', 'userId', 'language', 'documentId'}:
                raise ValueError('Invalid draft entity')
            if not isinstance(entity.get('type'), str) or len(entity['type']) > 64 or not integer(entity.get('offset'), 0, utf16_length) or not integer(entity.get('length'), 1, utf16_length) or entity['offset'] + entity['length'] > utf16_length:
                raise ValueError('Invalid entity range')
            if any(k in entity and (not isinstance(entity[k], str) or len(entity[k]) > 2048) for k in ('url', 'userId', 'language', 'documentId')):
                raise ValueError('Invalid entity data')
    elif kind == 'folder':
        if set(value) - {'title', 'emoticon', 'color', 'flags', 'pinnedPeers', 'includePeers', 'excludePeers'} or not isinstance(value.get('title'), str) or not 1 <= len(value['title']) <= 128:
            raise ValueError('Invalid folder')
        if 'emoticon' in value and (not isinstance(value['emoticon'], str) or len(value['emoticon']) > 32):
            raise ValueError('Invalid folder icon')
        if 'color' in value and not integer(value['color'], 0, 255):
            raise ValueError('Invalid folder color')
        flags = value.get('flags', {})
        if not isinstance(flags, dict) or set(flags) - FLAGS or any(type(v) is not bool for v in flags.values()):
            raise ValueError('Invalid folder flags')
        if any(k in value and not peers(value[k]) for k in ('pinnedPeers', 'includePeers', 'excludePeers')):
            raise ValueError('Invalid folder peers')
    if len(json.dumps(value, ensure_ascii=False).encode()) > 65536:
        raise ValueError('State item too large')
    return kind, key, value, payload['expectedVersion']


def snapshot(db, user_id):
    row = db.execute('SELECT revision FROM account_sync_revisions WHERE user_id=?', (user_id,)).fetchone()
    items = [{'kind': k, 'key': key, 'value': json.loads(value) if value is not None else None,
              'version': version, 'updatedAt': date} for k, key, value, version, date in db.execute(
                  'SELECT kind,item_key,value,version,updated_at FROM account_sync_items WHERE user_id=? ORDER BY kind,item_key', (user_id,))]
    return {'type': 'account_sync_state', 'revision': row[0] if row else 0, 'items': items}


def update(db, user_id, payload):
    kind, key, value, expected = normalize(payload)
    row = db.execute('SELECT value,version FROM account_sync_items WHERE user_id=? AND kind=? AND item_key=?', (user_id, kind, key)).fetchone()
    version = row[1] if row else 0
    if expected != version:
        raise Conflict()
    # Dialog patches preserve fields changed by other operations after refetch.
    if kind == 'dialog' and value is not None:
        value = {**(json.loads(row[0]) if row and row[0] else {}), **value}
    serialized = json.dumps(value, ensure_ascii=False, sort_keys=True) if value is not None else None
    if row and row[0] == serialized:
        return snapshot(db, user_id)
    count = db.execute('SELECT COUNT(*) FROM account_sync_items WHERE user_id=? AND kind=?', (user_id, kind)).fetchone()[0]
    if not row and count >= (100 if kind == 'folder' else 2000):
        raise ValueError('Account state limit reached')
    size = db.execute('SELECT COALESCE(SUM(LENGTH(value)),0) FROM account_sync_items WHERE user_id=?', (user_id,)).fetchone()[0]
    if size - (len(row[0]) if row and row[0] else 0) + (len(serialized) if serialized else 0) > 1048576:
        raise ValueError('Account state storage limit reached')
    db.execute('INSERT INTO account_sync_revisions VALUES (?,1) ON CONFLICT(user_id) DO UPDATE SET revision=revision+1', (user_id,))
    revision = db.execute('SELECT revision FROM account_sync_revisions WHERE user_id=?', (user_id,)).fetchone()[0]
    db.execute('INSERT INTO account_sync_items VALUES (?,?,?,?,?,?) ON CONFLICT(user_id,kind,item_key) DO UPDATE SET value=excluded.value,version=excluded.version,updated_at=excluded.updated_at',
               (user_id, kind, key, serialized, revision, int(time.time())))
    return snapshot(db, user_id)


def install(app, connect, authenticate, send_to_user, cors=lambda req, res: res):
    async def handler(request):
        if request.method == 'OPTIONS':
            return web.Response(status=204)
        if request.method not in ('GET', 'POST'):
            return web.json_response({'success': False, 'error': 'Method not allowed'}, status=405)
        user_id = await authenticate(request)
        if not user_id:
            return web.json_response({'success': False, 'error': 'Authentication required'}, status=401)
        with connect() as db:
            if request.method == 'GET':
                result = snapshot(db, user_id)
            else:
                try:
                    payload = await request.json()
                    normalize(payload)
                    db.execute('BEGIN IMMEDIATE')
                    result = update(db, user_id, payload)
                    db.commit()
                except Conflict:
                    db.rollback()
                    return web.json_response({'success': False, 'error': 'STATE_VERSION_CONFLICT', **snapshot(db, user_id)}, status=409)
                except (ValueError, TypeError, UnicodeError):
                    db.rollback()
                    return web.json_response({'success': False, 'error': 'Invalid account state'}, status=400)
        if request.method == 'POST':
            kind, key = payload['kind'], payload['key']
            item = next(i for i in result['items'] if i['kind'] == kind and i['key'] == key)
            await send_to_user(user_id, {'type': 'account_sync_update', 'revision': result['revision'], 'item': item})
        return web.json_response({'success': True, **result})
    async def endpoint(request):
        return cors(request, await handler(request))
    app.router.add_route('*', '/user/sync', endpoint)
