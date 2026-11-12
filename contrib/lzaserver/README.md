# lzaserver - an online server for Link Play

**Experiment.** A small server anyone can run so PL:A Recomp / PL:Z-A Recomp players can Link Trade (and, in Z-A,
Private Battle) over the internet without Steam. Players who enter **the same Link Code** in the same game meet on
the server, and it passes their games' traffic between them - the game's own "Nearby" play, carried over the internet.

It is not Nintendo's online service and doesn't imitate it: Ranked Battles, Mystery Gift codes and other features
that talk to Nintendo's servers stay offline.

## For players

1. Launcher > **Mods**: switch the **Steam relay** plugin on.
2. In **Online server** (same page), type the server's address, e.g. `play.example.org` or `203.0.113.5:39200`.
3. Start the game, go to the trade (Arceus: Simona in the Galaxy Hall, Link Trade, nearby; Z-A: Link Play, Link Trade
   or Private Battle, nearby) and enter a Link Code. Your friend does the same with the same code.

Pick a code nobody else would use: anyone on that server who types the same code lands in the same lobby, just like
codes on the Switch.

## Running a server

One UDP port (default **39200**): open it in the firewall and forward it on the router.

```sh
make lzaserver              # in the repository: build/lzaserver (Windows: PLATFORM=windows ... build/win/lzaserver.exe)
cc -O2 -o lzaserver lzaserver.c   # or just this file, on any machine with a C compiler
./lzaserver --port 39200 --max-clients 256
```

It prints who comes and goes (an address and a random id) and, every minute, how much it passed on. Nothing is
stored; a player is forgotten 15 s after their last packet. It uses almost no CPU: a few hundred packets a second per
trade.

### systemd

```ini
# /etc/systemd/system/lzaserver.service
[Unit]
Description=lzaserver (PL:A / PL:Z-A Recomp Link Play)
After=network-online.target

[Service]
ExecStart=/usr/local/bin/lzaserver --port 39200
DynamicUser=yes
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

### Docker

```dockerfile
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base
COPY lzaserver.c /src/
RUN cc -O2 -static -o /lzaserver /src/lzaserver.c

FROM scratch
COPY --from=build /lzaserver /lzaserver
EXPOSE 39200/udp
ENTRYPOINT ["/lzaserver", "--port", "39200"]
```

`docker build -t lzaserver . && docker run -d -p 39200:39200/udp lzaserver`

### AMP (CubeCoders)

No ready-made AMP template yet. AMP's Generic module can run any program: point it at the `lzaserver` binary with
`--port {{$ServerPort}}` (UDP) as its command line. A proper template is a to-do.

## Protocol

See the comment at the top of `lzaserver.c` (UDP; hello / lobby / hosting / data packets; a client may only speak with
the id it said hello with; data only reaches members of the same game's lobby). The game side is the relay plugin,
`contrib/steamrelay/steamrelay.c` (its "online server" mode).
