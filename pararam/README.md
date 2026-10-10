# Pararam Desktop

A native Windows client for [pararam.io](https://pararam.io), built from the
open-source [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
code base (v7.2.10).

The Telegram Desktop UI is kept as is. Its backend is replaced: instead of
talking MTProto to Telegram servers, every request the UI makes is answered
in-process by a translation layer that calls the pararam.io REST API.
**No connection to Telegram servers is ever made.**

```
 Telegram Desktop UI (Qt, unchanged)
        │  MTProto requests (messages.getHistory, messages.sendMessage, ...)
        ▼
 MTP::details::Session ──► Pararam::Backend  (Telegram/SourceFiles/pararam)
                               │  translates requests and builds MTProto
                               │  responses / updates from pararam data
                               ▼
                           Pararam::Api  ──HTTPS──►  api.pararam.io
                                                     file.pararam.io
```

## What works

| pararam.io                         | shown in the client as                    |
|------------------------------------|-------------------------------------------|
| Email + password (+ 2FA code) login| Custom sign-in screen                     |
| Private chats                      | Private chats with the user               |
| Group chats                        | Groups (read-only chats: posting blocked for non-admins) |
| Favorite chats                     | Pinned chats (pin / unpin syncs back)     |
| Hidden chats                       | Archived chats (archive / unarchive syncs back) |
| Chat tags                          | Chat folders (editing folders edits tags) |
| Pinned posts                       | Pinned messages bar                       |
| Posts, replies                     | Messages, replies                         |
| `**bold**`, `` `code` ``, ```` ``` ````, `~~strike~~`, `[text](url)` links, mentions, tags, quotes, spoilers | Formatted text (from the server's `text_parsed`) |
| User and chat avatars              | Profile pictures (from the file server)   |
| Files                              | Documents (download, open, save)          |
| Images (JPEG, PNG, GIF, WebP, AVIF, HEIC) | Inline photos, server side resized previews |
| Reactions (one per user, 47 kinds) | Message reactions (add, change, remove, who reacted) |
| Read marks                         | Read / unread state and counters          |
| Who read (`/msg/lastread`, `/activity/who-read`) | Double check marks, "Seen by" list in groups, read time in private chats |
| New posts, edits, deletions, reactions | Live updates (polling plus the main socket's `post_edit`) |
| Members                            | Group members list                        |
| Users / chats search               | Global search                             |
| Posts search                       | Search in a chat and global message search|
| Calls in private chats             | Voice and video calls (ringing, accept, decline, mute, camera, screen sharing) |
| Calls in group chats               | Video chats (participants, mute, camera tiles) |
| Call posts                         | Call history in chats and in the Calls list |

You can also: send, reply, edit and delete messages; send files and photos
(uploaded to pararam.io); forward messages (sent as copies); start a new
private chat from search; join and leave group chats; log out.

Features Telegram has and pararam.io does not (stickers, reactions, stories,
polls, payments, scheduled video chats, ...) are disabled or answer with
empty data.

## Download

Every push builds `Pararam.exe` on GitHub Actions
(`.github/workflows/pararam-windows.yml`). Download the
**Pararam Desktop x64** artifact from the latest successful run.

The first runs on a new repository build all dependencies (Qt, FFmpeg,
WebRTC, ...) from source, which can take longer than one GitHub Actions job
may run. Progress is cached, so just re-run the workflow until it produces
the artifact; after that each build only compiles the app. Public
repositories get faster (4-core) runners for free. The
executable is portable and keeps its data in
`%APPDATA%\Pararam Desktop`, separate from a Telegram Desktop install.

Pararam Desktop does not register `tg://` links and has the Telegram
auto-updater disabled, so it does not interfere with Telegram Desktop.

## Building

Follow [docs/building-win.md](../docs/building-win.md) (the regular Telegram
Desktop build instructions) and configure with:

```
configure.bat x64 qt6 -D TDESKTOP_API_TEST=ON ^
  -D DESKTOP_APP_DISABLE_AUTOUPDATE=ON ^
  -D DESKTOP_APP_DISABLE_CRASH_REPORTS=ON
```

The Telegram API id is not used, because no Telegram server is contacted.

## Developing without a pararam.io account

`pararam/mock_server.py` is an in-memory mock of the pararam.io API with a
few users, chats and posts:

```
python3 pararam/mock_server.py --port 8765
set PARARAM_API_URL=http://127.0.0.1:8765
set PARARAM_FILE_URL=http://127.0.0.1:8765
Pararam.exe
```

Sign in as `alice@example.com` / `password` (or `bob@example.com` /
`password` with the two-factor code `123456`). To simulate someone else
posting:

```
curl -X POST http://127.0.0.1:8765/_mock/incoming -d "{\"chat_id\": 10, \"user_id\": 3, \"text\": \"hello\"}"
```

`PARARAM_API_URL` and `PARARAM_FILE_URL` also let you point the client at a
self-hosted pararam.io installation.

### Calls with the mock server

With `pip install aiohttp aiortc numpy` the mock also runs a media server
(port 8766): calls get answered by simulated users that send a test tone and
a moving test picture. Start the client with:

```
set PARARAM_SOCK_URL=ws://127.0.0.1:8766
set PARARAM_MEDIA_INSECURE=1
set PARARAM_CALL_ALLOW_DIRECT=1
set PARARAM_CALL_ANY_CODEC=1
```

(the mock has no TLS, no TURN server and no VP9). Outgoing private calls are
answered by the other user after 2 seconds. Helpers:

```
curl -X POST http://127.0.0.1:8765/_mock/call_incoming -d "{\"chat_id\": 20, \"user_id\": 3, \"video\": true}"
curl -X POST http://127.0.0.1:8765/_mock/call_join -d "{\"chat_id\": 10, \"user_id\": 2, \"video\": true}"
curl -X POST http://127.0.0.1:8765/_mock/call_leave -d "{\"chat_id\": 20, \"user_id\": 3}"
curl -X POST http://127.0.0.1:8765/_mock/call_state -d "{}"
```

### Tests

* `pararam/tests/run.sh`: unit tests of the JSON models and the text
  formatting conversion (needs only Qt).
* `pararam/tests/run_backend_test.sh <cmake build dir>`: integration test
  that drives `Pararam::Backend` with requests serialized by the generated
  TL classes and parses the responses back, against the mock server (Linux,
  needs a configured CMake build of the project).

## How it works

* `mtproto/session.cpp`: the MTProto session never opens a socket and hands
  each serialized request to `Pararam::Backend`.
* `pararam/pararam_backend.cpp`: parses the TL request, calls pararam.io and
  replies with TL objects produced by the generated scheme code, so the UI
  sees ordinary Telegram responses. It keeps `pts` counters and an update
  log, so `updates.getDifference` and `updates.getChannelDifference` work.
* Ids: pararam user ids are Telegram user ids and group chat ids are
  supergroup ids. Message ids in groups are pararam post numbers. In private
  chats they are `slot << 20 | post_no`, because Telegram uses one message
  id space for all private chats.
* Live updates: `POST /core/chat/sync` every 3 seconds (and right away when an
  opened chat asks for updates); new posts become `updateNewMessage`, read
  marks `updateRead*Inbox`, edits and deletions are detected by re-reading the
  latest posts of changed chats.
* `pararam/pararam_api.cpp`: cookie and XSRF handling, login (with 2FA),
  JSON requests, file download and multipart upload. The session (cookies) is
  stored encrypted with the local key, like Telegram Desktop's own
  authorization data, so a local passcode protects it too.
* `intro/intro_pararam.cpp`: the pararam.io sign-in screen.
* Calls (`pararam/pararam_calls.cpp`, `pararam/calls/`): pararam.io calls go
  through its media server, which mixes the audio of a room and forwards the
  video of every participant. The backend answers the `phone.*` requests of
  the Telegram call UI (it plays the other side of the Telegram call key
  exchange, which is not used) and joins the room of the chat:
  `POST /core/call`, then the media server socket and HTTP API.
  `pararam/calls/pararam_media_engine.cpp` does the WebRTC part with the
  libwebrtc built into Telegram Desktop: one connection to the media server
  that sends our audio and video and plays the mix, and one receive-only
  connection per participant for their video. It is plugged into the call
  UI as a tgcalls implementation (`pararam_call_instance.cpp` for private
  calls, `pararam_group_instance.cpp` for video chats). Incoming calls are
  noticed through the pararam.io main socket (`GET /core/call` as a
  fallback).

The pararam.io endpoints are the ones used by the official Python client
[`pararamio`](https://gitlab.com/pararam-public/py-pararamio).

## Known limitations

* pararam.io does not report image sizes, which inline photos need, so the
  client reads them from the first 256 KB of each image, or from the
  server side resized preview (`?s=400`) when that fails (HEIC, large
  metadata). Images whose size cannot be read either way are shown as files.
* GIFs are shown as still photos.
* Reactions are shown as the standard emoji of the client's emoji set
  (pararam.io's text style ✔ and ❤ become ✔️ and ❤️ and are mapped back
  when sending), not as the pararam.io icons.
* Muting a chat is stored locally (per account), not on pararam.io.
* Calls were built from the pararam.io web client and tested against a mock
  media server only; the details of the real media server may need tuning.
* Screen sharing in video chats (group calls) is not supported yet, in
  private calls the screen replaces the camera like on pararam.io.
* Read marks of private chats update live (main socket), the ones of
  groups are refreshed every 15 seconds while the chat is open or when
  it changes.
* Forwarded messages are sent as copies with a "Forwarded from" line.
* Live updates use polling, so they arrive with a delay of up to ~3 seconds.
* Edits and deletions of old posts (beyond the last 30 of a chat) are only
  picked up when the chat history is reloaded, unless the main socket
  reports them (`post_edit`).

## License

Pararam Desktop is a modified version of Telegram Desktop and is licensed
under the same terms: GNU GPL v3 with the OpenSSL exception. See
[LICENSE](../LICENSE) and [LEGAL](../LEGAL). It is not affiliated with
Telegram or pararam.io.
