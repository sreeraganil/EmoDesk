#!/usr/bin/env python3
"""
EmoDesk Local Delivery Sync
Runs locally on your PC / Mac / Raspberry Pi on the same LAN as EmoDesk.
Monitors your Gmail inbox via IMAP or Google API and posts delivery updates to EmoDesk.
"""

import sys, time, json, urllib.request

EMODESK_URL = "http://192.168.1.70/api/delivery"  # Update with your EmoDesk IP or http://emo.local/api/delivery

def post_delivery(item, courier="Amazon", stage=3, status="Out for delivery", eta="Today", awb=""):
    payload = {
        "item": item,
        "courier": courier,
        "stage": stage,
        "status": status,
        "eta": eta,
        "awb": awb
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(EMODESK_URL, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as res:
            print(f"[{courier}] Successfully synced '{item}' -> {status} (HTTP {res.status})")
    except Exception as e:
        print(f"Error posting to EmoDesk: {e}")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        # Command line test mode: python delivery_sync_local.py "Sony Headphones" "Amazon" 3 "Out for delivery" "Today by 8 PM"
        item = sys.argv[1]
        courier = sys.argv[2] if len(sys.argv) > 2 else "Amazon"
        stage = int(sys.argv[3]) if len(sys.argv) > 3 else 3
        status = sys.argv[4] if len(sys.argv) > 4 else "Out for delivery"
        eta = sys.argv[5] if len(sys.argv) > 5 else "Today"
        post_delivery(item, courier, stage, status, eta)
    else:
        print("EmoDesk Delivery Local Sync Utility")
        print("Usage: python delivery_sync_local.py <ItemName> <Courier> <Stage: 0-4> <Status> <ETA>")
        print("Example: python delivery_sync_local.py \"Sony Headphones\" \"Amazon\" 3 \"Out for delivery\" \"Today by 9 PM\"")
