"""LZ&A Helper: test builds for bug reports, gated by the owner.

    python3 lzabot.py --test-build <post id> --file build.zip [--what ".."] [--why ".."] [--fix ".."] [--try ".."] [-n]

--what: one line, what it fixes. --why: what was wrong, --fix: what changed - both in plain words for the reporter.

1. Staging (CLI, from the PC): the bot posts the build in #dev-approvals - the exact message the reporter will get, the
   files, and Approve / Edit / "I'll reply myself" buttons. Nothing reaches the report yet. Edit changes the text
   first; "I'll reply myself" dismisses the card (nothing is posted).
   Plain replies work the same way (lzabot.py --reply <post id> --text ..): for reports without a fix yet - asking for
   a log, saying what's going on - in the bot's own direct voice; "Send as bot" posts it.
2. Approve (the owner only - checked against the presser's user id, anyone else is turned away): the bot posts it in
   the report's thread, pinging the reporter, with numbered steps and three buttons: Fixed / Still broken / Something
   else broke. Everyone sees the buttons (Discord can't hide part of a message), but only the reporter (and the owner)
   can press them; anyone else is told to post in the thread instead.
3. Feedback: "Fixed" is noted in #dev-approvals. The other two ask what happened (a short form) and send it to
   #dev-approvals with "Make it a fix task" / "Ignore" buttons - again the owner only.
4. tools/discord_grab.py lists every answer (test-build-answers.md); only the owner-approved ones are tasks,
   everything else people say stays information. A task is always a bug fix - never a release, a push or anything else someone asks for.

Stateless on the bot side: everything a button needs is in its message (custom ids carry the post id, the embed carries
the text), so buttons keep working across restarts and the CLI can stage from another machine.
"""
import json
import os
import re
import sys
import time
import urllib.request
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import discord_sync as ds  # noqa: E402

APPROVALS = os.environ.get("LZABOT_APPROVALS", "1571298860335108136")  # #dev-approvals (owner + bot only)
OWNER = 422199944418689055  # (the only user whose presses approve anything)
TASK_OK = "FIX TASK APPROVED"  # footer prefix the grabber looks for (only the bot writes it, after the owner check)
GAME_LABEL = {"Arceus": "PL:A Recomp", "Z-A": "PL:Z-A Recomp"}
MAX_FILE = 10 * 1024 * 1024  # (a bot's upload limit on a server without boosts)


def thread_url(tid, guild=None):
    return f"https://discord.com/channels/{guild or ds.GUILD}/{tid}"


def instructions(files, what, try_, game, why="", fix=""):
    """The message the reporter gets (also shown in #dev-approvals before it goes out)."""
    names = [n for f in files for n in zip_names(f)] or [os.path.basename(f) for f in files]
    exe = [n for n in names if n.lower().endswith(".exe")]
    folder = f"the folder with {exe[0]}" if exe else "your game folder (the one with the .exe)"
    keep = ", ".join(n for n in names if "/" not in n and n.lower() != "readme.txt") or "the files it replaces"
    lines = [f"**Test build{' for ' + GAME_LABEL[game] if game in GAME_LABEL else ''}**" + (f" - {what}" if what else ""),
             ""] + ([f"**What was wrong:** {why}", ""] if why else []) + ([f"**What changed:** {fix}", ""] if fix else []) + [
             "1. Download the zip below.",
             f"2. Close the game. Open {folder}.",
             f"3. Copy your current {keep} somewhere safe (that's how you go back).",
             "4. Unzip the test build into that folder and pick **Replace**.",
             "5. Start the game the usual way." + (f" Then: {try_}" if try_ else ""),
             "6. Press one button below:",
             "   - **Fixed** - the problem is gone",
             "   - **Still broken** - same problem",
             "   - **Something else broke** - a new problem",
             "",
             "If anything's wrong, also drop `lasrecomp.log` in this thread (it's next to the .exe, close the game first)."]
    return "\n".join(lines)


def zip_names(path):
    if not path.lower().endswith(".zip"):
        return []
    import zipfile
    try:
        with zipfile.ZipFile(path) as z:
            return [i.filename for i in z.infolist() if not i.is_dir()]
    except zipfile.BadZipFile:
        return []


def multipart(method, path, payload, files):
    """ds.api with attachments."""
    ds.TOKEN = ds.TOKEN or ds.token()
    b = uuid.uuid4().hex
    parts = [f"--{b}\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n"
             .encode() + json.dumps(payload).encode() + b"\r\n"]
    for i, f in enumerate(files):
        with open(f, "rb") as fh:
            data = fh.read()
        parts.append(f"--{b}\r\nContent-Disposition: form-data; name=\"files[{i}]\"; filename=\"{os.path.basename(f)}\"\r\n"
                     "Content-Type: application/octet-stream\r\n\r\n".encode() + data + b"\r\n")
    body = b"".join(parts) + f"--{b}--\r\n".encode()
    req = urllib.request.Request(ds.API + path, data=body, method=method, headers={
        "Authorization": "Bot " + ds.TOKEN, "Content-Type": f"multipart/form-data; boundary={b}",
        "User-Agent": "DiscordBot (https://gitlab.com/mr.wifi1143/LZA-Recomp, 1.0)"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        sys.exit(f"{method} {path}: HTTP {e.code} {e.read().decode(errors='replace')[:300]}")


def button(label, cid, style):
    return {"type": 2, "style": style, "label": label, "custom_id": cid}


def _thread(post):
    t = ds.api("GET", f"/channels/{post}")
    if t.get("type") not in (11, 12):
        sys.exit(f"{post} isn't a post / thread")
    return t


def _card(kind, post, t, title, text, files, note, dry_run, log):
    for f in files:
        if os.path.getsize(f) > MAX_FILE:
            sys.exit(f"{f}: {os.path.getsize(f) >> 20} MB, over Discord's {MAX_FILE >> 20} MB - ship only the changed files")
    fields = [{"name": "Goes to", "value": f"<#{post}> (pings <@{t['owner_id']}>)", "inline": False}]
    if files:
        fields.append({"name": "Files", "value": "\n".join(f"`{os.path.basename(f)}` ({max(1, os.path.getsize(f) // 1024)} KB)"
                                                           for f in files), "inline": False})
    embed = {"title": f"{title}: {t['name']}"[:256], "url": thread_url(post, t.get("guild_id")), "description": text,
             "color": 0xF1C40F, "fields": fields, "footer": {"text": f"waiting for the owner · reporter {t['owner_id']}"}}
    payload = {"content": note, "embeds": [embed], "allowed_mentions": {"parse": []},
               "components": [{"type": 1, "components": [
                   button("Approve" if kind == "tb" else "Send as bot", f"lzabot:{kind}:ok:{post}", 3),
                   button("Edit", f"lzabot:{kind}:edit:{post}", 1),
                   button("I'll reply myself", f"lzabot:{kind}:no:{post}", 2)]}]}
    if dry_run:
        log(json.dumps(payload, indent=1))
        return None
    m = multipart("POST", f"/channels/{APPROVALS}/messages", payload, files)
    log(f"staged in #dev-approvals: {thread_url(APPROVALS)}/{m['id']}")
    return m["id"]


def stage(post, files, what="", try_="", game="", dry_run=False, log=print, why="", fix=""):
    """A test build: the approval card in #dev-approvals."""
    t = _thread(post)
    if not game:  # (the post's own game tag)
        forum = ds.api("GET", f"/channels/{t['parent_id']}")
        names = {x["id"]: x["name"] for x in forum.get("available_tags", [])}
        game = next((names[i] for i in t.get("applied_tags", []) if names.get(i) in GAME_LABEL), "")
    return _card("tb", post, t, "Test build for", instructions(files, what, try_, game, why, fix), files,
                 "New test build - approve to post it in the report.", dry_run, log)


def stage_reply(post, text, files=(), dry_run=False, log=print):
    """A reply in the bot's voice (no fix needed: asking for a log, saying what's going on): the same card, and
    "Send as bot" posts it in the report, pinging the reporter."""
    return _card("rp", post, _thread(post), "Reply for", text, list(files),
                 "Reply drafted - send it as the bot, edit it, or reply yourself.", dry_run, log)


def results(limit=200):
    """For discord_grab.py: every tester answer card in #dev-approvals (Fixed / Still broken / Something else broke),
    newest first, with its state: info (Fixed), waiting (for the owner), task (owner approved), ignored."""
    me = ds.api("GET", "/users/@me")["id"]
    out, before = [], None
    while len(out) < limit:
        q = "?limit=100" + (f"&before={before}" if before else "")
        msgs = ds.api("GET", f"/channels/{APPROVALS}/messages{q}") or []
        for m in msgs:
            for e in m.get("embeds", []) if m["author"]["id"] == me else []:
                title, foot = e.get("title", ""), (e.get("footer") or {}).get("text", "")
                if not title.startswith(("Fixed - ", "Still broken - ", "Something else broke - ")):
                    continue
                state = ("task" if foot.startswith(TASK_OK) else "info" if foot == "information only"
                         else "ignored" if foot == "Ignored" else "waiting")
                out.append({"id": m["id"], "time": m["timestamp"], "title": title, "url": e.get("url", ""), "state": state,
                            "text": e.get("description", ""), "fields": {f["name"]: f["value"] for f in e.get("fields", [])}})
        if len(msgs) < 100:
            break
        before = msgs[-1]["id"]
    return out


def approved_tasks(limit=200):
    """For discord_grab.py: the owner-approved fix tasks in #dev-approvals (newest first). A message counts only when
    the bot wrote it and its footer says approved - only the bot can write that, and only after the owner's press."""
    me = ds.api("GET", "/users/@me")["id"]
    out, before = [], None
    while len(out) < limit:
        q = "?limit=100" + (f"&before={before}" if before else "")
        msgs = ds.api("GET", f"/channels/{APPROVALS}/messages{q}") or []
        if not msgs:
            break
        for m in msgs:
            if m["author"]["id"] != me:
                continue
            for e in m.get("embeds", []):
                if (e.get("footer") or {}).get("text", "").startswith(TASK_OK):
                    out.append({"id": m["id"], "time": m["timestamp"], "title": e.get("title", ""), "url": e.get("url", ""),
                                "text": e.get("description", ""), "fields": {f["name"]: f["value"] for f in e.get("fields", [])}})
        before = msgs[-1]["id"]
        if len(msgs) < 100:
            break
    return out


RECHECK_STATE = os.path.expanduser("~/.local/state/lzabot/recheck.json")  # {post id: newest message id seen}
RECHECK_FORUMS = ("bug-reports", "help", "known-issues")


def recheck(requester="", dry_run=False, log=print):
    """/recheck (moderators): what people wrote in the report forums since the last check, as one digest card in
    #dev-approvals (only messages from others than the owner and bots count). The first run looks back 24 hours.
    Returns the line for the moderator who asked."""
    try:
        seen = json.load(open(RECHECK_STATE))
    except (OSError, ValueError):
        seen = {}
    floor = str(((int(time.time() * 1000) - 86400000) - 1420070400000) << 22)
    me = ds.api("GET", "/users/@me")["id"]
    chans = {c["id"]: c for c in ds.api("GET", f"/guilds/{ds.GUILD}/channels")
             if c["type"] == 15 and c["name"] in RECHECK_FORUMS}
    tags = {t["id"]: t["name"] for c in chans.values() for t in c.get("available_tags", [])}
    rows = []  # (an archived post that gets a message is active again: the active list is enough)
    for t in ds.api("GET", f"/guilds/{ds.GUILD}/threads/active")["threads"]:
        if t.get("parent_id") not in chans:
            continue
        after = max(int(seen.get(t["id"], 0)), int(floor))
        last = int(t.get("last_message_id") or 0)
        if last <= after:
            continue
        new_post = t["id"] not in seen and int(t["id"]) > int(floor)
        msgs = ds.api("GET", f"/channels/{t['id']}/messages?after={after}&limit=100") or []
        seen[t["id"]] = str(max([last] + [int(m["id"]) for m in msgs]))
        people = [m for m in msgs if m["author"]["id"] not in (me, str(OWNER)) and not m["author"].get("bot")]
        if not people:
            continue
        people.sort(key=lambda m: int(m["id"]))
        files = [a["filename"] for m in people for a in m.get("attachments", [])]
        said = next((m["content"] for m in reversed(people) if m["content"].strip()), "")
        said = re.sub(r"\s+", " ", re.sub(r"<@[!&]?\d+>", "@someone", said)).strip()
        rows.append((int(people[-1]["id"]), f"**[{t['name'][:80]}]({thread_url(t['id'])})** · #{chans[t['parent_id']]['name']}"
                     + "".join(f" · {tags[x]}" for x in t.get("applied_tags", []) if x in tags)
                     + f" · {'new post, ' if new_post else ''}{len(people)} new"
                     + (f" · 📎 {', '.join(files[:4])[:120]}" if files else "")
                     + (f"\n> {said[:160]}" if said else "")))
    if not dry_run:
        os.makedirs(os.path.dirname(RECHECK_STATE), exist_ok=True)
        json.dump(seen, open(RECHECK_STATE, "w"), indent=1)
    if not rows:
        return "Nothing new in the reports since the last check."
    rows.sort(reverse=True)
    desc, n = "", 0
    for _, r in rows:
        if len(desc) + len(r) > 3900:
            break
        desc += r + "\n\n"
        n += 1
    embed = {"title": f"New in the reports: {len(rows)} post(s)", "description": desc.strip()
             + (f"\n\n...and {len(rows) - n} more" if n < len(rows) else ""), "color": 0x5865F2,
             "footer": {"text": f"/recheck by {requester}" if requester else "/recheck"}}
    if dry_run:
        log(json.dumps(embed, indent=1))
    else:
        ds.api("POST", f"/channels/{APPROVALS}/messages", {"embeds": [embed], "allowed_mentions": {"parse": []}})
    return f"Found {len(rows)} report(s) with new messages - sent to the dev queue."


# ---- bot side (discord.py) ----

def items(discord, log):
    """The DynamicItem classes for client.add_dynamic_items (custom ids carry the post id)."""
    import io

    def owner(interaction):
        return interaction.user.id == OWNER

    async def deny(interaction):
        log(f"test build: {interaction.user} ({interaction.user.id}) pressed an owner button - refused")
        await interaction.response.send_message("Only the owner can do that.", ephemeral=True)

    def restyle(msg, color, footer, extra=None):
        e = msg.embeds[0] if msg.embeds else discord.Embed()
        e.color = color
        e.set_footer(text=footer[:2048])
        if extra:
            e.add_field(name=extra[0], value=extra[1][:1024], inline=False)
        return e

    LABEL = {"ok": "Approve", "no": "I'll reply myself", "edit": "Edit"}
    STYLE = {"ok": discord.ButtonStyle.success, "no": discord.ButtonStyle.secondary, "edit": discord.ButtonStyle.primary}

    class EditText(discord.ui.Modal):
        def __init__(self, text):
            super().__init__(title="Edit before it goes out", timeout=1800)
            self.text = discord.ui.TextInput(label="Message", style=discord.TextStyle.paragraph, default=text[:4000],
                                             max_length=4000)
            self.add_item(self.text)

        async def on_submit(self, interaction):
            e = interaction.message.embeds[0]
            e.description = str(self.text.value)
            await interaction.response.edit_message(embed=e)  # (buttons stay: Approve sends the edited text)

    class Review(discord.ui.DynamicItem[discord.ui.Button],
                 template=r"lzabot:(?P<kind>tb|rp):(?P<act>ok|no|edit):(?P<post>\d+)"):
        """The owner's card in #dev-approvals: tb = test build (posted with the Fixed / Still broken buttons),
        rp = a plain reply in the bot's voice. Approve posts it, Edit changes the text first, "I'll reply myself"
        dismisses it (nothing is posted)."""
        def __init__(self, kind, act, post):
            label = "Send as bot" if (kind, act) == ("rp", "ok") else LABEL[act]
            super().__init__(discord.ui.Button(label=label, style=STYLE[act], custom_id=f"lzabot:{kind}:{act}:{post}"))
            self.kind, self.act, self.post = kind, act, int(post)

        @classmethod
        async def from_custom_id(cls, interaction, item, match):
            return cls(match["kind"], match["act"], match["post"])

        async def callback(self, interaction):
            if not owner(interaction):
                return await deny(interaction)
            msg = interaction.message
            if self.act == "edit":
                return await interaction.response.send_modal(EditText(msg.embeds[0].description or ""))
            if self.act == "no":
                await interaction.response.edit_message(embed=restyle(msg, 0x95A5A6, "Dismissed - you're replying yourself"),
                                                        view=None)
                log(f"{self.kind} for {self.post}: dismissed")
                return
            await interaction.response.defer()
            e = msg.embeds[0]
            thread = interaction.client.get_channel(self.post) or await interaction.client.fetch_channel(self.post)
            files = [discord.File(io.BytesIO(await a.read()), filename=a.filename) for a in msg.attachments]
            reporter = thread.owner_id
            view = discord.utils.MISSING
            if self.kind == "tb":
                view = discord.ui.View(timeout=None)
                for kind in ("fixed", "broken", "other"):
                    view.add_item(Feedback(kind, self.post))
            body = f"<@{reporter}>\n{e.description}"
            kw = {"content": body} if len(body) <= 2000 else {"content": f"<@{reporter}>",
                                                              "embed": discord.Embed(description=e.description)}
            sent = await thread.send(**kw, files=files, view=view,
                                     allowed_mentions=discord.AllowedMentions(users=[discord.Object(reporter)]))
            await msg.edit(content=None, embed=restyle(msg, 0x2ECC71, f"Approved - posted · reporter {reporter}",
                                                       ("Posted", sent.jump_url)), view=None, attachments=msg.attachments)
            log(f"{self.kind} for {self.post}: approved, posted {sent.jump_url}")

    class Report(discord.ui.Modal):
        def __init__(self, kind, post):
            super().__init__(title="Still broken" if kind == "broken" else "Something else broke", timeout=600)
            self.kind, self.post = kind, post
            self.what = discord.ui.TextInput(label="What happened?", style=discord.TextStyle.paragraph, max_length=1500,
                                             placeholder="What you saw (crash, freeze, wrong look...)")
            self.where = discord.ui.TextInput(label="Where / what were you doing?", required=False, max_length=300,
                                              placeholder="e.g. Jubilife at night, riding Wyrdeer, in a battle")
            self.add_item(self.what)
            self.add_item(self.where)

        async def on_submit(self, interaction):
            await interaction.response.send_message(
                "Sent to the dev, thanks. If you haven't yet, drop `lasrecomp.log` in this thread (next to the .exe).",
                ephemeral=True)
            await mirror(interaction, self.kind, self.post, str(self.what.value), str(self.where.value or ""))

    async def mirror(interaction, kind, post, what="", where=""):
        ch = interaction.client.get_channel(int(APPROVALS)) or await interaction.client.fetch_channel(int(APPROVALS))
        u = interaction.user
        label = {"fixed": "Fixed", "broken": "Still broken", "other": "Something else broke"}[kind]
        e = discord.Embed(title=f"{label} - {interaction.channel.name}"[:256], url=interaction.message.jump_url,
                          color={"fixed": 0x2ECC71, "broken": 0xE74C3C, "other": 0xE67E22}[kind],
                          description=("> " + what.replace("\n", "\n> "))[:4000] if what else None)
        e.add_field(name="From", value=f"{u} ({u.id})" + (" - the reporter" if u.id == getattr(interaction.channel, "owner_id", 0) else ""),
                    inline=False)
        if where:
            e.add_field(name="Where", value=where[:1024], inline=False)
        e.add_field(name="Post", value=f"<#{post}>", inline=False)
        view = None
        if kind == "fixed":
            e.set_footer(text="information only")
        else:
            e.set_footer(text="waiting for the owner")
            view = discord.ui.View(timeout=None)
            view.add_item(Task("yes", post))
            view.add_item(Task("no", post))
        await ch.send(embed=e, view=view, allowed_mentions=discord.AllowedMentions.none())
        log(f"test build feedback for {post}: {label} from {u.id}")

    recent = {}  # (user, post) -> last press (no flooding #dev-approvals)

    class Feedback(discord.ui.DynamicItem[discord.ui.Button], template=r"lzabot:tb:fb:(?P<kind>fixed|broken|other):(?P<post>\d+)"):
        def __init__(self, kind, post):
            style = {"fixed": discord.ButtonStyle.success, "broken": discord.ButtonStyle.danger,
                     "other": discord.ButtonStyle.secondary}[kind]
            label = {"fixed": "Fixed", "broken": "Still broken", "other": "Something else broke"}[kind]
            super().__init__(discord.ui.Button(label=label, style=style, custom_id=f"lzabot:tb:fb:{kind}:{post}"))
            self.kind, self.post = kind, int(post)

        @classmethod
        async def from_custom_id(cls, interaction, item, match):
            return cls(match["kind"], match["post"])

        async def callback(self, interaction):
            reporter = getattr(interaction.channel, "owner_id", None)
            if interaction.user.id not in (reporter, OWNER):  # (only the report's author answers its test build)
                return await interaction.response.send_message(
                    "These buttons are for the person who made this report. If you're seeing the same problem, post "
                    "in this thread with what happened and your `lasrecomp.log`.", ephemeral=True)
            key = (interaction.user.id, self.post, self.kind)
            if time.time() - recent.get(key, 0) < 120:
                return await interaction.response.send_message("Already got that one, thanks.", ephemeral=True)
            recent[key] = time.time()
            if self.kind == "fixed":
                await interaction.response.send_message("Nice, noted. Thanks for testing.", ephemeral=True)
                await mirror(interaction, "fixed", self.post)
            else:
                await interaction.response.send_modal(Report(self.kind, self.post))

    class Task(discord.ui.DynamicItem[discord.ui.Button], template=r"lzabot:tb:task:(?P<act>yes|no):(?P<post>\d+)"):
        def __init__(self, act, post):
            super().__init__(discord.ui.Button(label="Make it a fix task" if act == "yes" else "Ignore",
                                               style=discord.ButtonStyle.primary if act == "yes" else discord.ButtonStyle.secondary,
                                               custom_id=f"lzabot:tb:task:{act}:{post}"))
            self.act, self.post = act, int(post)

        @classmethod
        async def from_custom_id(cls, interaction, item, match):
            return cls(match["act"], match["post"])

        async def callback(self, interaction):
            if not owner(interaction):
                return await deny(interaction)
            msg = interaction.message
            if self.act == "yes":
                e = restyle(msg, 0x3498DB, f"{TASK_OK} by {interaction.user.id} · post {self.post}")
                log(f"test build feedback for {self.post}: approved as a fix task")
            else:
                e = restyle(msg, 0x95A5A6, "Ignored")
            await interaction.response.edit_message(embed=e, view=None)

    return [Review, Feedback, Task]
