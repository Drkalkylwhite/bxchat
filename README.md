# BXChat

A small encrypted IRC-style chat in C: one server, many clients, a BitchX-flavoured terminal UI.

## Build

Needs a C compiler and [libsodium](https://libsodium.org).

| Platform | Setup | Build |
|---|---|---|
| Debian/Ubuntu | `sudo apt install build-essential libsodium-dev` | `make` |
| Arch | `sudo pacman -S libsodium` | `make` |
| macOS | `brew install libsodium` | `make` |
| Windows | MSYS2 MinGW shell: `mingw-w64-x86_64-{gcc,make,libsodium}` | `make` |

Produces `bxserver` and `bxclient`. (Windows 10+ console or Windows Terminal is required for the ANSI UI.)

## Run

```
bxserver [-p port] [-b bindaddr] [-w password]      # default port 7000
bxclient [-p port] [-n nick] [-w password] host
```

The password can also come from the `BXCHAT_PASSWORD` environment variable
(preferable to `-w`, which is visible in process lists). No password = open server.

## Commands

`/join #chan` `/part` `/nick x` `/msg nick text` `/me action` `/topic [text]`
`/who` `/list` `/whois nick` `/quit [reason]` `/help` `/clear`

Keys: PgUp/PgDn scrollback, Up/Down history, Left/Right/Home/End, Ctrl-A/E/K/U/W, Ctrl-L redraw, Ctrl-C quit.

## Security design

* Every connection does an ephemeral X25519 key exchange (`crypto_kx`) -> forward secrecy.
* All traffic is framed and encrypted with XSalsa20-Poly1305 (`crypto_secretbox`), with
  a per-direction key and counter nonce, so tampering, replay and reordering are detected.
* If the server has a password, `Argon2id(password, salt)` is mixed into both session keys.
  A wrong password means the keys differ and the server drops the connection; the password
  is never sent. It also authenticates the server to the client, so a man-in-the-middle
  without the password cannot join the session.
