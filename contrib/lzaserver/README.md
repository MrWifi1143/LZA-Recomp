# lzaserver - an online server for Link Play

**Experiment.** A small server anyone can run so PL:A Recomp / PL:Z-A Recomp players can Link Trade (and, in Z-A,
Private Battle) over the internet without Steam. Players who enter **the same Link Code** in the same game meet on
the server, and it passes their games' traffic between them - the game's own "Nearby" play, carried over the internet.

It is not Nintendo's online service and doesn't imitate it: Ranked Battles, Mystery Gift codes and other features
that talk to Nintendo's servers stay offline.

## For players

Playing the game never needs an account: offline (and nearby / with Steam friends) it uses the save on your PC as
always. Playing **on a server** does: an account on that server.

1. Launcher > **Options > Online**: type the server's address, e.g. `play.example.org` or `203.0.113.5:39200`, and tick
   **Online play** (the Steam relay plugin - the same tick box as on the Mods page).
2. Under **Account**, type a name and password and press **Create account** (an account **on that server only**), or
   **Sign in** to one you made before. You stay signed in (a token in `online/accounts.txt`); **Sign out** ends it.
3. Start the game, go to the trade (Arceus: Simona in the Galaxy Hall, Link Trade, nearby; Z-A: Link Play, Link Trade
   or Private Battle, nearby) and enter a Link Code. Your friend does the same with the same code. **Show other
   players walking around** shows the server's other players in the world (Z-A).

Not signed in, the server lets you in no further: the game says "Sign in to <server> to play online there" (and at a
Link Code) - everything else plays as usual. Pick a code nobody else would use: anyone on that server who types the
same code lands in the same lobby, just like codes on the Switch.

### Your save and the account

Signing in links the save on your PC to the account (**Keep my save synced with this account**, on by default):

- the first sign-in with nothing on the server sends your save up; if the server already has one (you played on
  another PC), the newer one is used and the other is kept as a backup (`save/sync-backups/`) - the Online tab says
  which;
- while you play, the save on your PC is the one that counts (like Steam Cloud): it's synced when the game starts (if
  the server can be reached), a moment after each save, and when you quit;
- offline in between is fine - no errors; what couldn't go up goes up once the server answers again (tried every
  30 s while the game runs, and at the next start);
- if both changed (you played on two machines without syncing in between), nothing is overwritten: the newer one is
  used and the other is kept as a backup (`save/sync-backups/`, and the server keeps its last 10 versions too).
  **Sync now** and the last sync's result are on the Online tab.

The password is sent encrypted (TLS) and is never stored on your PC - only a sign-in token
(`online/accounts.txt`). The first time you sign in to a server, the game remembers that server's key; if the key
ever changes, the game refuses to talk to it ("the server's identity changed") until you press **Forget the
server's key** - only do that if the server's owner says they changed it. The game's log never holds your password,
token or the server's address.

## Running a server

One port number (default **39200**) for **UDP** (Link Play) and **TCP** (accounts and save sync): open both in the
firewall and forward both on the router.

```sh
make lzaserver              # in the repository: build/lzaserver (Windows: PLATFORM=windows ... build/win/lzaserver.exe)
./lzaserver --port 39200 --max-clients 256
./lzaserver --no-accounts  # an open server (anyone plays, no accounts) - e.g. for friends on your own LAN
cc -O2 -DLZA_NO_ACCOUNTS -o lzaserver lzaserver.c   # the same, just this file, any C compiler
```

Accounts and saves live in `--data` (default `./lzaserver-data`): back that folder up. Options:

| Option | Default | |
|---|---|---|
| `--data DIR` | `lzaserver-data` | accounts, saves, the server's own certificate |
| `--bind ADDR` | all | the address to listen on (UDP and TCP) |
| `--signup open\|closed` | open | `closed`: no new accounts from the game; make them with `lzaserver --data DIR --add-user NAME` (password on stdin) |
| `--save-max-mb N` | 32 | the largest save taken (a Z-A save is about 6 MB) |
| `--save-versions N` | 10 | versions of each game's save kept per account |
| `--quota-mb N` | 256 | per account, all versions of all games together |
| `--max-accounts N` | 1000 | |
| `--cert FILE --key FILE` | | your own certificate (PEM, e.g. Let's Encrypt for your domain) instead of the server's own |
| `--open-play` | | accounts and save sync kept, but anyone may play here without signing in |
| `--no-accounts` | | no accounts at all: anyone may play here (the old open server, e.g. on a LAN) |

By default playing here needs an account here: a game's hello must carry a play ticket (asked for over TLS with the
player's session, kept in memory 24 hours, ended with the session; the UDP hello carries only its id and a SipHash MAC
over the hello and the address's cookie - the key never goes over UDP). Without one the player is told to sign in and
nothing they send is passed on.

Security: TLS 1.3 only (Mbed TLS). Without `--cert`, the server makes its own certificate at its first start
(`<data>/tls/`, keep `key.pem` private and backed up - a new key makes every player's game refuse the server until
they press Forget the server's key) and prints its key fingerprint: publish it so players can compare. Passwords are
stored as salted Argon2id hashes (19 MiB, 2 passes), sessions as SHA-256 of random tokens (180 days unused: gone,
at most 10 devices per account). Sign-in tries: 5 wrong per account and 8 wrong per address in 10 minutes, then a
wait; 3 new accounts per address per hour; 32 connections at once, 4 per address. These counters are in memory (a
restart clears them).

It prints who comes and goes (an address and a random id) and, every minute, how much it passed on. For Link Play
nothing is stored; a player is forgotten 15 s after their last packet (accounts: above). It uses almost no CPU: a few hundred packets a second per
trade.

### systemd

```ini
# /etc/systemd/system/lzaserver.service
[Unit]
Description=lzaserver (PL:A / PL:Z-A Recomp Link Play)
After=network-online.target

[Service]
ExecStart=/usr/local/bin/lzaserver --port 39200 --data /var/lib/lzaserver
DynamicUser=yes
StateDirectory=lzaserver
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

### Docker

```dockerfile
# (build context: the repository root)
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base
COPY contrib/lzaserver /src/contrib/lzaserver
COPY runtime/acct_proto.h /src/runtime/
COPY third_party/mbedtls /src/third_party/mbedtls
COPY third_party/monocypher /src/third_party/monocypher
WORKDIR /src
RUN cc -O2 -static -Ithird_party/mbedtls/include -Ithird_party/mbedtls '-DMBEDTLS_CONFIG_FILE="lza_mbedtls_config.h"' \
       -Ithird_party/mbedtls/library -Iruntime -Ithird_party contrib/lzaserver/lzaserver.c contrib/lzaserver/lzaserver_accounts.c \
       third_party/monocypher/monocypher.c third_party/mbedtls/library/*.c -o /lzaserver -lpthread

FROM scratch
COPY --from=build /lzaserver /lzaserver
EXPOSE 39200/udp 39200/tcp
VOLUME /data
ENTRYPOINT ["/lzaserver", "--port", "39200", "--data", "/data"]
```

`docker build -f contrib/lzaserver/Dockerfile -t lzaserver . && docker run -d -p 39200:39200/udp -p 39200:39200/tcp -v lzaserver-data:/data lzaserver`
(the Dockerfile above, saved as `contrib/lzaserver/Dockerfile`: its compile line is checked with gcc, the image itself not built yet)

### AMP (CubeCoders)

No ready-made AMP template yet. AMP's Generic module can run any program: point it at the `lzaserver` binary with
`--port {{$ServerPort}}` (UDP) as its command line. A proper template is a to-do.

## Protocol

See the comment at the top of `lzaserver.c` (UDP; hello / lobby / hosting / data packets; a client may only speak with
the id it said hello with; data only reaches members of the same game's lobby). The game side is the relay plugin,
`contrib/steamrelay/steamrelay.c` (its "online server" mode). Accounts and save sync: `runtime/acct_proto.h` (TCP +
TLS), server `lzaserver_accounts.c`, game `runtime/acct.c` (in the runtime itself, so the PS5 port has it too).
