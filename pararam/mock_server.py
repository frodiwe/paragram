#!/usr/bin/env python3
"""A small in-memory mock of the pararam.io REST API.

Used to develop and test Pararam Desktop without a real pararam.io
account. It implements the subset of endpoints the client uses and keeps
everything in memory.

Usage:
    python3 pararam/mock_server.py --port 8765
    PARARAM_API_URL=http://127.0.0.1:8765 \
    PARARAM_FILE_URL=http://127.0.0.1:8765 ./Telegram

Accounts:
    alice@example.com / password        (no two-factor authentication)
    bob@example.com   / password, code 123456

Test helpers:
    POST /_mock/incoming {"chat_id": 2, "user_id": 3, "text": "hi"}
        posts a message on behalf of another user.
    POST /_mock/edit {"chat_id": 2, "post_no": 1, "text": "edited"}
    POST /_mock/delete {"chat_id": 2, "post_no": 1}
    GET  /_mock/state
"""

import argparse
import datetime
import json
import re
import secrets
import threading
import uuid
import struct
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import mock_calls
except ImportError:  # aiohttp / aiortc are not installed.
    mock_calls = None
from urllib.parse import parse_qs, unquote, urlparse

LOCK = threading.RLock()


def now_iso(offset_seconds=0):
    moment = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(
        seconds=offset_seconds)
    return moment.strftime('%Y-%m-%dT%H:%M:%S.%fZ')


USERS = {
    1: {'id': 1, 'name': 'Alice Example', 'unique_name': 'alice',
        'email': 'alice@example.com', 'password': 'password', 'totp': None},
    2: {'id': 2, 'name': 'Bob Builder', 'unique_name': 'bob',
        'email': 'bob@example.com', 'password': 'password', 'totp': '123456'},
    3: {'id': 3, 'name': 'Carol Tester', 'unique_name': 'carol',
        'email': 'carol@example.com', 'password': 'password', 'totp': None},
    4: {'id': 4, 'name': 'Deploy Bot', 'unique_name': 'deploybot',
        'email': 'bot@example.com', 'password': None, 'totp': None,
        'is_bot': True},
}

CHATS = {}
POSTS = {}  # chat_id -> {post_no: post}
READ = {}  # (chat_id, user_id) -> last read post_no
FAVORITES = set()  # (chat_id, user_id)
HIDDEN = set()  # (chat_id, user_id)
TAGS = {}  # (user_id, tag) -> set of chat ids
FILES = {}  # guid -> {name, mime_type, data}
SESSIONS = {}  # session cookie -> user_id (or pending dict)
XSRF = set()


def add_chat(chat_id, title, users, pm=False, author=1, read_only=False):
    CHATS[chat_id] = {
        'id': chat_id,
        'type': 'pm' if pm else 'group',
        'title': title,
        'description': '' if pm else 'Mock chat %s' % title,
        'custom_title': None,
        'pm': pm,
        'read_only': read_only,
        'author_id': author,
        'thread_users': list(users),
        'thread_admins': [author],
        'time_created': now_iso(-86400 * 30),
        'time_updated': now_iso(),
        'history_start': 0,
        'pinned': [],
        'tshow': True,
        'mute': None,
    }
    POSTS[chat_id] = {}


def add_post(chat_id, user_id, text, reply_no=None, file=None,
             attachments=None, event=None, minutes_ago=0):
    posts = POSTS[chat_id]
    post_no = len(posts) + 1
    post = {
        'uuid': uuid.uuid4().hex,
        'post_no': post_no,
        'chat_id': chat_id,
        'user_id': user_id,
        'text': text,
        'text_parsed': parse_text(text),
        'reply_no': reply_no,
        'time_created': now_iso(-60 * minutes_ago),
        'time_edited': None,
        'ver': 1,
        'is_deleted': False,
        'meta': {
            'user': {
                'id': user_id,
                'name': USERS[user_id]['name'],
                'unique_name': USERS[user_id]['unique_name'],
                'is_bot': USERS[user_id].get('is_bot', False),
            },
        },
        'event': event,
    }
    if file:
        post['meta']['file'] = file
    if attachments:
        post['meta']['attachments'] = attachments
    posts[post_no] = post
    CHATS[chat_id]['time_updated'] = now_iso()
    READ[(chat_id, user_id)] = post_no
    return post


def parse_text(text):
    result = []
    for match in re.finditer(r'(https?://\S+)|(@\w+)|([^@h]+|.)', text or ''):
        url, mention, rest = match.groups()
        if url:
            result.append({'type': 'link', 'value': url, 'link': url})
        elif mention:
            user = next((u for u in USERS.values()
                         if u['unique_name'] == mention[1:]), None)
            if user:
                result.append({'type': 'mention', 'value': mention,
                               'id': user['id'], 'name': user['name']})
            else:
                result.append({'type': 'text', 'value': mention})
        else:
            result.append({'type': 'text', 'value': rest})
    return result


def make_png(width, height):
    def chunk(kind, data):
        body = kind + data
        return (struct.pack('>I', len(data)) + body
                + struct.pack('>I', zlib.crc32(body) & 0xffffffff))
    rows = b''.join(b'\x00' + bytes([200, 80, 40]) * width
                    for _ in range(height))
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height,
                                         8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows))
            + chunk(b'IEND', b''))


def seed():
    add_chat(10, 'General', [1, 2, 3, 4])
    add_chat(11, 'Releases', [1, 3, 4], read_only=True, author=3)
    add_chat(20, '', [1, 3], pm=True, author=3)
    add_chat(21, '', [1, 2], pm=True, author=2)
    add_post(10, 3, 'Welcome to the **General** chat!', minutes_ago=300)
    add_post(10, 2, 'Hi everyone, see https://pararam.io for docs',
             minutes_ago=200)
    add_post(10, 1, 'Hello @bob, `code` and ~~strike~~', reply_no=2,
             minutes_ago=100)
    deleted = add_post(10, 3, 'this one was deleted', minutes_ago=90)
    deleted['is_deleted'] = True
    deleted['text'] = ''
    for i in range(1, 121):
        add_post(10, [2, 3, 4][i % 3], 'Message number %d' % i,
                 minutes_ago=80 - i * 0.5)
    FILES['guid-report'] = {
        'name': 'report.txt',
        'mime_type': 'text/plain',
        'data': b'Quarterly report\n' * 100,
    }
    add_post(10, 3, '', file={
        'guid': 'guid-report', 'name': 'report.txt', 'size': 1700,
        'mime_type': 'text/plain'}, minutes_ago=10)
    add_post(11, 4, 'Release 1.0 deployed', minutes_ago=60)
    add_post(11, 3, 'Release 1.1 deployed', minutes_ago=30)
    png = make_png(40, 30)
    FILES['guid-screen'] = {'name': 'screen.png', 'mime_type': 'image/png',
                            'data': png}
    add_post(11, 3, 'Screenshot of the release', file={
        'guid': 'guid-screen', 'name': 'screen.png', 'size': len(png),
        'mime_type': 'image/png'}, minutes_ago=20)
    add_post(20, 3, 'Hey Alice, how are you?', minutes_ago=50)
    add_post(20, 1, 'Fine, thanks!', minutes_ago=45)
    add_post(20, 3, 'Great, see you tomorrow', minutes_ago=5)
    add_post(21, 2, 'Ping', minutes_ago=400)
    READ[(10, 1)] = 100
    HIDDEN.add((21, 1))
    TAGS[(1, 'Work')] = {10, 11}
    READ[(20, 1)] = 2
    FAVORITES.add((20, 1))


def chat_for(chat_id, user_id):
    chat = dict(CHATS[chat_id])
    posts = POSTS[chat_id]
    chat['posts_count'] = len(posts)
    chat['last_read_post_no'] = READ.get((chat_id, user_id), 0)
    chat['is_favorite'] = (chat_id, user_id) in FAVORITES
    chat['tshow'] = (chat_id, user_id) not in HIDDEN
    last = posts.get(len(posts))
    chat['last_msg'] = last['text'] if last else ''
    chat['last_msg_author_id'] = last['user_id'] if last else None
    return chat


def visible_chats(user_id):
    return [cid for cid, chat in CHATS.items()
            if user_id in chat['thread_users']]


def public_user(user):
    return {k: v for k, v in user.items()
            if k not in ('password', 'totp', 'email')}


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        print('[mock] %s %s' % (self.command, self.path), flush=True)

    # Helpers.

    def send_json(self, code, value, headers=None):
        body = json.dumps(value).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        for key, header in (headers or {}).items():
            self.send_header(key, header)
        self.end_headers()
        self.wfile.write(body)

    def read_body(self):
        length = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(length) if length else b''

    def json_body(self):
        raw = self.read_body()
        return json.loads(raw) if raw else {}

    def session(self):
        cookie = self.headers.get('Cookie') or ''
        match = re.search(r'session=([\w-]+)', cookie)
        return match.group(1) if match else None

    def user_id(self):
        value = SESSIONS.get(self.session())
        return value if isinstance(value, int) else None

    def check_xsrf(self):
        token = self.headers.get('X-Xsrftoken')
        if token not in XSRF:
            self.send_json(400, {'error': 'xsrf', 'message': 'xsrf'})
            return False
        return True

    def require_user(self):
        user_id = self.user_id()
        if not user_id:
            self.send_json(401, {'error': 'unauthorized'})
        return user_id

    # Dispatch.

    def do_GET(self):
        with LOCK:
            self.route('GET')

    def do_POST(self):
        with LOCK:
            self.route('POST')

    def do_PUT(self):
        with LOCK:
            self.route('PUT')

    def do_DELETE(self):
        with LOCK:
            self.route('DELETE')

    def route(self, method):
        url = urlparse(self.path)
        path = url.path
        query = {k: v[0] for k, v in parse_qs(url.query).items()}

        if path.startswith('/_mock/'):
            return self.mock(method, path)
        if path == '/auth/init':
            return self.auth_init()
        if path == '/auth/login/password' and method == 'POST':
            return self.auth_login()
        if path == '/auth/totp' and method == 'POST':
            return self.auth_totp()
        if path == '/auth/next':
            return self.auth_next()
        if path == '/auth/logout':
            SESSIONS.pop(self.session(), None)
            return self.send_json(200, {'status': 'ok'})
        if path.startswith('/download/') and method == 'GET':
            return self.download(path)
        if path == '/upload' and method == 'POST':
            return self.upload()

        if method != 'GET' and not self.check_xsrf():
            return
        user_id = self.require_user()
        if not user_id:
            return

        if path == '/core/call':
            return self.calls(method)
        if path == '/user/presence' and method == 'POST':
            if not self.check_xsrf():
                return
            self.json_body()
            return self.send_json(200, {})
        if path == '/user/me':
            return self.send_json(200, public_user(USERS[user_id]))
        if path == '/user/list':
            ids = [int(i) for i in query.get('ids', '').split(',') if i]
            return self.send_json(200, {'users': [
                public_user(USERS[i]) for i in ids if i in USERS]})
        if path == '/user/search':
            flt = query.get('flt', '').lower()
            found = []
            for user in USERS.values():
                if flt in user['name'].lower() or flt in user['unique_name']:
                    item = public_user(user)
                    pm = next((cid for cid, chat in CHATS.items()
                               if chat['pm']
                               and set(chat['thread_users']) == {user_id, user['id']}),
                              None)
                    item['pm_thread_id'] = pm
                    found.append(item)
            return self.send_json(200, {'flt': flt, 'users': found})
        if path == '/core/chat/sync' and method == 'GET':
            return self.send_json(200, {'chats': visible_chats(user_id)})
        if path == '/core/chat/sync' and method == 'POST':
            return self.chat_sync(user_id)
        if path == '/core/chat' and method == 'GET':
            ids = [int(i) for i in query.get('ids', '').split(',') if i]
            return self.send_json(200, {'chats': [
                chat_for(i, user_id) for i in ids if i in CHATS]})
        if path == '/core/chat/search':
            flt = query.get('flt', '').lower()
            threads = [chat_for(cid, user_id) for cid, chat in CHATS.items()
                       if not chat['pm'] and flt in chat['title'].lower()]
            return self.send_json(200, {'flt': flt, 'threads': threads})
        match = re.fullmatch(r'/core/chat/pm/(\d+)', path)
        if match and method == 'POST':
            other = int(match.group(1))
            for cid, chat in CHATS.items():
                if chat['pm'] and set(chat['thread_users']) == {user_id, other}:
                    return self.send_json(200, {'chat_id': cid})
            cid = max(CHATS) + 1
            add_chat(cid, '', [user_id, other], pm=True, author=user_id)
            return self.send_json(200, {'chat_id': cid})
        match = re.fullmatch(
            r'/core/chat/(\d+)/(favorite|unfavorite|enter|quit|hide|show)', path)
        if match and method == 'POST':
            cid, action = int(match.group(1)), match.group(2)
            if action == 'favorite':
                FAVORITES.add((cid, user_id))
            elif action == 'unfavorite':
                FAVORITES.discard((cid, user_id))
            elif action == 'enter':
                CHATS[cid]['thread_users'].append(user_id)
            elif action == 'quit':
                CHATS[cid]['thread_users'].remove(user_id)
            elif action == 'hide':
                HIDDEN.add((cid, user_id))
            elif action == 'show':
                HIDDEN.discard((cid, user_id))
            CHATS[cid]['time_updated'] = now_iso()
            return self.send_json(200, {'result': 'OK'})
        if path == '/user/chat/tags':
            if method == 'GET':
                return self.send_json(200, {'chats_tags': [
                    {'tag': tag, 'chat_ids': sorted(chats)}
                    for (owner, tag), chats in TAGS.items()
                    if owner == user_id and chats]})
            name, cid = query.get('name', ''), int(query.get('chat_id', 0))
            chats = TAGS.setdefault((user_id, name), set())
            if method == 'PUT':
                chats.add(cid)
            elif method == 'DELETE':
                chats.discard(cid)
            return self.send_json(200, {'result': 'OK'})
        if path == '/msg/post' and method == 'GET':
            return self.get_posts(query)
        match = re.fullmatch(r'/msg/post/(\d+)', path)
        if match and method == 'POST':
            return self.create_post(user_id, int(match.group(1)))
        match = re.fullmatch(r'/msg/post/(\d+)/(\d+)', path)
        if match and method in ('PUT', 'DELETE'):
            cid, no = int(match.group(1)), int(match.group(2))
            post = POSTS.get(cid, {}).get(no)
            if not post:
                return self.send_json(404, {'error': 'not_found'})
            if post['user_id'] != user_id:
                return self.send_json(403, {'error': 'forbidden'})
            post['ver'] += 1
            post['time_edited'] = now_iso()
            CHATS[cid]['time_updated'] = now_iso()
            if method == 'PUT':
                body = self.json_body()
                post['text'] = body.get('text', '')
                post['text_parsed'] = parse_text(post['text'])
            else:
                post['is_deleted'] = True
                post['text'] = ''
            return self.send_json(200, {
                'uuid': post['uuid'], 'chat_id': cid, 'post_no': no,
                'ver': post['ver']})
        match = re.fullmatch(r'/msg/lastread/(\d+)', path)
        if match and method == 'POST':
            cid = int(match.group(1))
            body = self.json_body()
            no = len(POSTS[cid]) if body.get('read_all') else body['post_no']
            READ[(cid, user_id)] = max(READ.get((cid, user_id), 0), no)
            return self.send_json(200, {
                'last_read_post_no': READ[(cid, user_id)],
                'posts_count': len(POSTS[cid])})
        if path == '/posts/search':
            q = query.get('q', '').lower()
            chat_ids = [int(i) for i in query.get('chat_ids', '').split(',') if i]
            found = []
            for cid in visible_chats(user_id):
                if chat_ids and cid not in chat_ids:
                    continue
                for post in POSTS[cid].values():
                    if not post['is_deleted'] and q in post['text'].lower():
                        item = dict(post)
                        item['thread_id'] = item.pop('chat_id')
                        found.append(item)
            found.sort(key=lambda p: p['time_created'], reverse=True)
            return self.send_json(200, {'posts': found[:50],
                                        'count': len(found)})
        return self.send_json(404, {'error': 'not_found', 'path': path})

    # Auth.

    def auth_init(self):
        token = secrets.token_hex(8)
        XSRF.add(token)
        headers = {'X-Xsrftoken': token}
        if not self.session():
            sid = secrets.token_hex(12)
            SESSIONS[sid] = None
            headers['Set-Cookie'] = 'session=%s; Path=/; HttpOnly' % sid
        self.send_json(200, {'user_id': self.user_id()}, headers)

    def auth_login(self):
        if not self.check_xsrf():
            return
        body = self.json_body()
        user = next((u for u in USERS.values()
                     if u['email'] == body.get('email')), None)
        if not user or user['password'] != body.get('password'):
            return self.send_json(400, {
                'error': 'Wrong email or password',
                'codes': {'non_field': 'bad_credentials'}})
        sid = self.session() or secrets.token_hex(12)
        headers = {'Set-Cookie': 'session=%s; Path=/; HttpOnly' % sid}
        if user['totp']:
            SESSIONS[sid] = {'pending': user['id']}
            return self.send_json(200, {'status': 'two_step_required'},
                                  headers)
        SESSIONS[sid] = {'ready': user['id']}
        self.send_json(200, {'status': 'ok'}, headers)

    def auth_totp(self):
        if not self.check_xsrf():
            return
        value = SESSIONS.get(self.session())
        if not isinstance(value, dict) or 'pending' not in value:
            return self.send_json(400, {'error': 'no_pending_login'})
        user = USERS[value['pending']]
        if self.json_body().get('code') != user['totp']:
            return self.send_json(400, {'error': 'wrong_code'})
        SESSIONS[self.session()] = {'ready': user['id']}
        self.send_json(200, {'status': 'OK'})

    def auth_next(self):
        value = SESSIONS.get(self.session())
        if isinstance(value, dict) and 'ready' in value:
            SESSIONS[self.session()] = value['ready']
            return self.send_json(200, {'status': 'ok'})
        if isinstance(value, int):
            return self.send_json(200, {'status': 'ok'})
        return self.send_json(403, {'error': 'two_step_required'})

    # Chats and posts.

    def chat_sync(self, user_id):
        body = self.json_body()
        known = {}
        for item in (body.get('ids') or '').split('/'):
            if item:
                cid, count, read = (int(x) for x in item.split('-'))
                known[cid] = (count, read)
        mine = visible_chats(user_id)
        status = {}
        for cid in mine:
            chat = chat_for(cid, user_id)
            state = (chat['posts_count'], chat['last_read_post_no'])
            if cid in known and known[cid] != state:
                status[str(cid)] = {
                    'posts_count': state[0],
                    'last_read_post_no': state[1],
                    'last_msg': chat['last_msg'],
                    'time_updated': chat['time_updated'],
                }
        since = body.get('sync_time')
        updated = [cid for cid in mine if cid in known and since
                   and CHATS[cid]['time_updated'] > since
                   and str(cid) not in status]
        self.send_json(200, {
            'new': [cid for cid in mine if cid not in known],
            'updated': updated,
            'removed': [cid for cid in known if cid not in mine],
            'status': status,
        })

    def get_posts(self, query):
        if 'ids' in query:
            return self.send_json(200, {'posts': []})
        cid = int(query['chat_id'])
        posts = POSTS.get(cid, {})
        count = len(posts)
        start, end = (int(x) for x in query.get('range', '-50x-1').split('x'))
        if start < 0:
            start = count + start + 1
        if end < 0:
            end = count + end + 1
        start = max(start, 1)
        result = [posts[no] for no in range(start, end + 1) if no in posts]
        self.send_json(200, {'posts': result, 'count': count})

    def create_post(self, user_id, cid):
        body = self.json_body()
        attachments = body.get('attachments') or []
        file = None
        for guid in attachments:
            if guid in FILES:
                stored = FILES[guid]
                file = {'guid': guid, 'name': stored['name'],
                        'size': len(stored['data']),
                        'mime_type': stored['mime_type']}
        post = add_post(cid, user_id, body.get('text', ''),
                        reply_no=body.get('reply_no'), file=file,
                        attachments=attachments)
        post['uuid'] = body.get('uuid') or post['uuid']
        self.send_json(200, {'uuid': post['uuid'], 'chat_id': cid,
                             'post_no': post['post_no']})

    # Files.

    def upload(self):
        if not self.check_xsrf():
            return
        if not self.require_user():
            return
        content_type = self.headers.get('Content-Type', '')
        boundary = content_type.split('boundary=')[-1].encode()
        raw = self.read_body()
        name, mime, data = 'file', 'application/octet-stream', b''
        for part in raw.split(b'--' + boundary):
            if b'\r\n\r\n' not in part:
                continue
            head, content = part.split(b'\r\n\r\n', 1)
            content = content.rsplit(b'\r\n', 1)[0]
            if b'filename="' in head:
                name = re.search(rb'filename="([^"]*)"', head).group(1).decode()
                found = re.search(rb'Content-Type: ([^\r\n]+)', head)
                if found:
                    mime = found.group(1).decode()
                data = content
        guid = uuid.uuid4().hex
        FILES[guid] = {'name': name, 'mime_type': mime, 'data': data}
        self.send_json(200, {'guid': guid, 'name': name, 'size': len(data),
                             'mime_type': mime, 'time_created': now_iso()})

    def download(self, path):
        parts = path.split('/')
        guid = parts[2] if len(parts) > 2 else ''
        stored = FILES.get(unquote(guid))
        if not stored:
            return self.send_json(404, {'error': 'not_found'})
        self.send_response(200)
        self.send_header('Content-Type', stored['mime_type'])
        self.send_header('Content-Length', str(len(stored['data'])))
        self.end_headers()
        self.wfile.write(stored['data'])

    # Calls.

    def calls(self, method):
        user_id = self.require_user()
        if not user_id:
            return
        if mock_calls is None:
            return self.send_json(404, {'error': 'calls_not_available'})
        if method == 'GET':
            return self.send_json(200, mock_calls.list_calls(
                user_id, set(visible_chats(user_id))))
        if not self.check_xsrf():
            return
        cid = int(self.json_body().get('chat_id', 0))
        chat = CHATS.get(cid)
        if not chat or user_id not in chat['thread_users']:
            return self.send_json(404, {'error': 'not_found'})
        return self.send_json(200, mock_calls.access(
            cid, user_id, chat['pm'], chat['thread_users']))

    def mock_call(self, path, body):
        cid = int(body.get('chat_id', 0))
        uid = int(body.get('user_id', 0))
        if path == '/_mock/call_state':
            return self.send_json(200, mock_calls.state())
        if path == '/_mock/call_config':
            return self.send_json(200, mock_calls.configure(body))
        chat = CHATS.get(cid)
        if not chat:
            return self.send_json(404, {'error': 'not_found'})
        if path == '/_mock/call_incoming':
            result = mock_calls.incoming(cid, uid, chat['pm'],
                                         chat['thread_users'],
                                         bool(body.get('video')))
        elif path == '/_mock/call_join':
            result = mock_calls.join(cid, uid, bool(body.get('video', True)))
        elif path == '/_mock/call_leave':
            result = mock_calls.leave(cid, uid)
        else:
            result = None
        if result is None:
            return self.send_json(404, {'error': 'no_call'})
        return self.send_json(200, result)

    # Test helpers.

    def mock(self, method, path):
        if path.startswith('/_mock/call_'):
            if mock_calls is None:
                return self.send_json(404, {'error': 'calls_not_available'})
            return self.mock_call(path, self.json_body())
        if path == '/_mock/state':
            return self.send_json(200, {
                str(cid): {'posts': len(posts)}
                for cid, posts in POSTS.items()})
        body = self.json_body()
        cid = int(body.get('chat_id', 0))
        if path == '/_mock/incoming':
            post = add_post(cid, int(body.get('user_id', 3)),
                            body.get('text', 'Incoming message'),
                            reply_no=body.get('reply_no'))
            return self.send_json(200, {'post_no': post['post_no']})
        post = POSTS.get(cid, {}).get(int(body.get('post_no', 0)))
        if not post:
            return self.send_json(404, {'error': 'not_found'})
        post['ver'] += 1
        post['time_edited'] = now_iso()
        CHATS[cid]['time_updated'] = now_iso()
        if path == '/_mock/edit':
            post['text'] = body.get('text', '')
            post['text_parsed'] = parse_text(post['text'])
        elif path == '/_mock/delete':
            post['is_deleted'] = True
            post['text'] = ''
        return self.send_json(200, {'ver': post['ver']})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--calls-port', type=int, default=8766)
    args = parser.parse_args()
    seed()
    if mock_calls is not None:
        mock_calls.start(args.calls_port)
    else:
        print('aiohttp / aiortc not installed, calls are not available',
              flush=True)
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    print('Mock pararam.io listening on http://127.0.0.1:%d' % args.port,
          flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
