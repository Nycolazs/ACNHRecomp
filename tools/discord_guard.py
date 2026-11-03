#!/usr/bin/env python3
"""The Discord server's spam guard (run every few minutes by .github/workflows/discord-guard.yml).

Discord's AutoMod acts the moment a message is sent; this script only keeps its rules as written here and bans
whoever fell into the trap:

- #no-chat-spam-trap: a channel nobody has a reason to write in. Any message there is blocked and its author timed
  out for 28 days at once (so a spam bot cannot go on to the other channels); this script then bans the account and
  deletes its last day of messages. The rule is exempt from every other channel, kept in step here when channels
  are added.
- Discord's own spam filter and scam phrases (fake Nitro / Steam gifts): blocked, alert in #mod-log. Mass mentions
  stay with the server's built-in "Block Mention Spam" rule (Discord's preset; the API cannot edit it).

Staff roles are never affected. Alerts arrive in #mod-log; one that led to a ban gets a hammer reaction.

DISCORD_BOT_TOKEN in the environment. --dry-run prints what it would do.
"""
import json
import os
import sys
import urllib.error
import urllib.request
import time

API = "https://discord.com/api/v10"
GUILD = "1563280028307820698"
MOD_LOG = "1567150731968913579"
TRAP = "1567150733751492648"
STAFF_ROLES = ["1563283075503230989", "1563283566442586183", "1563283089885503528", "1563282078009786480"]  # admin, mods, bots
TRAP_RULE = "Spam trap (#no-chat-spam-trap)"
HAMMER = "%F0%9F%94%A8"
TIMEOUT_28_DAYS = 28 * 24 * 3600

SCAM_KEYWORDS = [
    "*free nitro*", "*nitro for free*", "*discord nitro free*", "*steam gift*", "*free steam*", "*@everyone*nitro*",
    "*discord.gift/*", "*discordgift*", "*dlscord*", "*disc0rd*", "*discorcl*", "*steamcommunlty*", "*steamcornmunity*",
    "*stearncommunity*", "*airdrop*", "*crypto giveaway*", "*onlyfans*", "*18+ leaks*", "*nudes*",
]

TOKEN = os.environ.get("DISCORD_BOT_TOKEN", "").strip()
DRY = "--dry-run" in sys.argv


def call(method, path, body=None, reason=None):
    headers = {"Authorization": "Bot " + TOKEN, "Content-Type": "application/json",
               "User-Agent": "DiscordBot (https://github.com/Nycolazs/ACNHRecomp, 1.0)"}
    if reason:
        headers["X-Audit-Log-Reason"] = urllib.request.quote(reason)
    for _ in range(5):
        req = urllib.request.Request(API + path, data=json.dumps(body).encode() if body is not None else None,
                                     headers=headers, method=method)
        try:
            with urllib.request.urlopen(req) as f:
                data = f.read()
                return f.status, (json.loads(data) if data else None)
        except urllib.error.HTTPError as e:
            if e.code == 429:
                time.sleep(float(json.loads(e.read() or b"{}").get("retry_after", 1)) + 0.2)
                continue
            return e.code, e.read().decode()[:300]
    return 429, "rate limited"


def want_rules(channels):
    # Every channel people can write in (text, announcements, voice chat, forums, media) except the trap.
    exempt = [c["id"] for c in channels if c["type"] in (0, 2, 5, 13, 15, 16) and c["id"] != TRAP]
    alert = {"type": 2, "metadata": {"channel_id": MOD_LOG}}
    block = {"type": 1, "metadata": {"custom_message": "This message was blocked by the server's spam filter."}}
    return [
        {"name": TRAP_RULE, "event_type": 1, "trigger_type": 1,
         "trigger_metadata": {"regex_patterns": [r"[\s\S]"]},
         "actions": [{"type": 1, "metadata": {"custom_message": "This channel is a spam trap."}}, alert,
                     {"type": 3, "metadata": {"duration_seconds": TIMEOUT_28_DAYS}}],
         "enabled": True, "exempt_roles": STAFF_ROLES, "exempt_channels": exempt[:50]},
        {"name": "Scam links and phrases", "event_type": 1, "trigger_type": 1,
         "trigger_metadata": {"keyword_filter": SCAM_KEYWORDS},
         "actions": [block, alert, {"type": 3, "metadata": {"duration_seconds": 3600}}],
         "enabled": True, "exempt_roles": STAFF_ROLES, "exempt_channels": []},
        {"name": "Suspected spam", "event_type": 1, "trigger_type": 3, "trigger_metadata": {},
         "actions": [block, alert], "enabled": True, "exempt_roles": STAFF_ROLES, "exempt_channels": []},
    ]


def sync_rules():
    st, channels = call("GET", f"/guilds/{GUILD}/channels")
    if st != 200:
        sys.exit(f"channels: {st} {channels}")
    st, have = call("GET", f"/guilds/{GUILD}/auto-moderation/rules")
    if st != 200:
        sys.exit(f"rules: {st} {have}")
    by_name = {r["name"]: r for r in have}
    for rule in want_rules(channels):
        old = by_name.get(rule["name"])
        if old is None and rule["trigger_type"] == 3:  # one such rule at most: adopt it
            old = next((r for r in have if r["trigger_type"] == rule["trigger_type"]), None)
        if old is None:
            print("create rule:", rule["name"])
            if not DRY:
                st, r = call("POST", f"/guilds/{GUILD}/auto-moderation/rules", rule, "spam guard")
                print("  ", st, "" if st in (200, 201) else r)
            continue
        meta = dict(rule["trigger_metadata"])
        if rule["trigger_type"] == 1:  # Discord fills the keyword rule's other lists in
            meta = {"keyword_filter": [], "regex_patterns": [], "allow_list": [], **meta}
        same = all(json.dumps(old.get(k), sort_keys=True) == json.dumps(rule[k], sort_keys=True)
                   for k in ("name", "actions", "enabled"))
        same = same and old.get("trigger_metadata") == meta
        same = same and all(sorted(old.get(k, [])) == sorted(rule[k]) for k in ("exempt_roles", "exempt_channels"))
        if not same:
            print("update rule:", rule["name"])
            if not DRY:
                body = {k: v for k, v in rule.items() if k != "trigger_type"}
                st, r = call("PATCH", f"/guilds/{GUILD}/auto-moderation/rules/{old['id']}", body, "spam guard")
                print("  ", st, "" if st == 200 else r)


def ban_trapped():
    st, msgs = call("GET", f"/channels/{MOD_LOG}/messages?limit=100")
    if st != 200:
        sys.exit(f"mod-log: {st} {msgs}")
    for m in msgs:
        if m.get("type") != 24:  # AUTO_MODERATION_ACTION
            continue
        fields = {f["name"]: f["value"] for e in m.get("embeds", []) for f in e.get("fields", [])}
        if fields.get("rule_name") != TRAP_RULE:
            continue
        if any(r.get("me") and r["emoji"]["name"] == "\U0001F528" for r in m.get("reactions", [])):
            continue  # already banned
        user = m["author"]
        print(f"ban {user['username']} ({user['id']})")
        if DRY:
            continue
        st, r = call("PUT", f"/guilds/{GUILD}/bans/{user['id']}", {"delete_message_seconds": 86400},
                     "spam trap: posted in #no-chat-spam-trap")
        print("  ", st, "" if st in (200, 204) else r)
        if st in (200, 204, 404):
            call("PUT", f"/channels/{MOD_LOG}/messages/{m['id']}/reactions/{HAMMER}/@me")


if __name__ == "__main__":
    if not TOKEN:
        sys.exit("DISCORD_BOT_TOKEN is not set")
    sync_rules()
    ban_trapped()
