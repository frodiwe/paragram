# Pararam Desktop

A native Windows client for [pararam.io](https://pararam.io), built on the
open-source [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
code base. The Telegram Desktop UI is kept. Its MTProto backend is replaced
by an in-process translator that talks to the pararam.io REST API, so the
client never connects to Telegram servers.

See **[pararam/README.md](pararam/README.md)** for features, architecture,
build and test instructions.

Windows builds are produced by the
[Pararam Desktop for Windows](.github/workflows/pararam-windows.yml)
workflow on every push.

Based on Telegram Desktop v7.2.10. The source code is published under GPLv3
with OpenSSL exception, see [LICENSE](LICENSE). This project is not
affiliated with Telegram or pararam.io.
