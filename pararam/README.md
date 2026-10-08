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
| Hidden chats                       | Archived chats                            |
| Posts, replies                     | Messages, replies                         |
| `**bold**`, `` `code` ``, ```` ``` ````, `~~strike~~`, links, mentions | Formatted text |
| Files                              | Documents (download, open, save)          |
| Read marks                         | Read / unread state and counters          |
| New posts, edits and deletions     | Live updates (polled every few seconds)   |
| Members                            | Group members list                        |
| Users / chats search               | Global search                             |
| Posts search                       | Search in a chat and global message search|

You can also: send, reply, edit and delete messages; send files and photos
(uploaded to pararam.io); forward messages (sent as copies); start a new
private chat from search; join and leave group chats; log out.

Features Telegram has and pararam.io does not (stickers, reactions, stories,
calls, polls, payments, ...) are disabled or answer with empty data.

## Download

Every push builds `Pararam.exe` on GitHub Actions
(`.github/workflows/pararam-windows.yml`). Download the
**Pararam Desktop x64** artifact from the latest successful run. The
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

The pararam.io endpoints are the ones used by the official Python client
[`pararamio`](https://gitlab.com/pararam-public/py-pararamio).

## Known limitations

* Images are shown as files (pararam.io does not report image sizes, which
  inline photos need). Opening them shows the image.
* No avatars yet.
* Outgoing messages show a single check mark: pararam.io does not report
  per-chat read receipts.
* Forwarded messages are sent as copies with a "Forwarded from" line.
* Live updates use polling, so they arrive with a delay of up to ~3 seconds.
* Edits and deletions of old posts (beyond the last 30 of a chat) are only
  picked up when the chat history is reloaded.

## License

Pararam Desktop is a modified version of Telegram Desktop and is licensed
under the same terms: GNU GPL v3 with the OpenSSL exception. See
[LICENSE](../LICENSE) and [LEGAL](../LEGAL). It is not affiliated with
Telegram or pararam.io.
