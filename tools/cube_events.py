#!/usr/bin/env python3
"""Manage the cube's scheduled events from a computer (standard library only).

    export CUBE_URL=http://<cube-ip>        # or http://localhost:8080 through an SSH tunnel
    export CUBE_TOKEN=<the cube's api_token>

    cube_events.py status
    cube_events.py list [--limit N]
    cube_events.py get ID
    cube_events.py add ID TITLE --at 2026-11-09T09:00:00+00:00 [--body ...] [--icon meeting] [--kind warning] [--ttl 600]
    cube_events.py add ID TITLE --repeat weekly --time 09:30 --days mon,wed
    cube_events.py add ID --mode busy|away|relaxing|normal --at ... [--text ...] [--gif ...]   (changes the idle screen)
    cube_events.py meeting ID TITLE START_ISO [--lead 15 --lead 5] [--body ...] [--busy MINUTES]
    cube_events.py delete ID          |  clear
    cube_events.py replace FILE.json  (a JSON list of events; sent in parts, swapped in atomically)
    cube_events.py fire ID            (fire an event now, to see how it looks)

The cube fires exactly what it is given: it does no "N minutes before" arithmetic. `meeting` shows the intended
pattern: one stored event per notification (here "in 15 min" at 08:45, and "starting now" at 09:00).
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta

PART_SIZE = 12  # events per request (the cube accepts up to 16 and a 6 KB body)


def call(method, path, body=None):
    base = os.environ.get("CUBE_URL", "").rstrip("/")
    token = os.environ.get("CUBE_TOKEN", "")
    if not base or not token:
        sys.exit("set CUBE_URL and CUBE_TOKEN (see --help)")
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(base + path, data=data, method=method)
    request.add_header("Authorization", "Bearer " + token)
    if data is not None:
        request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=20) as reply:
            return json.loads(reply.read() or "{}")
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")
        sys.exit(f"HTTP {error.code}: {detail}")
    except urllib.error.URLError as error:
        sys.exit(f"cannot reach the cube: {error.reason}")


def show(value):
    print(json.dumps(value, indent=2))


def parse_instant(text):
    value = datetime.fromisoformat(text.replace("Z", "+00:00"))
    if value.tzinfo is None:
        sys.exit(f"{text!r} has no timezone offset: write for example 2026-11-09T09:00:00+00:00")
    return value


def build_event(args):
    if getattr(args, "mode", None):
        event = {"id": args.id, "mode": args.mode}
        for field in ("text", "gif"):
            if getattr(args, field, None):
                event[field] = getattr(args, field)
    else:
        event = {"id": args.id, "title": args.title}
        for field in ("body", "value", "icon", "kind", "display"):
            if getattr(args, field, None):
                event[field] = getattr(args, field)
        if args.ttl is not None:
            event["ttl_s"] = args.ttl
    if args.at:
        event["at"] = args.at
    else:
        event.update({"repeat": args.repeat, "time": args.time})
        if args.days:
            event["days"] = args.days.split(",")
        if args.day:
            event["day"] = args.day
    return event


def meeting_events(args):
    """One event per notification: each lead time, then the start itself."""
    start = parse_instant(args.start)
    events = []
    for minutes in sorted(set(args.lead), reverse=True):
        events.append({
            "id": f"{args.id}-{minutes}m", "title": f"{args.title} in {minutes} min"[:27], "icon": "meeting",
            "kind": "warning", "value": start.strftime("%H:%M"), "ttl_s": minutes * 60,
            "at": (start - timedelta(minutes=minutes)).isoformat(),
        })
    events.append({
        "id": f"{args.id}-now", "title": f"{args.title} starting"[:27], "icon": "meeting", "kind": "warning",
        "value": start.strftime("%H:%M"), "ttl_s": 600, "at": start.isoformat(),
    })
    for event in events:
        if args.body:
            event["body"] = args.body
    if args.busy:
        # the idle screen says "busy" for the length of the meeting, then goes back to normal
        events.append({"id": f"{args.id}-busy", "mode": "busy", "text": args.title[:27], "at": start.isoformat()})
        events.append({"id": f"{args.id}-free", "mode": "normal",
                       "at": (start + timedelta(minutes=args.busy)).isoformat()})
    return events


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    p = sub.add_parser("list"); p.add_argument("--limit", type=int, default=64)
    p = sub.add_parser("get"); p.add_argument("id")
    p = sub.add_parser("delete"); p.add_argument("id")
    sub.add_parser("clear")
    p = sub.add_parser("fire"); p.add_argument("id")
    p = sub.add_parser("replace"); p.add_argument("file")
    p = sub.add_parser("add"); p.add_argument("id"); p.add_argument("title", nargs="?", default="")
    p.add_argument("--mode", choices=["normal", "busy", "away", "relaxing"], help="change the idle screen instead of showing a card")
    p.add_argument("--text"); p.add_argument("--gif")
    p.add_argument("--at"); p.add_argument("--repeat", choices=["daily", "weekly", "monthly"]); p.add_argument("--time")
    p.add_argument("--days"); p.add_argument("--day", type=int)
    for field in ("body", "value", "icon", "kind", "display"):
        p.add_argument("--" + field)
    p.add_argument("--ttl", type=int)
    p = sub.add_parser("meeting"); p.add_argument("id"); p.add_argument("title"); p.add_argument("start")
    p.add_argument("--lead", type=int, action="append", default=[15]); p.add_argument("--body")
    p.add_argument("--busy", type=int, metavar="MINUTES", help="also set the screen to busy at the start, back to normal after MINUTES")
    args = parser.parse_args()

    if args.command == "status":
        show(call("GET", "/api/v1/events/status"))
    elif args.command == "list":
        show(call("GET", f"/api/v1/events?limit={args.limit}"))
    elif args.command == "get":
        show(call("GET", "/api/v1/events?id=" + urllib.parse.quote(args.id)))
    elif args.command == "delete":
        show(call("DELETE", "/api/v1/events?id=" + urllib.parse.quote(args.id)))
    elif args.command == "clear":
        show(call("DELETE", "/api/v1/events?all=true"))
    elif args.command == "fire":
        show(call("POST", "/api/v1/events/fire?id=" + urllib.parse.quote(args.id)))
    elif args.command == "add":
        if bool(args.at) == bool(args.repeat):
            sys.exit("give either --at, or --repeat with --time")
        if not args.mode and not args.title:
            sys.exit("a card needs a title (or use --mode to change the idle screen)")
        show(call("POST", "/api/v1/events", build_event(args)))
    elif args.command == "meeting":
        events = meeting_events(args)
        show(call("POST", "/api/v1/events", {"events": events}))
    elif args.command == "replace":
        with open(args.file) as handle:
            events = json.load(handle)
        parts = [events[i:i + PART_SIZE] for i in range(0, len(events), PART_SIZE)] or [[]]
        for number, part in enumerate(parts):
            reply = call("POST", "/api/v1/events/replace",
                         {"events": part, "begin": number == 0, "commit": number == len(parts) - 1})
        show(reply)


if __name__ == "__main__":
    main()
