#!/usr/bin/env bash
# Runs the relay (relay.py, decision 0082) as a service on a fresh Ubuntu server: an Oracle Cloud Always Free VM,
# or any VPS. Run it once on the server, as the user that can sudo:
#
#     curl -fsSL https://raw.githubusercontent.com/ILFforever/YW4Recomp/master/tools/relay/setup_server.sh | bash
#
# or with another port / git ref:  ... | bash -s -- --port 41230 --ref master
# Running it again updates relay.py to that ref and restarts the service. What it does:
#   - relay.py from GitHub into /opt/yw4-relay (Python's standard library only: nothing else installed)
#   - a systemd service, yw4-relay, as its own throwaway user, restarted if it stops, started at boot
#   - opens the port in the server's own firewall (Oracle's Ubuntu images reject everything not allowed in
#     iptables, a step people miss). The cloud's firewall (Oracle: the subnet's security list) is set in its web
#     console, not here.
# Logs: journalctl -u yw4-relay -f    Stop: sudo systemctl stop yw4-relay    Remove: --remove
set -euo pipefail

PORT=41230
REF=master
REMOVE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --port) PORT="$2"; shift 2 ;;
        --ref) REF="$2"; shift 2 ;;
        --remove) REMOVE=1; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
case "$PORT" in ''|*[!0-9]*) echo "--port needs a number" >&2; exit 2 ;; esac

UNIT=/etc/systemd/system/yw4-relay.service
DIR=/opt/yw4-relay

if [ "$REMOVE" = 1 ]; then
    sudo systemctl disable --now yw4-relay 2>/dev/null || true
    sudo rm -f "$UNIT"
    sudo rm -rf "$DIR"
    sudo systemctl daemon-reload
    echo "relay removed (the firewall rule for port $PORT is left; sudo iptables -L INPUT --line-numbers shows it)"
    exit 0
fi

command -v python3 >/dev/null || { echo "python3 is missing: sudo apt-get install -y python3" >&2; exit 1; }

echo "== relay.py ($REF)"
sudo mkdir -p "$DIR"
tmp=$(mktemp)
curl -fsSL "https://raw.githubusercontent.com/ILFforever/YW4Recomp/$REF/tools/relay/relay.py" -o "$tmp"
python3 -m py_compile "$tmp"  # a broken download never replaces a working relay
sudo install -m 0644 "$tmp" "$DIR/relay.py"
rm -f "$tmp"

echo "== service yw4-relay on port $PORT"
sudo tee "$UNIT" >/dev/null <<EOF
[Unit]
Description=YW4 Recomp relay (playing together over the internet)
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/bin/python3 -u $DIR/relay.py --port $PORT --host 0.0.0.0
Restart=always
RestartSec=3
DynamicUser=yes
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
MemoryMax=256M

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable yw4-relay >/dev/null
sudo systemctl restart yw4-relay

echo "== firewall on this server (port $PORT/tcp)"
if command -v iptables >/dev/null && ! sudo iptables -C INPUT -p tcp --dport "$PORT" -j ACCEPT 2>/dev/null; then
    # before the image's catch-all REJECT, if it has one (Oracle's do), else at the end
    n=$(sudo iptables -L INPUT --line-numbers -n | awk '$2 == "REJECT" {print $1; exit}')
    if [ -n "$n" ]; then sudo iptables -I INPUT "$n" -p tcp --dport "$PORT" -j ACCEPT
    else sudo iptables -A INPUT -p tcp --dport "$PORT" -j ACCEPT; fi
    if command -v netfilter-persistent >/dev/null; then sudo netfilter-persistent save >/dev/null 2>&1
    else echo "   (no netfilter-persistent: the rule lasts until a reboot; sudo apt-get install -y iptables-persistent)"; fi
fi
if command -v ufw >/dev/null && sudo ufw status | grep -q "Status: active"; then sudo ufw allow "$PORT/tcp" >/dev/null; fi

sleep 1
if systemctl is-active --quiet yw4-relay; then
    echo
    echo "The relay runs. Players put these two lines in their settings.ini (one room code per group of friends):"
    echo "    YW4_RELAY=<this server's public IP, from the cloud's console>:$PORT"
    echo "    YW4_ROOM=<a room code your group agrees on>"
    echo "Still needed on Oracle: an ingress rule for TCP $PORT in the subnet's security list (web console)."
else
    echo "the service didn't start:" >&2
    sudo journalctl -u yw4-relay -n 20 --no-pager >&2
    exit 1
fi
