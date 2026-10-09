#!/usr/bin/env python3
# This file is part of Pararam Desktop,
# an alternative pararam.io client based on Telegram Desktop.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
"""Mock of the pararam.io call servers, used by mock_server.py.

Needs aiohttp and aiortc (pip install aiohttp aiortc). Serves, on its own
port:

* the main socket (/sock/websocket), which only says "calls changed";
* a media server: the room socket (/{room_id}/{user_id}?access_key=...)
  and its HTTP API (/session/*, /reject, /leave).

The media server answers SDP offers with aiortc. The "choirmaster"
connection sends a 440 Hz tone, the connection to a simulated participant
sends a moving test picture. aiortc has no VP9, so start the client with
PARARAM_CALL_ANY_CODEC=1 to get video, plus PARARAM_CALL_ALLOW_DIRECT=1 and
PARARAM_MEDIA_INSECURE=1 (the mock has no TURN server and no TLS).

Simulated participants are controlled by the /_mock/call_* helpers of
mock_server.py.
"""

import asyncio
import fractions
import json
import secrets
import threading
import time

import av
import numpy
from aiohttp import WSMsgType, web
from aiortc import (MediaStreamTrack, RTCPeerConnection,
                    RTCSessionDescription, VideoStreamTrack)

CHOIRMASTER = 14548
LOCK = threading.RLock()
LOOP = None
PORT = 8766
ROOMS = {}  # chat_id -> Room
MAIN_SOCKETS = set()
CONFIG = {'auto_answer': True, 'answer_video': True, 'answer_delay': 2.0}
NAMES = {}


class ToneTrack(MediaStreamTrack):
    kind = 'audio'

    def __init__(self):
        super().__init__()
        self._samples = 0
        self._start = None

    async def recv(self):
        rate, count = 48000, 960
        if self._start is None:
            self._start = time.time()
        wait = self._start + self._samples / rate - time.time()
        if wait > 0:
            await asyncio.sleep(wait)
        t = (numpy.arange(count) + self._samples) / rate
        data = (numpy.sin(2 * numpy.pi * 440 * t) * 6000).astype(numpy.int16)
        frame = av.AudioFrame.from_ndarray(
            data.reshape(1, -1), format='s16', layout='mono')
        frame.sample_rate = rate
        frame.pts = self._samples
        frame.time_base = fractions.Fraction(1, rate)
        self._samples += count
        return frame


class PictureTrack(VideoStreamTrack):
    def __init__(self, user_id):
        super().__init__()
        self._user_id = user_id
        self._frame = 0

    async def recv(self):
        pts, time_base = await self.next_timestamp()
        image = numpy.zeros((360, 640, 3), numpy.uint8)
        image[:, :] = (40 + (self._user_id * 70) % 160, 90, 160)
        x = (self._frame * 6) % 600
        image[140:220, x:x + 40] = (255, 255, 255)
        self._frame += 1
        frame = av.VideoFrame.from_ndarray(image, format='bgr24')
        frame.pts = pts
        frame.time_base = time_base
        return frame


class Member:
    def __init__(self, user_id, ws, auth):
        self.user_id = user_id
        self.ws = ws
        self.auth = auth
        self.audio = True
        self.video = False
        self.video_source = 'empty'
        self.pcs = {}
        self.received = {'audio': 0, 'video': 0}
        self.offers = 0
        self.qualities = {}


class Room:
    def __init__(self, chat_id, starter_id, silent, pm, users):
        self.chat_id = chat_id
        self.pm_users = list(users) if pm else []
        self.room_id = 'room-%d-%s' % (chat_id, secrets.token_hex(3))
        self.sess_id = secrets.token_hex(6)
        self.starter_id = starter_id
        self.silent = silent
        self.pm = pm
        self.keys = {}  # access_key -> user_id
        self.members = {}  # user_id -> Member
        self.virtual = {}  # user_id -> {'audio', 'video'}
        self.rejected = set()

    def user_ids(self):
        return list(self.members) + list(self.virtual)

    def user_json(self, user_id):
        if user_id == CHOIRMASTER:
            return {'id': CHOIRMASTER, 'type': 'USER', 'audio': True,
                    'video': False, 'video_source_type': 'empty'}
        if user_id in self.members:
            m = self.members[user_id]
            return {'id': user_id, 'type': 'USER', 'audio': m.audio,
                    'video': m.video, 'video_source_type': m.video_source,
                    'app_type': 'browser'}
        v = self.virtual[user_id]
        return {'id': user_id, 'type': 'USER', 'audio': v['audio'],
                'video': v['video'],
                'video_source_type': 'video' if v['video'] else 'empty',
                'app_type': 'browser'}


def log(*args):
    print('[calls]', *args, flush=True)


def schedule(coroutine):
    """Runs a coroutine on the calls loop from any thread."""
    if LOOP is None:
        return
    asyncio.run_coroutine_threadsafe(coroutine, LOOP)


async def send(ws, value, reliable=False):
    if reliable:
        value = dict(value, request_id='%s_%d' % (
            secrets.token_hex(4), int(time.time() * 1000)))
    try:
        await ws.send_str(json.dumps(value))
    except Exception:  # noqa: BLE001 - the socket may be closing.
        pass


async def broadcast(room, value, exclude=None):
    for user_id, member in list(room.members.items()):
        if user_id != exclude:
            await send(member.ws, value, reliable=True)


def message(kind, data=None, src=None):
    result = {'type': 'message', 'message': {'type': kind}}
    if data is not None:
        result['message']['data'] = data
    if src is not None:
        result['message']['src_user_id'] = src
    return result


async def notify_main_sockets():
    for ws in list(MAIN_SOCKETS):
        try:
            await ws.send_str(json.dumps({'msg': 'call', 'data': {},
                                          'ver': 1}))
        except Exception:  # noqa: BLE001
            MAIN_SOCKETS.discard(ws)


# API used by mock_server.py (called from its threads).

def list_calls(user_id, visible_chat_ids):
    with LOCK:
        result = {}
        for chat_id, room in ROOMS.items():
            if chat_id not in visible_chat_ids or not room.user_ids():
                continue
            result[str(chat_id)] = {
                'users': [{'id': uid} for uid in room.user_ids()],
                'starter_id': room.starter_id,
                'silent': room.silent,
                'sess_id': room.sess_id,
            }
        return result


def access(chat_id, user_id, pm, users):
    with LOCK:
        room = ROOMS.get(chat_id)
        if room is None:
            room = Room(chat_id, user_id, silent=not pm, pm=pm, users=users)
            ROOMS[chat_id] = room
        key = secrets.token_hex(8)
        room.keys[key] = user_id
        return {
            'room_id': room.room_id,
            'media_server': '127.0.0.1:%d' % PORT,
            'access_key': key,
            'turnhost': '127.0.0.1',
            'turnuser': 'user',
            'turnpassword': 'password',
        }


def incoming(chat_id, user_id, pm, users, video=False):
    with LOCK:
        room = ROOMS.get(chat_id)
        if room is None:
            room = Room(chat_id, user_id, silent=not pm, pm=pm, users=users)
            ROOMS[chat_id] = room
        room.virtual[user_id] = {'audio': True, 'video': video}
    schedule(notify_main_sockets())
    return {'room_id': room.room_id, 'sess_id': room.sess_id}


def join(chat_id, user_id, video=True):
    with LOCK:
        room = ROOMS.get(chat_id)
        if room is None:
            return None
        room.virtual[user_id] = {'audio': True, 'video': video}
    schedule(_announce_join(room, user_id))
    return {'ok': True}


async def _announce_join(room, user_id):
    await broadcast(room, message('join', {'user': room.user_json(user_id)}))
    await notify_main_sockets()


def leave(chat_id, user_id):
    with LOCK:
        room = ROOMS.get(chat_id)
        if room is None or user_id not in room.virtual:
            return None
        del room.virtual[user_id]
    schedule(_user_left(room, user_id))
    return {'ok': True}


def state():
    with LOCK:
        return {
            str(chat_id): {
                'room_id': room.room_id,
                'starter_id': room.starter_id,
                'virtual': list(room.virtual),
                'rejected': list(room.rejected),
                'members': {
                    str(uid): {
                        'audio': m.audio,
                        'video': m.video,
                        'video_source': m.video_source,
                        'offers': m.offers,
                        'received': m.received,
                        'connections': {
                            str(dst): pc.connectionState
                            for dst, pc in m.pcs.items()},
                        'qualities': m.qualities,
                    } for uid, m in room.members.items()},
            } for chat_id, room in ROOMS.items()}


def configure(values):
    with LOCK:
        CONFIG.update(values)
        return dict(CONFIG)


# Room logic (calls loop).

async def _user_left(room, user_id):
    await broadcast(room, message('leave', src=user_id), exclude=user_id)
    if room.pm:
        # Like stay_in_call=false: a private call ends when one leaves.
        for uid, member in list(room.members.items()):
            await send(member.ws, message('leave', src=uid), reliable=True)
        await _close_room(room)
    elif not room.user_ids():
        await _close_room(room)
    await notify_main_sockets()


async def _close_room(room):
    with LOCK:
        if ROOMS.get(room.chat_id) is room:
            del ROOMS[room.chat_id]
        members = list(room.members.values())
        room.members.clear()
        room.virtual.clear()
    for member in members:
        for pc in member.pcs.values():
            await pc.close()
        await member.ws.close()


async def _auto_answer(room, starter_id):
    await asyncio.sleep(CONFIG['answer_delay'])
    with LOCK:
        if ROOMS.get(room.chat_id) is not room or not room.pm:
            return
        others = [uid for uid in room.pm_users if uid != starter_id]
        if not others or others[0] in room.virtual:
            return
        room.virtual[others[0]] = {'audio': True,
                                   'video': CONFIG['answer_video']}
        callee = others[0]
    log('auto answer by', callee, 'in chat', room.chat_id)
    await _announce_join(room, callee)


async def media_socket(request):
    room_id = request.match_info['room_id']
    user_id = int(request.match_info['user_id'])
    key = request.query.get('access_key', '')
    with LOCK:
        room = next((r for r in ROOMS.values() if r.room_id == room_id),
                    None)
        valid = room is not None and room.keys.get(key) == user_id
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    if not valid:
        await ws.close(code=4403)
        return ws
    member = Member(user_id, ws, secrets.token_hex(10))
    with LOCK:
        room.members[user_id] = member
        room.virtual.pop(user_id, None)
    log('user', user_id, 'joined room', room.room_id)
    await send(ws, {'type': 'approve', 'call_uid': secrets.token_hex(4),
                    'http_session_auth_key': member.auth, 'invited': False})
    users = [room.user_json(CHOIRMASTER)] + [
        room.user_json(uid) for uid in room.user_ids()]
    await send(ws, message('initial', {'users': users}), reliable=True)
    await broadcast(room, message('join', {'user': room.user_json(user_id)}),
                    exclude=user_id)
    await notify_main_sockets()
    if (room.pm and user_id == room.starter_id and CONFIG['auto_answer']
            and not room.virtual):
        asyncio.ensure_future(_auto_answer(room, user_id))
    left = False
    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                continue
            data = json.loads(msg.data)
            kind = data.get('type')
            if 'request_id' in data:
                await send(ws, {'type': 'r-confirm-from-server',
                                'confirm_id': data['request_id']})
            if kind == 'hb':
                await send(ws, {'type': 'hb', 'hb': data.get('hb')})
            elif kind == 'leave':
                left = True
                await send(ws, message('leave', src=user_id), reliable=True)
                break
            elif kind == 'message':
                inner = data.get('message', {})
                payload = inner.get('data', {})
                if payload.get('type') == 'vqualities-dict':
                    member.qualities.update(payload.get('vq_dict', {}))
    finally:
        with LOCK:
            if room.members.get(user_id) is member:
                del room.members[user_id]
        for pc in member.pcs.values():
            await pc.close()
        log('user', user_id, 'left room', room.room_id,
            '(leave)' if left else '(closed)')
        await _user_left(room, user_id)
    return ws


def _member_by_auth(auth):
    with LOCK:
        for room in ROOMS.values():
            for member in room.members.values():
                if member.auth == auth:
                    return room, member
    return None, None


async def _consume(member, track):
    try:
        while True:
            await track.recv()
            member.received[track.kind] += 1
    except Exception:  # noqa: BLE001 - the track ended.
        pass


async def session_api(request):
    path = request.match_info['path']
    room, member = _member_by_auth(
        request.headers.get('XCALL-SESSION-AUTH-KEY', ''))
    if member is None:
        return web.json_response({'message': 'unauthorized'}, status=401)
    body = await request.json()
    if path == 'sdp-offer':
        return await _sdp_offer(room, member, body)
    if path == 'stream-change':
        if 'audio' in body:
            member.audio = bool(body['audio'])
        if 'video' in body:
            member.video = bool(body['video'])
            member.video_source = (body.get('video_source') or 'video'
                                   if member.video else 'empty')
        change = {'type': 'streamchange'}
        change.update({k: body[k] for k in ('audio', 'video',
                                            'video_source') if k in body})
        await broadcast(room, message('session', change, src=member.user_id),
                        exclude=member.user_id)
    return web.json_response({'data': json.dumps({})})


async def _sdp_offer(room, member, body):
    dst = int(body.get('dst_user_id', 0))
    offer = body.get('sdp', {})
    old = member.pcs.pop(dst, None)
    if old is not None:
        await old.close()
    pc = RTCPeerConnection()
    member.pcs[dst] = pc
    member.offers += 1

    @pc.on('track')
    def on_track(track):
        if dst == CHOIRMASTER:
            asyncio.ensure_future(_consume(member, track))

    await pc.setRemoteDescription(RTCSessionDescription(
        sdp=offer.get('sdp', ''), type='offer'))
    if dst == CHOIRMASTER:
        pc.addTrack(ToneTrack())
    elif dst in room.virtual and room.virtual[dst]['video']:
        pc.addTrack(PictureTrack(dst))
    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)
    log('answered offer from', member.user_id, 'to', dst)
    return web.json_response({'data': json.dumps({
        'type': 'answer',
        'sdpAnswer': pc.localDescription.sdp,
        'timestamp': int(time.time() * 1000),
    })})


async def reject(request):
    body = await request.json()
    user_id = int(body.get('user_id', 0))
    with LOCK:
        room = next((r for r in ROOMS.values()
                     if r.room_id == body.get('room_id')), None)
        valid = room is not None and room.keys.get(
            body.get('access_key')) == user_id
    if not valid:
        return web.json_response({'message': 'bad access'}, status=403)
    room.rejected.add(user_id)
    log('user', user_id, 'rejected the call in chat', room.chat_id)
    for uid, member in list(room.members.items()):
        await send(member.ws, message('leave', src=uid), reliable=True)
    await _close_room(room)
    await notify_main_sockets()
    return web.json_response({})


async def leave_api(request):
    body = await request.json()
    user_id = int(body.get('user_id', 0))
    with LOCK:
        room = next((r for r in ROOMS.values()
                     if r.room_id == body.get('room_id')), None)
    if room is not None and room.keys.get(body.get('access_key')) == user_id:
        await _user_left(room, user_id)
    return web.json_response({})


async def main_socket(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    MAIN_SOCKETS.add(ws)
    log('main socket connected')
    try:
        async for msg in ws:
            pass
    finally:
        MAIN_SOCKETS.discard(ws)
    return ws


def _run(port):
    global LOOP
    LOOP = asyncio.new_event_loop()
    asyncio.set_event_loop(LOOP)
    app = web.Application()
    app.router.add_get('/sock/websocket', main_socket)
    app.router.add_post('/session/{path}', session_api)
    app.router.add_post('/reject', reject)
    app.router.add_post('/leave', leave_api)
    app.router.add_get('/{room_id}/{user_id}', media_socket)
    runner = web.AppRunner(app)
    LOOP.run_until_complete(runner.setup())
    LOOP.run_until_complete(web.TCPSite(runner, '127.0.0.1', port).start())
    log('mock media server and main socket on 127.0.0.1:%d' % port)
    LOOP.run_forever()


def start(port):
    global PORT
    PORT = port
    thread = threading.Thread(target=_run, args=(port,), daemon=True)
    thread.start()
