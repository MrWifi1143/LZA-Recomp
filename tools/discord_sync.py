#!/usr/bin/env python3
"""Mark Discord forum posts (known-issues, bug-reports) fixed when a release ships their fix.

A commit names the post(s) it fixes with a trailer line in its message:

    Discord: 1569946164802097223            (the post's thread id)
    Discord: https://discord.com/channels/<guild>/<thread>   (or a link to it)

Older fixes without the trailer go in tools/discord_fixes.json: {"<thread id>": "<commit>", ...}.

After a release:

    tools/discord_sync.py release --since LegendsA-v1.2.0-rc4 --until LegendsA-v1.2.0-rc5 \
        --name "Arceus 1.2.0-rc5" --url https://gitlab.com/mr.wifi1143/LZA-Recomp/-/releases/LegendsA-v1.2.0-rc5

finds every post fixed by a commit in since..until, swaps its Open tag for Closed (Fixed in bug-reports, whichever
the forum has) and replies "Fixed in <name>: <url>". --dry-run prints what it would do; --archive also closes the post.

    tools/discord_sync.py posts      lists the posts of both forums with their tags and ids
    tools/discord_sync.py --guild <id> ...   the same against a test server (needs forums named known-issues/bug-reports)
    tools/discord_sync.py pending    posts with a fix committed on this branch but not in any release tag yet

The bot token is read from ~/.config/lzarecomp/discord-token (or LZARECOMP_DISCORD_TOKEN_FILE); it is never printed.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

API = "https://discord.com/api/v10"
GUILD = os.environ.get("LZARECOMP_DISCORD_GUILD", "1569929395542425721")  # LZ&A Recomp (--guild: a test server)
FORUMS = ("known-issues", "bug-reports")
OPEN_TAGS = ("open",)
CLOSED_TAGS = ("closed", "fixed", "resolved", "solved")
PENDING_TAGS = ("fixed (unreleased)", "fixed in next release", "pending release")
TRAILER = re.compile(r"^\s*Discord:\s*(.+)$", re.I | re.M)
HERE = os.path.dirname(os.path.abspath(__file__))
MAPFILE = os.path.join(HERE, "discord_fixes.json")


def token():
    path = os.environ.get("LZARECOMP_DISCORD_TOKEN_FILE", os.path.expanduser("~/.config/lzarecomp/discord-token"))
    try:
        return open(path).read().strip()
    except OSError:
        sys.exit(f"no bot token at {path}")


TOKEN = None


def api(method, path, body=None):
    global TOKEN
    TOKEN = TOKEN or token()
    data = json.dumps(body).encode() if body is not None else None
    for _ in range(5):
        req = urllib.request.Request(API + path, data=data, method=method, headers={
            "Authorization": "Bot " + TOKEN, "Content-Type": "application/json",
            "User-Agent": "DiscordBot (https://gitlab.com/mr.wifi1143/LZA-Recomp, 1.0)"})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                raw = r.read()
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            if e.code == 429:
                time.sleep(float(json.loads(e.read() or b"{}").get("retry_after", 1)) + 0.1)
                continue
            sys.exit(f"{method} {path}: HTTP {e.code} {e.read().decode(errors='replace')[:300]}")
    sys.exit(f"{method} {path}: rate limited")


def forums():
    """{forum name: channel} for the forums we manage."""
    chans = api("GET", f"/guilds/{GUILD}/channels")
    found = {c["name"]: c for c in chans if c["type"] == 15 and c["name"] in FORUMS}
    missing = [f for f in FORUMS if f not in found]
    if missing:
        print(f"warning: forum(s) not visible to the bot: {', '.join(missing)}", file=sys.stderr)
    return found


def forum_posts(forum):
    posts = [t for t in api("GET", f"/guilds/{GUILD}/threads/active")["threads"] if t.get("parent_id") == forum["id"]]
    before = None
    while True:
        q = f"/channels/{forum['id']}/threads/archived/public?limit=100" + (f"&before={before}" if before else "")
        page = api("GET", q)
        posts += page["threads"]
        if not page.get("has_more") or not page["threads"]:
            return posts
        before = page["threads"][-1]["thread_metadata"]["archive_timestamp"]


def tag_id(forum, names):
    for t in forum.get("available_tags", []):
        if t["name"].strip().lower() in names:
            return t["id"]
    return None


def git(*args):
    return subprocess.run(["git", *args], cwd=HERE, check=True, capture_output=True, text=True).stdout


def thread_ids(text):
    ids = []
    for m in TRAILER.finditer(text):
        for tok in re.split(r"[\s,]+", m.group(1).strip()):
            nums = re.findall(r"\d{15,21}", tok)
            if nums:
                ids.append(nums[-1])  # (a link's last number is the thread)
    return ids


def fixes_in(rev_range):
    """{thread id: [(short hash, subject)]} for commits in rev_range (git range syntax)."""
    out = {}
    log = git("log", "--format=%H%x1f%h%x1f%s%x1f%B%x1e", rev_range)
    in_range = set()
    for rec in filter(str.strip, log.split("\x1e")):
        full, short, subj, body = rec.strip("\n").split("\x1f", 3)
        in_range.add(full)
        for tid in thread_ids(body):
            out.setdefault(tid, []).append((short, subj))
    if os.path.exists(MAPFILE):
        for tid, commit in json.load(open(MAPFILE)).items():
            try:
                full = git("rev-parse", "--verify", commit + "^{commit}").strip()
            except subprocess.CalledProcessError:
                continue
            if full in in_range:
                out.setdefault(tid, []).append((full[:7], git("log", "-1", "--format=%s", full).strip()))
    return out


def set_tags(forum, post, add, drop, dry):
    tags = [t for t in post.get("applied_tags", []) if t not in drop]
    if add and add not in tags:
        tags.append(add)
    if tags == post.get("applied_tags", []):
        return False
    if not dry:
        body = {"applied_tags": tags[:5]}
        if post.get("thread_metadata", {}).get("archived"):
            body["archived"] = False
        api("PATCH", f"/channels/{post['id']}", body)
    return True


def cmd_posts(a):
    for name, forum in forums().items():
        names = {t["id"]: t["name"] for t in forum.get("available_tags", [])}
        print(f"#{name}  (tags: {', '.join(names.values())})")
        for p in forum_posts(forum):
            tags = ", ".join(names.get(t, t) for t in p.get("applied_tags", []))
            arch = " [archived]" if p.get("thread_metadata", {}).get("archived") else ""
            print(f"  {p['id']}  {p['name']}  [{tags}]{arch}")


def resolve(ids):
    """{thread id: (forum, post)} for the ids that are posts in our forums."""
    fs = {f["id"]: f for f in forums().values()}
    res = {}
    for tid in ids:
        post = api("GET", f"/channels/{tid}")
        if post.get("parent_id") not in fs:
            print(f"  {tid}: not a post in {'/'.join(FORUMS)} - skipped", file=sys.stderr)
            continue
        res[tid] = (fs[post["parent_id"]], post)
    return res


def apply_fixes(fixed, name, url="", dry_run=False, no_reply=False, archive=False, log=print):
    """fixed = {thread id: [(short hash, subject)]}: close each post and reply once per release name."""
    for tid, (forum, post) in resolve(fixed).items():
        closed = tag_id(forum, CLOSED_TAGS)
        drop = {tag_id(forum, OPEN_TAGS), tag_id(forum, PENDING_TAGS)} - {None}
        log(f"#{forum['name']}: {post['name']}  <- {', '.join(c for c, _ in fixed[tid])}")
        if closed is None:
            log(f"  (no Closed/Fixed tag in #{forum['name']}: tags left as they are)")
        changed = set_tags(forum, post, closed, drop, dry_run)
        log("  tags -> closed" if changed else "  tags already closed")
        if not no_reply:
            msg = f"Fixed in **{name}**" + (f": {url}" if url else ".")
            me = api("GET", "/users/@me")["id"]
            if any(m["author"]["id"] == me and f"**{name}**" in m.get("content", "")
                   for m in api("GET", f"/channels/{tid}/messages?limit=100")):
                log("  already replied for this release")
            elif dry_run:
                log(f"  would reply: {msg}")
            else:
                api("POST", f"/channels/{tid}/messages", {"content": msg, "allowed_mentions": {"parse": []}})
                log("  replied")
        if archive and not dry_run:
            api("PATCH", f"/channels/{tid}", {"archived": True})
            log("  archived")


def cmd_release(a):
    fixed = fixes_in(f"{a.since}..{a.until}")
    if not fixed:
        print(f"no Discord-linked fixes in {a.since}..{a.until}")
        return
    apply_fixes(fixed, a.name, a.url, a.dry_run, a.no_reply, a.archive)


def cmd_pending(a):
    tags = [t for t in git("tag", "--merged", a.branch, "--sort=-creatordate").split() if re.match(a.tag_glob, t)]
    rng = f"{tags[0]}..{a.branch}" if tags else a.branch
    fixed = fixes_in(rng)
    print(f"fixes committed after {tags[0] if tags else '(no tag)'} on {a.branch}:")
    for tid, (forum, post) in resolve(fixed).items():
        print(f"  #{forum['name']}: {post['name']}  <- {', '.join(c for c, _ in fixed[tid])}")
        if a.mark:
            pend = tag_id(forum, PENDING_TAGS)
            if pend is None:
                print(f"    (no 'Fixed (unreleased)' tag in #{forum['name']})")
            elif set_tags(forum, post, pend, {tag_id(forum, OPEN_TAGS)} - {None}, a.dry_run):
                print("    tagged Fixed (unreleased)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--guild", help="server id (default LZ&A Recomp; use a test server's id to try it out)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("posts").set_defaults(fn=cmd_posts)
    r = sub.add_parser("release")
    r.add_argument("--since", required=True, help="previous release tag")
    r.add_argument("--until", default="HEAD", help="the new release tag (default HEAD)")
    r.add_argument("--name", required=True, help='e.g. "Arceus 1.2.0-rc5"')
    r.add_argument("--url", default="", help="release page link for the reply")
    r.add_argument("--dry-run", action="store_true")
    r.add_argument("--no-reply", action="store_true")
    r.add_argument("--archive", action="store_true", help="also close (archive) the post")
    r.set_defaults(fn=cmd_release)
    p = sub.add_parser("pending")
    p.add_argument("--branch", default="HEAD")
    p.add_argument("--tag-glob", default=r"Legends(A|ZA)-v")
    p.add_argument("--mark", action="store_true", help="tag them 'Fixed (unreleased)' if the forum has that tag")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(fn=cmd_pending)
    a = ap.parse_args()
    global GUILD
    GUILD = a.guild or GUILD
    a.fn(a)


if __name__ == "__main__":
    main()
