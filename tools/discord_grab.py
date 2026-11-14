#!/usr/bin/env python3
"""Fetch what's new in the Discord bug forums: messages, attached logs / videos / screenshots, video links.

    tools/discord_grab.py                 every post in #bug-reports, #known-issues and #help with messages since the last
                                          run, and #arceus / #z-a / #general-chat (only report-like lines shown)
    tools/discord_grab.py --all           everything again (the state is ignored, files already saved are kept)
    tools/discord_grab.py --thread <id>   one post, all of it
    tools/discord_grab.py --dry-run       what's new, nothing saved

New replies on OLD posts count too (people add to an old report instead of making a new one): each post's newest
message id is remembered (~/.local/state/lzarecomp/grab.json), and a post with a newer one is fetched again.

Saved under --out (default /mnt/ssd/PLA/testout/discord): <forum>/<post id>-<title>/thread.md (the new messages
appended, oldest first, with who and when) plus each attachment as <message id>-<file name>. A pasted link straight
to a video / log / archive file (catbox, Dropbox, a Discord CDN link...) is downloaded as is; video site links
(YouTube, Streamable, Medal, Twitch clips, Google Drive) are only listed: those sites' terms don't allow downloading.
A summary of what's new goes to stdout. Reporters' names stay in these local files only (never in commits/notes).
Uses the LZ&A Helper bot (tools/discord_sync.py: token in ~/.config/lzarecomp/discord-token, never printed).
"""
import argparse
import json
import os
import re
import shutil
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discord_sync as d  # noqa: E402

GRAB_FORUMS = ("bug-reports", "known-issues", "help")
GRAB_CHANNELS = ("arceus", "z-a", "general-chat")  # (chat: saved whole, only likely reports shown; first run: 3 days back)
REPORTISH = re.compile(r"crash|bug|log|regress|broke|break|error|fps|stutter|freez|hang|stuck|black screen|missing|"
                       r"doesn.?t work|not work|issue|glitch|lag|slow", re.I)
STATE = os.path.expanduser("~/.local/state/lzarecomp/grab.json")
VIDEO_LINK = re.compile(r"https?://(?:www\.)?(?:youtu\.be/|youtube\.com/(?:watch|shorts)|streamable\.com/|medal\.tv/"
                        r"|clips\.twitch\.tv/|www\.twitch\.tv/\S+/clip/|drive\.google\.com/)\S+", re.I)
DIRECT_FILE = re.compile(r"https?://\S+?\.(?:mp4|webm|mov|mkv|avi|m4v|log|txt|zip|7z|rar)(?:\?[^\s>)]*)?(?=\s|$|>|\))", re.I)
MAX_ATTACHMENT = 500 << 20


def slug(s):
    return re.sub(r"[^A-Za-z0-9]+", "-", s).strip("-")[:50] or "post"


def when(m):
    return m["timestamp"][:16].replace("T", " ")


def messages_after(tid, after):
    """The post's messages newer than `after` (a message id; 0: all), oldest first."""
    out, cur = [], str(after or 0)
    while True:
        page = d.api("GET", f"/channels/{tid}/messages?limit=100&after={cur}")
        if not page:
            return out
        page.sort(key=lambda m: int(m["id"]))
        out += page
        cur = page[-1]["id"]
        if len(page) < 100:
            return out


def download(url, path):
    if os.path.exists(path):
        return "had"
    req = urllib.request.Request(url, headers={"User-Agent": "DiscordBot (lzarecomp grab, 1.0)"})
    tmp = path + ".part"
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    os.replace(tmp, path)
    return "saved"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="/mnt/ssd/PLA/testout/discord")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--thread")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    state = {}
    if os.path.exists(STATE) and not a.all and not a.thread:
        state = json.load(open(STATE))
    chans = d.api("GET", f"/guilds/{d.GUILD}/channels")
    fs = {c["name"]: c for c in chans if c["type"] == 15}
    posts = []
    for name in GRAB_FORUMS:
        if name in fs:
            tags = {t["id"]: t["name"] for t in fs[name].get("available_tags", [])}
            posts += [(name, tags, p) for p in d.forum_posts(fs[name])]
    if not a.thread:  # (chat channels: like one long post each)
        import time
        for c in chans:
            if c["type"] == 0 and c["name"] in GRAB_CHANNELS:
                key = "chan:" + c["id"]
                if key not in state:
                    state[key] = str(((int(time.time() * 1000) - 3 * 86400000) - 1420070400000) << 22)
                state[c["id"]] = state[key]
                posts.append(("chat", {}, {"id": c["id"], "name": c["name"], "last_message_id": c.get("last_message_id"),
                                          "chat": True}))
    if a.thread:
        posts = [x for x in posts if x[2]["id"] == a.thread]
    owner = d.NO_PING  # (the user's own messages: kept, not counted as a report)
    new_total = 0
    for forum, tags, p in sorted(posts, key=lambda x: int(x[2].get("last_message_id") or 0)):
        tid, last = p["id"], int(p.get("last_message_id") or 0)
        seen = int(state.get(tid, 0))
        if last and last <= seen:
            continue
        msgs = messages_after(tid, seen)
        if not msgs:
            state[tid] = str(last)
            continue
        folder = os.path.join(a.out, forum, f"{tid}-{slug(p['name'])}")
        tagnames = ", ".join(tags.get(t, t) for t in p.get("applied_tags", []))
        others = [m for m in msgs if m["author"]["id"] not in owner]
        print(f"\n#{forum} {tid}  {p['name']}  [{tagnames}]  {len(msgs)} new ({len(others)} not yours)"
              f"{'  (an old post with new replies)' if seen else ''}")
        print(f"  https://discord.com/channels/{d.GUILD}/{tid}")
        if not a.dry_run:
            os.makedirs(folder, exist_ok=True)
        lines = []
        for m in msgs:
            who = (m["author"].get("global_name") or m["author"]["username"]) + (" (you)" if m["author"]["id"] in owner else "")
            text = m["content"].strip()
            lines.append(f"## {when(m)}  {who}  ({m['id']})\n\n{text}\n")
            short = text.replace("\n", " ")[:200]
            if not p.get("chat") or m.get("attachments") or REPORTISH.search(text):
                print(f"  {when(m)} {'you' if m['author']['id'] in owner else 'them'}: {short}")
            for at in m.get("attachments", []):
                fn = f"{m['id']}-{at['filename']}"
                size = at.get("size", 0)
                st = "listed"
                if not a.dry_run:
                    st = "too big, not fetched" if size > MAX_ATTACHMENT else download(at["url"], os.path.join(folder, fn))
                lines.append(f"- attachment: {fn} ({size >> 10} KB, {st})\n")
                print(f"    + {at['filename']} ({size >> 10} KB) {st}")
            for link in DIRECT_FILE.findall(text):  # (a pasted link to the file itself: catbox, Dropbox ?dl=1, Discord CDN...)
                name = f"{m['id']}-" + re.sub(r"[^A-Za-z0-9._-]", "_", link.split("?")[0].rsplit("/", 1)[-1])[-80:]
                st = "listed"
                if not a.dry_run:
                    try:
                        st = download(link.replace("dl=0", "dl=1"), os.path.join(folder, name))
                    except Exception as e:  # (expired / private links)
                        st = f"failed: {str(e)[:80]}"
                lines.append(f"- linked file: {link} -> {name} ({st})\n")
                print(f"    + linked file {link} {st}")
            for link in VIDEO_LINK.findall(text):
                st = "listed (watch it on the site: its terms don't allow downloading)"
                lines.append(f"- video link: {link} ({st})\n")
                print(f"    + video {link} {st}")
        if not a.dry_run:
            with open(os.path.join(folder, "thread.md"), "a") as f:
                if not seen:
                    f.write(f"# {p['name']}\n\n#{forum}, tags: {tagnames}\nhttps://discord.com/channels/{d.GUILD}/{tid}\n\n")
                f.write("\n".join(lines) + "\n")
            state[tid] = msgs[-1]["id"]
            if p.get("chat"):
                state["chan:" + tid] = msgs[-1]["id"]
        new_total += len(msgs)
    if not a.dry_run and not a.thread:
        os.makedirs(os.path.dirname(STATE), exist_ok=True)
        json.dump(state, open(STATE, "w"), indent=1)
    print(f"\n{new_total} new message(s)" + ("" if a.dry_run else f"; saved under {a.out}"))


if __name__ == "__main__":
    main()
