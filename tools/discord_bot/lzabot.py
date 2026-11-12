#!/usr/bin/env python3
"""LZ&A Helper: keeps the bot online and closes fixed Discord posts when a release appears on GitLab.

Every few minutes it reads the public GitLab project's tags. A new LegendsA-v* / LegendsZA-v* tag whose release page
exists is compared with the game's previous tag; commits with a `Discord: <thread id>` trailer (or listed in
tools/discord_fixes.json at that tag) get their posts closed and a "Fixed in <game> <version>" reply
(discord_sync.apply_fixes). The first start only records the tags that exist - old releases are never replayed.

    python3 lzabot.py              run (online, polling, /known-issue command)
    python3 lzabot.py --once -n    one check, dry run, no Discord connection (for testing)
    python3 lzabot.py --promote <post id> [--title ..] [--game ..] [--note ..] [-n]   what /known-issue does

/known-issue (moderators: Manage Threads), used inside a post of another forum (#bug-reports ...): opens a
#known-issues post with the report's title (or `title`), a link back, the reporter and a quote of the report, tagged
Open + the game (`game`, else the report's own game tag); replies in the report with the link (no pings). The two posts
are linked (~/.local/state/lzabot/links.json): a release that fixes either one closes both.

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
LINKS = os.path.expanduser("~/.local/state/lzabot/links.json")  # {post id: linked post id} (both ways)
KNOWN = "known-issues"
GAME_TAGS = ("Arceus", "Z-A", "LegendsVR", "Legends Studio")
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


def load_links():
    try:
        return json.load(open(LINKS))
    except (OSError, ValueError):
        return {}


def link_posts(a, b):
    links = load_links()
    links[a], links[b] = b, a
    os.makedirs(os.path.dirname(LINKS), exist_ok=True)
    tmp = LINKS + ".tmp"
    json.dump(links, open(tmp, "w"), indent=1)
    os.replace(tmp, LINKS)


def with_links(fixed):
    """A fix of one post of a linked pair (a report and its known issue) closes the other one too."""
    links = load_links()
    for tid in list(fixed):
        other = links.get(tid)
        if other:
            fixed.setdefault(other, fixed[tid])
    return fixed


def promote(post_id, title="", game="", note="", dry_run=False, log=log):
    """Open a #known-issues post for the forum post post_id (a bug report): returns (known post id or None, text)."""
    post = ds.api("GET", f"/channels/{post_id}")
    chans = ds.api("GET", f"/guilds/{ds.GUILD}/channels")
    forums = {c["id"]: c for c in chans if c["type"] == 15}
    src = forums.get(post.get("parent_id"))
    known = next((c for c in forums.values() if c["name"] == KNOWN), None)
    if not src or not known:
        return None, "Use it inside a forum post (e.g. in #bug-reports); #known-issues must be visible to the bot."
    if src["id"] == known["id"]:
        return None, "This post is already in #known-issues."
    if post_id in load_links():
        return None, f"Already tracked: https://discord.com/channels/{ds.GUILD}/{load_links()[post_id]}"
    try:
        first = ds.api("GET", f"/channels/{post_id}/messages/{post_id}")  # (a forum post's first message = its id)
    except SystemExit:
        first = {}
    author = (first.get("author") or {}).get("id") or post.get("owner_id")
    text = (first.get("content") or "").strip()
    quote = "\n".join("> " + l for l in text[:1200].splitlines()) + ("\n> ..." if len(text) > 1200 else "")
    images = [a["url"] for a in first.get("attachments", [])][:4]
    src_tags = {t["id"]: t["name"] for t in src.get("available_tags", [])}
    want = [game] if game else [src_tags[t] for t in post.get("applied_tags", []) if src_tags.get(t) in GAME_TAGS]
    ktags = {t["name"].strip().lower(): t["id"] for t in known.get("available_tags", [])}
    tags = [ktags[n] for n in ["open"] + [w.lower() for w in want] if n in ktags]
    link = f"https://discord.com/channels/{ds.GUILD}/{post_id}"
    body = (note.strip() + "\n\n" if note.strip() else "") + f"Reported by <@{author}> in {link}" + \
        (f"\n{quote}" if quote else "") + ("\n" + "\n".join(images) if images else "")
    name = (title or post["name"])[:100]
    if dry_run:
        log(f"would open #{KNOWN} post '{name}' tags {tags}:\n{body[:1900]}")
        return None, "dry run"
    made = ds.api("POST", f"/channels/{known['id']}/threads", {
        "name": name, "applied_tags": tags[:5],
        "message": {"content": body[:2000], "allowed_mentions": {"parse": []}}})
    kid = made["id"]
    klink = f"https://discord.com/channels/{ds.GUILD}/{kid}"
    ds.api("POST", f"/channels/{post_id}/messages", {
        "content": f"Tracked as a known issue: {klink} - it gets closed here too when the fix is released.",
        "allowed_mentions": {"parse": []}})
    link_posts(post_id, kid)
    log(f"promoted {post_id} -> {kid} '{name}'")
    return kid, klink


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
            fixed = with_links(fixes_between(older[0], tag))
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
    ap.add_argument("--promote", metavar="POST_ID", help="what /known-issue does, for that post")
    ap.add_argument("--title", default="")
    ap.add_argument("--game", default="", choices=("",) + GAME_TAGS)
    ap.add_argument("--note", default="")
    a = ap.parse_args()
    if a.promote:
        print(promote(a.promote, a.title, a.game, a.note, a.dry_run)[1])
        return
    if a.once:
        check(a.dry_run)
        return

    import discord
    from discord import app_commands
    client = discord.Client(intents=discord.Intents.none() | discord.Intents(guilds=True))
    tree = app_commands.CommandTree(client)
    guild = discord.Object(id=int(ds.GUILD))

    @tree.command(name="known-issue", description="Open a #known-issues post for this report (linked: a release closes both)",
                  guild=guild)
    @app_commands.describe(title="known issue title (default: this post's)", game="game tag (default: this post's)",
                           note="a line to put above the report")
    @app_commands.choices(game=[app_commands.Choice(name=g, value=g) for g in GAME_TAGS])
    @app_commands.default_permissions(manage_threads=True)
    @app_commands.guild_only()
    async def known_issue(interaction: discord.Interaction, title: str = "", game: app_commands.Choice[str] = None,
                          note: str = ""):
        await interaction.response.defer(ephemeral=True, thinking=True)
        try:
            kid, text = await asyncio.to_thread(promote, str(interaction.channel_id), title, game.value if game else "",
                                                note, False)
        except SystemExit as e:  # (ds.api: an HTTP error)
            kid, text = None, f"Discord refused it: {e}"
        await interaction.followup.send(text if not kid else f"Opened {text}", ephemeral=True)

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
        if not getattr(client, "_synced", False):
            client._synced = True
            try:
                await tree.sync(guild=guild)
                log("/known-issue registered")
            except Exception as e:
                log(f"command sync failed: {e!r}")
        await client.change_presence(activity=discord.Activity(type=discord.ActivityType.watching, name="for fixes"))
        if not getattr(client, "_polling", False):
            client._polling = True
            client.loop.create_task(poll())

    client.run(ds.token(), log_handler=None)


if __name__ == "__main__":
    main()
