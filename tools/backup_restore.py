#!/usr/bin/env python3
"""
EmoDesk Backup & Restore Tool
=============================
Backs up everything from EmoDesk to your PC and restores it back:
  - Configuration & Settings (Device name, display, audio, power, profiles)
  - All Server URLs (cricket match server, MQTT broker, RSS feeds, iCal feeds)
  - All API Keys & Credentials (YouTube key, Telegram bot token, Wi-Fi password, MQTT logins)
  - All Cards (every card with its schedules, properties, and order)
  - Pet Care Stats & High Scores
  - All Custom Media Files (pictures in /img, animations in /anim, audio clips in /snd)

Usage:
  # Backup everything to PC
  python tools/backup_restore.py backup --host emo.local
  python tools/backup_restore.py backup --host 192.168.1.50 --out my_emo_backup.json

  # Inspect a backup file offline
  python tools/backup_restore.py inspect --file my_emo_backup.json

  # Upload and restore everything back to EmoDesk
  python tools/backup_restore.py restore --host emo.local --file my_emo_backup.json
"""

import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


def http_get(url, timeout=15):
    req = urllib.request.Request(url, headers={"User-Agent": "EmoDesk-Backup-Tool/1.2"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def http_get_json(url, timeout=15):
    data = http_get(url, timeout=timeout)
    return json.loads(data.decode("utf-8"))


def http_post_multipart(url, fields, files, timeout=20):
    boundary = "----EmoDeskBoundary" + hex(int(time.time() * 1000))[2:]
    body = bytearray()

    for k, v in fields.items():
        body.extend(f"--{boundary}\r\n".encode("utf-8"))
        body.extend(f'Content-Disposition: form-data; name="{k}"\r\n\r\n'.encode("utf-8"))
        body.extend(f"{v}\r\n".encode("utf-8"))

    for field_name, filename, file_bytes in files:
        body.extend(f"--{boundary}\r\n".encode("utf-8"))
        body.extend(
            f'Content-Disposition: form-data; name="{field_name}"; filename="{filename}"\r\n'.encode("utf-8")
        )
        body.extend(b"Content-Type: application/octet-stream\r\n\r\n")
        body.extend(file_bytes)
        body.extend(b"\r\n")

    body.extend(f"--{boundary}--\r\n".encode("utf-8"))

    req = urllib.request.Request(
        url,
        data=bytes(body),
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "User-Agent": "EmoDesk-Backup-Tool/1.2",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def http_post_json(url, obj, timeout=20):
    payload = json.dumps(obj).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=payload,
        headers={
            "Content-Type": "application/json",
            "User-Agent": "EmoDesk-Backup-Tool/1.2",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def cmd_backup(args):
    host = args.host.strip()
    if not host.startswith("http://") and not host.startswith("https://"):
        base_url = f"http://{host}"
    else:
        base_url = host

    print(f"[*] Connecting to EmoDesk at {base_url}...")
    try:
        status = http_get_json(f"{base_url}/api/status", timeout=6)
        dev_name = status.get("name", "Emo")
        fw = status.get("fw", "unknown")
        print(f"[+] Connected! Device: '{dev_name}', Firmware: v{fw}")
    except Exception as e:
        print(f"[-] Warning: could not fetch /api/status ({e}), trying direct backup...")
        dev_name = "Emo"
        fw = "unknown"

    print("[*] Fetching configuration, keys, and cards from /api/backup...")
    try:
        backup_raw = http_get(f"{base_url}/api/backup", timeout=15)
        backup_data = json.loads(backup_raw.decode("utf-8"))
    except Exception as e:
        print(f"[-] Failed to fetch backup: {e}")
        return 1

    backup_data["version"] = "1.2.0"
    backup_data["exportedAt"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

    # Media files backup
    backup_data["files"] = []
    if not args.no_media:
        print("[*] Checking for custom media files (pictures, animations, sound clips)...")
        try:
            files_meta = http_get_json(f"{base_url}/api/files", timeout=10)
            file_list = files_meta.get("files", [])
            if file_list:
                print(f"[+] Found {len(file_list)} media file(s) on EmoDesk:")
                for i, item in enumerate(file_list, 1):
                    path = item.get("path", "")
                    name = item.get("name", os.path.basename(path))
                    directory = item.get("dir", os.path.dirname(path))
                    print(f"    ({i}/{len(file_list)}) Downloading {path} ({item.get('size', 0)} bytes)...")
                    try:
                        file_bytes = http_get(f"{base_url}/api/file?path={urllib.parse.quote(path)}", timeout=15)
                        b64_data = base64.b64encode(file_bytes).decode("ascii")
                        backup_data["files"].append({
                            "path": path,
                            "dir": directory,
                            "name": name,
                            "size": len(file_bytes),
                            "data": b64_data
                        })
                    except Exception as err:
                        print(f"    [-] Failed to download {path}: {err}")
            else:
                print("[+] No custom media files found on storage.")
        except Exception as e:
            print(f"[-] Could not query /api/files: {e}")

    # Output filename
    if args.out:
        out_path = args.out
    else:
        date_str = time.strftime("%Y%m%d_%H%M%S")
        safe_name = "".join(c if c.isalnum() or c in "-_" else "_" for c in dev_name.lower())
        out_path = f"emodesk_backup_{safe_name}_{date_str}.json"

    print(f"[*] Writing backup to '{out_path}'...")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(backup_data, f, indent=2)

    file_size_kb = os.path.getsize(out_path) / 1024.0
    print("\n" + "=" * 60)
    print("BACKUP COMPLETED SUCCESSFULLY")
    print("=" * 60)
    print(f"  File:       {os.path.abspath(out_path)} ({file_size_kb:.1f} KB)")
    print_summary(backup_data)
    print("\nYou can upload this backup anytime using:")
    print(f"  python tools/backup_restore.py restore --host {host} --file {out_path}\n")
    return 0


def cmd_restore(args):
    if not os.path.isfile(args.file):
        print(f"[-] Backup file not found: {args.file}")
        return 1

    host = args.host.strip()
    if not host.startswith("http://") and not host.startswith("https://"):
        base_url = f"http://{host}"
    else:
        base_url = host

    print(f"[*] Reading and validating backup from '{args.file}'...")
    try:
        with open(args.file, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        print(f"[-] Failed to read backup JSON: {e}")
        return 1

    if "config" not in data or ("cards" not in data and "cards" not in data.get("cards", {})):
        print("[-] Invalid backup file: missing 'config' or 'cards'.")
        return 1

    print("\n" + "=" * 60)
    print("BACKUP CONTENTS TO BE RESTORED")
    print("=" * 60)
    print_summary(data)
    print("=" * 60)

    if not args.yes:
        ans = input(f"\nAre you sure you want to restore this to '{base_url}'? [y/N]: ").strip().lower()
        if ans not in ("y", "yes"):
            print("[-] Cancelled by user.")
            return 0

    print(f"\n[*] Connecting to EmoDesk at {base_url}...")
    try:
        status = http_get_json(f"{base_url}/api/status", timeout=5)
        print(f"[+] Connected to '{status.get('name', 'Emo')}'")
    except Exception as e:
        print(f"[-] Warning: could not fetch /api/status ({e}), proceeding anyway...")

    # 1. Restore media files
    media_files = data.get("files", [])
    if media_files and not args.no_media:
        print(f"[*] Restoring {len(media_files)} custom media file(s)...")
        for i, item in enumerate(media_files, 1):
            name = item.get("name", "file")
            directory = item.get("dir", "/img")
            b64_data = item.get("data", "")
            if not b64_data:
                continue
            file_bytes = base64.b64decode(b64_data)
            print(f"    ({i}/{len(media_files)}) Uploading {directory}/{name} ({len(file_bytes)} bytes)...")
            try:
                upload_url = f"{base_url}/api/upload?dir={urllib.parse.quote(directory)}&name={urllib.parse.quote(name)}"
                http_post_multipart(upload_url, fields={}, files=[("file", name, file_bytes)], timeout=20)
            except Exception as e:
                print(f"    [-] Upload error for {name}: {e}")
        print("[+] Media files uploaded.")

    # 2. Restore settings, URLs, keys & cards
    print("[*] Uploading configuration, URLs, keys, and cards to /api/restore...")
    restore_payload = {
        "config": data.get("config", {}),
        "cards": data.get("cards", {}),
        "state": data.get("state", {}),
    }

    try:
        # Use multipart file upload (handled by streaming LittleFS in web.cpp)
        payload_bytes = json.dumps(restore_payload).encode("utf-8")
        http_post_multipart(
            f"{base_url}/api/restore",
            fields={},
            files=[("backup", "restore.json", payload_bytes)],
            timeout=25,
        )
        print("[+] Settings and cards restored successfully!")
    except Exception as e:
        print(f"[-] Multipart restore failed ({e}), trying JSON body fallback...")
        try:
            http_post_json(f"{base_url}/api/restore", restore_payload, timeout=25)
            print("[+] Settings and cards restored via JSON fallback!")
        except Exception as e2:
            print(f"[-] Restore failed: {e2}")
            return 1

    print("[*] EmoDesk is restarting now...")
    print("[*] Waiting for device to come back online (up to 30 seconds)...")
    time.sleep(3)
    start_wait = time.time()
    online = False
    while time.time() - start_wait < 30:
        try:
            st = http_get_json(f"{base_url}/api/status", timeout=2)
            if st:
                online = True
                print(f"\n[+] EmoDesk is back online! Device name: '{st.get('name', 'Emo')}', IP: {st.get('ip', 'N/A')}")
                break
        except Exception:
            pass
        time.sleep(1.5)
        sys.stdout.write(".")
        sys.stdout.flush()

    if not online:
        print("\n[-] Device reboot started. Open http://" + host + " in your browser once connected.")

    print("\n" + "=" * 60)
    print("RESTORE COMPLETE!")
    print("=" * 60 + "\n")
    return 0


def cmd_inspect(args):
    if not os.path.isfile(args.file):
        print(f"[-] File not found: {args.file}")
        return 1

    try:
        with open(args.file, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        print(f"[-] Failed to read backup: {e}")
        return 1

    print("\n" + "=" * 60)
    print(f"BACKUP INSPECTION: {os.path.abspath(args.file)}")
    print("=" * 60)
    print_summary(data)
    print("=" * 60 + "\n")
    return 0


def print_summary(data):
    cfg = data.get("config", {})
    cards_obj = data.get("cards", {})
    if isinstance(cards_obj, dict):
        cards = cards_obj.get("cards", [])
    elif isinstance(cards_obj, list):
        cards = cards_obj
    else:
        cards = []

    print(f"  Device Name:      {cfg.get('name', 'Emo')}")
    print(f"  Firmware Version: {data.get('fw', 'N/A')}")
    print(f"  Backup Timestamp: {data.get('exportedAt', 'N/A')}")

    # URLs
    urls = []
    if cfg.get("cric", {}).get("url"):
        urls.append(f"Match Server: {cfg['cric']['url']}")
    if cfg.get("mqtt", {}).get("host"):
        urls.append(f"MQTT Broker: {cfg['mqtt']['host']}")
    for c in cards:
        p = c.get("p", {})
        if p.get("url"):
            urls.append(f"{c.get('name') or c.get('type')} URL: {p['url']}")

    print("\n  [Configured URLs]:")
    if urls:
        for u in urls:
            print(f"    - {u}")
    else:
        print("    (None)")

    # Keys & Credentials
    keys = []
    if cfg.get("keys", {}).get("youtube"):
        keys.append("YouTube API Key: [Present]")
    if cfg.get("tg", {}).get("token"):
        keys.append(f"Telegram Bot Token: [Present] (Chat: {cfg['tg'].get('chat', 'unpaired')})")
    if cfg.get("wifi", {}).get("ssid"):
        has_pass = bool(cfg["wifi"].get("pass") and cfg["wifi"].get("pass") != "********")
        keys.append(f"Wi-Fi SSID: {cfg['wifi']['ssid']} (Password: {'[Present]' if has_pass else '[Masked/None]'})")
    if cfg.get("mqtt", {}).get("user") or cfg.get("mqtt", {}).get("pass"):
        keys.append(f"MQTT Credentials: [User: {cfg['mqtt'].get('user', '')}]")
    for c in cards:
        p = c.get("p", {})
        if p.get("key"):
            keys.append(f"{c.get('name') or c.get('type')} API Key: [Present]")
        if p.get("token"):
            keys.append(f"{c.get('name') or c.get('type')} Token: [Present]")

    print("\n  [Keys & Credentials]:")
    if keys:
        for k in keys:
            print(f"    - {k}")
    else:
        print("    (None)")

    # Cards
    print(f"\n  [Cards ({len(cards)})]:")
    if cards:
        card_names = [f"{c.get('name') or c.get('type')} ({c.get('type')})" for c in cards]
        for name in card_names[:10]:
            print(f"    - {name}")
        if len(card_names) > 10:
            print(f"    ... and {len(card_names) - 10} more cards")
    else:
        print("    (None)")

    # Media Files
    files = data.get("files", [])
    print(f"\n  [Media Files ({len(files)})]:")
    if files:
        for f in files:
            size_kb = f.get("size", 0) / 1024.0
            print(f"    - {f.get('path', f.get('name', 'file'))} ({size_kb:.1f} KB)")
    else:
        print("    (None in backup)")


def main():
    parser = argparse.ArgumentParser(description="EmoDesk Full Backup & Restore Tool")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # backup
    p_backup = subparsers.add_parser("backup", help="Download complete backup from EmoDesk to PC")
    p_backup.add_argument("--host", default="emo.local", help="EmoDesk hostname or IP (default: emo.local)")
    p_backup.add_argument("--out", help="Output JSON backup file path")
    p_backup.add_argument("--no-media", action="store_true", help="Skip downloading media files")

    # restore
    p_restore = subparsers.add_parser("restore", help="Upload and restore a backup file to EmoDesk")
    p_restore.add_argument("--host", default="emo.local", help="EmoDesk hostname or IP (default: emo.local)")
    p_restore.add_argument("--file", required=True, help="Path to backup JSON file to restore")
    p_restore.add_argument("-y", "--yes", action="store_true", help="Skip confirmation prompt")
    p_restore.add_argument("--no-media", action="store_true", help="Skip restoring media files")

    # inspect
    p_inspect = subparsers.add_parser("inspect", help="Inspect an existing backup file offline")
    p_inspect.add_argument("--file", required=True, help="Path to backup JSON file to inspect")

    args = parser.parse_args()
    if args.command == "backup":
        sys.exit(cmd_backup(args))
    elif args.command == "restore":
        sys.exit(cmd_restore(args))
    elif args.command == "inspect":
        sys.exit(cmd_inspect(args))


if __name__ == "__main__":
    main()
