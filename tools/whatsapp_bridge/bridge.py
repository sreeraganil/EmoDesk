#!/usr/bin/env python3
"""
WhatsApp → EmoDesk Bridge
=========================
Receives WhatsApp messages via the Meta Cloud API webhook,
filters by a contact whitelist, and forwards them to EmoDesk's
POST /api/message endpoint.

Setup steps (full guide in README below):
  1. pip install flask requests
  2. Fill in EMODESKS, VERIFY_TOKEN, WHATSAPP_TOKEN, ALLOWED_NUMBERS
  3. python bridge.py
  4. Expose port 5000 via your cloud provider / ngrok / Cloudflare Tunnel
  5. Register the public URL as your Meta webhook endpoint

Usage:
  python bridge.py [--port 5000] [--host 0.0.0.0]
"""

import argparse
import hashlib
import hmac
import json
import logging
import os
import sys
import time
from typing import Optional

import requests
from flask import Flask, request, jsonify, abort

# ---------------------------------------------------------------------------
# ── CONFIGURE THESE ─────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------

# Your EmoDesk device(s).  You can list several IPs if you have multiple units.
EMODESKS: list[str] = [
    "http://emo.local",           # mDNS hostname (works on most LANs)
    # "http://192.168.1.42",      # or use a static IP
]

# A secret string you choose — paste the same value into the Meta webhook form.
VERIFY_TOKEN: str = os.environ.get("WA_VERIFY_TOKEN", "my-secret-verify-token")

# Your WhatsApp Cloud API permanent access token (from Meta developer console).
WHATSAPP_TOKEN: str = os.environ.get("WA_TOKEN", "PASTE_YOUR_TOKEN_HERE")

# Optional: App Secret for payload signature verification (recommended for production).
# Leave empty ("") to skip verification.
APP_SECRET: str = os.environ.get("WA_APP_SECRET", "")

# Whitelist — only these phone numbers (in international format without +, e.g. "919876543210")
# will be forwarded to EmoDesk.  Set to [] to allow ALL contacts.
ALLOWED_NUMBERS: list[str] = [
    # "919876543210",   # +91 98765 43210
    # "12025550100",    # +1 202-555-0100
]

# How to show the EmoDesk notification: True = popup + sound, False = silent message card
NOTIFY_WITH_POPUP: bool = True

# Emo face when a message arrives (ignored if NOTIFY_WITH_POPUP is False)
# Options: neutral happy love excited surprised sad angry fear sick sleepy
EMO_FACE: str = "happy"

# Sound on arrival (ignored if NOTIFY_WITH_POPUP is False)
# Examples: sfx:msg   sfx:bell   sfx:chime   melody:startup
SOUND: str = "sfx:msg"

# ---------------------------------------------------------------------------
# ── INTERNALS ───────────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-7s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("wa-bridge")

app = Flask(__name__)


def verify_signature(raw_body: bytes, signature_header: Optional[str]) -> bool:
    """Verify X-Hub-Signature-256 if APP_SECRET is configured."""
    if not APP_SECRET:
        return True   # signature check disabled
    if not signature_header or not signature_header.startswith("sha256="):
        return False
    expected = hmac.new(APP_SECRET.encode(), raw_body, hashlib.sha256).hexdigest()
    return hmac.compare_digest(f"sha256={expected}", signature_header)


def sender_display(wa_id: str, profile: dict) -> str:
    """Return a short display name: profile name or last-7 digits of the number."""
    name = (profile.get("name") or "").strip()
    if name:
        return name[:22]           # cap at 22 chars so it fits in lastMsg.from[24]
    return f"+…{wa_id[-7:]}"       # fallback: last 7 digits


def push_to_emodesk(from_name: str, text: str) -> None:
    """POST the message to every configured EmoDesk."""
    payload = {
        "from": from_name,
        "text": text,
        "source": "whatsapp",
    }
    if NOTIFY_WITH_POPUP:
        payload["speak"] = True
        payload["emo"]   = EMO_FACE
        payload["sound"] = SOUND
    else:
        payload["speak"] = False

    for base_url in EMODESKS:
        url = f"{base_url.rstrip('/')}/api/message"
        try:
            r = requests.post(url, json=payload, timeout=4)
            if r.ok:
                log.info("✓ Pushed to %s — %s: %s", base_url, from_name, text[:60])
            else:
                log.warning("✗ EmoDesk %s returned %d: %s", base_url, r.status_code, r.text[:120])
        except requests.exceptions.RequestException as e:
            log.error("✗ Could not reach %s: %s", base_url, e)


# ---------------------------------------------------------------------------
# ── WEBHOOK ENDPOINTS ───────────────────────────────────────────────────────
# ---------------------------------------------------------------------------

@app.get("/webhook")
def webhook_verify():
    """
    Meta sends a GET to verify the webhook endpoint.
    This must respond with the hub.challenge value.
    """
    mode      = request.args.get("hub.mode")
    token     = request.args.get("hub.verify_token")
    challenge = request.args.get("hub.challenge")

    if mode == "subscribe" and token == VERIFY_TOKEN:
        log.info("Webhook verified by Meta ✓")
        return challenge, 200
    log.warning("Webhook verification failed (bad token?)")
    abort(403)


@app.post("/webhook")
def webhook_receive():
    """
    Meta sends a POST for each incoming WhatsApp message.
    Payload reference: https://developers.facebook.com/docs/whatsapp/cloud-api/webhooks/payload-examples
    """
    # 1. Verify signature
    raw = request.get_data()
    sig = request.headers.get("X-Hub-Signature-256")
    if not verify_signature(raw, sig):
        log.warning("Rejected payload — bad signature")
        abort(403)

    data = request.get_json(silent=True) or {}

    # 2. Walk the nested structure
    for entry in data.get("entry", []):
        for change in entry.get("changes", []):
            val = change.get("value", {})

            # Only handle inbound messages (not status updates, etc.)
            messages = val.get("messages", [])
            contacts = val.get("contacts", [])
            contact_map = {c["wa_id"]: c.get("profile", {}) for c in contacts}

            for msg in messages:
                if msg.get("type") != "text":
                    # Only plain text for now; ignore image/audio/sticker/etc.
                    log.debug("Skipped non-text message type: %s", msg.get("type"))
                    continue

                wa_id = msg.get("from", "")
                text  = (msg.get("text") or {}).get("body", "").strip()

                if not text:
                    continue

                # 3. Whitelist check
                if ALLOWED_NUMBERS and wa_id not in ALLOWED_NUMBERS:
                    log.info("Ignored message from unlisted number: %s", wa_id)
                    continue

                # 4. Build display name
                profile  = contact_map.get(wa_id, {})
                from_name = sender_display(wa_id, profile)

                log.info("← WhatsApp from %s (%s): %s", from_name, wa_id, text[:80])

                # 5. Forward to EmoDesk
                push_to_emodesk(from_name, text)

    # Always return 200 quickly so Meta doesn't retry
    return jsonify({"status": "ok"}), 200


@app.get("/health")
def health():
    """Simple liveness check — useful for cloud uptime monitors."""
    return jsonify({"status": "running", "time": int(time.time())}), 200


# ---------------------------------------------------------------------------
# ── ENTRY POINT ─────────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------

def check_config() -> bool:
    ok = True
    if WHATSAPP_TOKEN == "PASTE_YOUR_TOKEN_HERE":
        log.error("Set WHATSAPP_TOKEN (env var WA_TOKEN or edit the script).")
        ok = False
    if not EMODESKS:
        log.error("EMODESKS list is empty — add at least one device URL.")
        ok = False
    if not VERIFY_TOKEN or VERIFY_TOKEN == "my-secret-verify-token":
        log.warning("VERIFY_TOKEN is using the default value — change it for security.")
    if not APP_SECRET:
        log.warning("APP_SECRET is empty — payload signature verification is disabled.")
    return ok


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="WhatsApp → EmoDesk bridge")
    parser.add_argument("--port", type=int, default=int(os.environ.get("PORT", 5000)))
    parser.add_argument("--host", default="0.0.0.0")
    args = parser.parse_args()

    if not check_config():
        sys.exit(1)

    log.info("Starting WhatsApp → EmoDesk bridge on %s:%d", args.host, args.port)
    log.info("EmoDesk targets: %s", EMODESKS)
    log.info("Whitelist: %s", ALLOWED_NUMBERS or ["ALL contacts"])

    app.run(host=args.host, port=args.port)
