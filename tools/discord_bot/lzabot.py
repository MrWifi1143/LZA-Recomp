#!/usr/bin/env python3
"""LZ&A Helper: keeps the bot online and closes fixed Discord posts when a release appears on GitLab.

Every few minutes it reads the public GitLab project's tags. A new LegendsA-v* / LegendsZA-v* tag whose release page
exists is compared with the game's previous tag; commits with a `Discord: <thread id>` trailer (or listed in
tools/discord_fixes.json at that tag) get their posts closed and a "Fixed in <game> <version>" reply
(discord_sync.apply_fixes). The first start only records the tags that exist - old releases are never replayed.

    python3 lzabot.py              run (online, polling, /known-issue command)
    python3 lzabot.py --once -n    one check, dry run, no Discord connection (for testing)
    python3 lzabot.py --promote <post id> [--title ..] [--game ..] [--note ..] [-n]   what /known-issue does
    python3 lzabot.py --announce <tag> [-n]   post that release in #releases now
    python3 lzabot.py --updates-button [-n]   post the "Get update pings" button message in #releases (once)

Each new full release (not a release candidate) also gets one short post in #releases: the version as a link to the
release page, the first few highlights of its notes (New / Fixed / Improved) and whether it is a hotfix. The post
pings the Updates role; members give themselves that role (or take it off) with the button in #releases or /updates.
The bot needs Manage Roles (its own role above Updates) and, in #releases, Mention @everyone/All Roles.

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
ANNOUNCE = "releases"  # each full release (no release candidates) gets one short post here (announce)
NAMES = {"LegendsA": "PL:A Recomp", "LegendsZA": "PL:Z-A Recomp"}
UPDATES = "updates"  # the role pinged by the #releases post; members toggle it (button / /updates)
UPDATES_BUTTON = "lzabot:updates"  # custom_id of the persistent button


def updates_role():
    """The Updates role's id, or None."""
    return next((r["id"] for r in ds.api("GET", f"/guilds/{ds.GUILD}/roles")
                 if r["name"].strip().lower() == UPDATES), None)


def toggle_updates(user_id, has):
    """Give or take the Updates role: returns the text for the member."""
    rid = updates_role()
    if not rid:
        return "There's no Updates role yet - ask a moderator."
    had = has(rid)
    try:
        ds.api("DELETE" if had else "PUT", f"/guilds/{ds.GUILD}/members/{user_id}/roles/{rid}", {})
    except SystemExit as e:
        log(f"updates role for {user_id} failed: {e}")
        return "Couldn't change your roles right now - please tell a moderator."
    return ("Update pings off. Press the button again (or /updates) to turn them back on." if had else
            "\N{BELL} You'll get pinged when a new version is out. Press again (or /updates) to turn it off.")


def post_updates_button(dry_run=False):
    chans = [c for c in ds.api("GET", f"/guilds/{ds.GUILD}/channels") if c["name"] == ANNOUNCE and c["type"] in (0, 5)]
    body = {"content": "\N{BELL} **Want a ping when a new version is out?**\nPress the button to get the Updates role. "
                       "Press it again to take it off. `/updates` works anywhere too.",
            "allowed_mentions": {"parse": []},
            "components": [{"type": 1, "components": [{"type": 2, "style": 1, "label": "Get update pings",
                                                       "emoji": {"name": "\N{BELL}"}, "custom_id": UPDATES_BUTTON}]}]}
    if dry_run or not chans:
        log(f"would post in #{ANNOUNCE}: {json.dumps(body, ensure_ascii=False)}" if chans else f"no #{ANNOUNCE}")
        return
    ds.api("POST", f"/channels/{chans[0]['id']}/messages", body)
    log(f"button posted in #{ANNOUNCE}")


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


def highlights(notes, n=4):
    """The bold lead of the first n bullets under the notes' New / Fixed / Improved headings."""
    out, take = [], False
    for line in notes.splitlines():
        if line.startswith("#"):
            take = re.sub(r"^#+\s*", "", line).lower().startswith(("new", "fixed", "improved", "changed"))
            continue
        m = re.match(r"^[-*] \*\*(.+?)\*\*(.*)", line) if take else None
        if m:
            h = m.group(1).strip()
            if h.endswith(":"):  # (a label: the start of its sentence too, cut at a word)
                rest = re.split(r"(?<=[.;])\s|, instead", m.group(2).strip())[0].rstrip(".;")
                if len(rest) > 90:
                    rest = rest[:90].rsplit(" ", 1)[0]
                    if rest.count("(") > rest.count(")"):  # (not cut inside brackets)
                        rest = rest[:rest.rfind("(")].rstrip()
                    rest += "\u2026"
                h = h + " " + rest
            out.append(h.rstrip(".:"))
            if len(out) == n:
                break
    return out


def announce(tag, dry_run=False, log=log):
    """One post in #releases for a full release: name + version (the link), its highlights, hotfix or not."""
    game, ver = TAG.match(tag).groups()
    if re.search(r"(rc|alpha|beta)", ver, re.I):
        log(f"{tag}: release candidate - no #{ANNOUNCE} post")
        return
    notes = gl(f"/releases/{urllib.parse.quote(tag, safe='')}").get("description") or ""
    lines = [f"- {h}" for h in highlights(notes)]
    if re.search(r"^A hotfix", notes, re.M):  # (the notes of a hotfix start a line with it)
        lines.append("Hotfix: same game code, nothing to rebuild. RecompLauncher updates by itself.")
    else:
        lines.append("RecompLauncher updates by itself.")
    embed = {"title": f"{NAMES[game]} {ver}", "url": RELEASES + tag, "description": "\n".join(lines),
             "color": 0xF2C94C if game == "LegendsA" else 0x3BB3E8, "footer": {"text": "Full notes and downloads: the link above"}}
    chans = [c for c in ds.api("GET", f"/guilds/{ds.GUILD}/channels") if c["name"] == ANNOUNCE and c["type"] in (0, 5)]
    if not chans:
        log(f"{tag}: no #{ANNOUNCE} channel the bot can see - not posted")
        return
    body = {"embeds": [embed], "allowed_mentions": {"parse": []}}
    rid = updates_role()
    if rid:
        body["content"] = f"<@&{rid}>"
        body["allowed_mentions"]["roles"] = [rid]
    if dry_run:
        log(f"{tag}: would post in #{ANNOUNCE}: {json.dumps(body, ensure_ascii=False)}")
        return
    msg = ds.api("POST", f"/channels/{chans[0]['id']}/messages", body)
    if chans[0]["type"] == 5:  # (an announcement channel: published to the servers that follow it)
        try:
            ds.api("POST", f"/channels/{chans[0]['id']}/messages/{msg['id']}/crosspost")
        except SystemExit as e:
            log(f"{tag}: posted, not published: {e}")
    log(f"{tag}: posted in #{ANNOUNCE}")


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
        try:
            announce(tag, dry_run)
        except (Exception, SystemExit) as e:  # (a missing permission must not stop the fixed-post replies)
            log(f"{tag}: #{ANNOUNCE} post failed: {e!r}")
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
    ap.add_argument("--announce", metavar="TAG", help=f"post that release in #{ANNOUNCE} now (e.g. one from before this existed)")
    ap.add_argument("--updates-button", action="store_true", help=f"post the update-pings button in #{ANNOUNCE}")
    a = ap.parse_args()
    if a.updates_button:
        post_updates_button(a.dry_run)
        return
    if a.announce:
        announce(a.announce, a.dry_run)
        return
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

    async def updates(interaction: discord.Interaction):
        roles = {r.id for r in getattr(interaction.user, "roles", [])}
        text = await asyncio.to_thread(toggle_updates, interaction.user.id, lambda rid: int(rid) in roles)
        await interaction.response.send_message(text, ephemeral=True)

    @tree.command(name="updates", description="Turn pings for new LZ&A Recomp versions on or off", guild=guild)
    @app_commands.guild_only()
    async def updates_cmd(interaction: discord.Interaction):
        await updates(interaction)

    class UpdatesButton(discord.ui.View):
        def __init__(self):
            super().__init__(timeout=None)

        @discord.ui.button(label="Get update pings", emoji="\N{BELL}", style=discord.ButtonStyle.primary,
                           custom_id=UPDATES_BUTTON)
        async def press(self, interaction: discord.Interaction, button: discord.ui.Button):
            await updates(interaction)

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
        if not getattr(client, "_view", False):
            client._view = True
            client.add_view(UpdatesButton())  # (the #releases button keeps working across restarts)
        if not getattr(client, "_synced", False):
            client._synced = True
            try:
                await tree.sync(guild=guild)
                log("/known-issue + /updates registered")
            except Exception as e:
                log(f"command sync failed: {e!r}")
        await client.change_presence(activity=discord.Activity(type=discord.ActivityType.watching, name="for fixes"))
        if not getattr(client, "_polling", False):
            client._polling = True
            client.loop.create_task(poll())

    client.run(ds.token(), log_handler=None)


if __name__ == "__main__":
    main()
