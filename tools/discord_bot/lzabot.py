#!/usr/bin/env python3
"""LZ&A Helper: keeps the bot online and closes fixed Discord posts when a release appears on GitLab.

Every few minutes it reads the public GitLab project's tags. A new LegendsA-v* / LegendsZA-v* tag whose release page
exists is compared with the game's previous tag; commits with a `Discord: <thread id>` trailer (or listed in
tools/discord_fixes.json at that tag) get their posts closed and a "Fixed in <game> <version>" reply
(discord_sync.apply_fixes). The first start only records the tags that exist - old releases are never replayed.

    python3 lzabot.py              run (online, polling)
    python3 lzabot.py --once -n    one check, dry run, no Discord connection (for testing)

Needs discord.py (pip install discord.py) for the online status; token as for discord_sync.py.
State: ~/.local/state/lzabot/seen.json
"""
import argparse
import asyncio
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import discord_sync as ds  # noqa: E402

GITLAB = "https://gitlab.com/api/v4/projects/88608822"
RELEASES = "https://gitlab.com/mr.wifi1143/LZA-Recomp/-/releases/"
GAMES = {"LegendsA": "Arceus", "LegendsZA": "Z-A"}
TAG = re.compile(r"^(LegendsA|LegendsZA)-v(.+)$")
STATE = os.path.expanduser("~/.local/state/lzabot/seen.json")
POLL_S = 300


def log(*a):
    print(*a, flush=True)


def gl(path):
    req = urllib.request.Request(GITLAB + path, headers={"User-Agent": "lzabot"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def gl_exists(path):
    try:
        gl(path)
        return True
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return False
        raise


def load_seen():
    try:
        return set(json.load(open(STATE)))
    except (OSError, ValueError):
        return None


def save_seen(seen):
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    tmp = STATE + ".tmp"
    json.dump(sorted(seen), open(tmp, "w"), indent=1)
    os.replace(tmp, STATE)


def mapping_at(tag):
    try:
        raw = gl(f"/repository/files/{urllib.parse.quote('tools/discord_fixes.json', safe='')}/raw?ref={tag}")
        return raw if isinstance(raw, dict) else {}
    except urllib.error.HTTPError:
        return {}


def fixes_between(prev, tag):
    commits = gl(f"/repository/compare?from={prev}&to={tag}&straight=false")["commits"]
    fixed = {}
    for c in commits:
        for tid in ds.thread_ids(c.get("message", "")):
            fixed.setdefault(tid, []).append((c["short_id"], c["title"]))
    for tid, ref in mapping_at(tag).items():
        for c in commits:
            if c["id"].startswith(ref):
                fixed.setdefault(tid, []).append((c["short_id"], c["title"]))
    return fixed


def check(dry_run=False):
    tags = gl("/repository/tags?per_page=100&order_by=updated&sort=desc")
    names = [t["name"] for t in tags if TAG.match(t["name"])]
    seen = load_seen()
    if seen is None:
        save_seen(set(names))
        log(f"first start: {len(names)} existing release tags recorded, none replayed")
        return
    for tag in reversed(names):  # (oldest new tag first)
        if tag in seen:
            continue
        game, ver = TAG.match(tag).groups()
        if not gl_exists(f"/releases/{urllib.parse.quote(tag, safe='')}"):
            log(f"{tag}: tag without a release page yet - later")
            continue
        older = [n for n in names[names.index(tag) + 1:] if n.startswith(game + "-v")]
        if older:
            fixed = fixes_between(older[0], tag)
            log(f"{tag}: {sum(map(len, fixed.values()))} Discord-linked fix(es) since {older[0]}")
            if fixed:
                ds.apply_fixes(fixed, f"{GAMES[game]} {ver}", RELEASES + tag, dry_run=dry_run, log=log)
        if not dry_run:
            seen.add(tag)
            save_seen(seen)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--once", action="store_true", help="one check, no Discord connection")
    ap.add_argument("-n", "--dry-run", action="store_true")
    a = ap.parse_args()
    if a.once:
        check(a.dry_run)
        return

    import discord
    client = discord.Client(intents=discord.Intents.none() | discord.Intents(guilds=True))

    async def poll():
        while True:
            try:
                await asyncio.to_thread(check, a.dry_run)
            except Exception as e:  # (network hiccups: try again next round)
                log(f"check failed: {e!r}")
            await asyncio.sleep(POLL_S)

    @client.event
    async def on_ready():
        log(f"online as {client.user}")
        await client.change_presence(activity=discord.Activity(type=discord.ActivityType.watching, name="for fixes"))
        if not getattr(client, "_polling", False):
            client._polling = True
            client.loop.create_task(poll())

    client.run(ds.token(), log_handler=None)


if __name__ == "__main__":
    main()
