#!/usr/bin/env python3
"""Scan a tree for data that must never be published: run it before every commit.

    python3 tools/audit-privacy.py            # scans the repository root
    python3 tools/audit-privacy.py some/dir

Flags MAC addresses, private IPv4 addresses that are not the documented
examples, Wi-Fi names/passphrases, key material, e-mail addresses, personal
absolute paths, device serial numbers, and files that must not be committed
(images, firmware, calibration, keys, logs). Exit status 1 if anything is found.
It prints file and line number, never the matching text.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
SELF = pathlib.Path(__file__).resolve()

# Addresses used as examples in this repository's documentation.
ALLOWED_IPS = re.compile(r'^(?:192\.168\.1\.(?:0|1|50|100)|192\.168\.77\.\d+|192\.0\.2\.\d+|127\.\d+\.\d+\.\d+|0\.0\.0\.0|10\.0\.0\.\d+)$')
IPV4 = re.compile(r'(?<![\d.])((?:10|127|172\.(?:1[6-9]|2\d|3[01])|192\.168)\.\d{1,3}\.\d{1,3}(?:\.\d{1,3})?)(?![\d.])')
# Real (non-placeholder) IPv4 addresses of any kind, to catch public addresses too.
ANY_IPV4 = re.compile(r'(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])')
CHECKS = [
    ('MAC address', re.compile(r'(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:])')),
    ('private key block', re.compile(r'-----BEGIN [A-Z ]*PRIVATE KEY-----')),
    ('SSH public key material', re.compile(r'ssh-(?:ed25519|rsa) AAAA[0-9A-Za-z+/=]{20,}')),
    # The domain must end in letters, so "root@192.168.1.50" (ssh syntax) is not an e-mail address.
    ('e-mail address', re.compile(r'[\w.+-]+@(?!example\.(?:com|org|invalid)\b|users\.noreply\.github\.com\b)[\w-]+(?:\.[\w-]+)*\.[A-Za-z]{2,}\b')),
    ('personal absolute path', re.compile(r'(?:[A-Za-z]:\\Users\\|/home/[a-z][\w.-]*/|/Users/[A-Za-z])')),
    ('device serial / identifier', re.compile(r'(?i)(?:androidboot\.serialno|serial(?:no|number)?|imei|meid)\s*[=:]\s*[A-Za-z0-9]{8,}')),
    ('Wi-Fi name or passphrase', re.compile(r'(?im)^\s*(?:ssid|psk)\s*=\s*"?(?!EXAMPLE-NETWORK|not-a-real-passphrase|YOUR-NETWORK|YOUR-PASSPHRASE|SUA-REDE|SUA-SENHA|@)[^"\s#]+')),
]
FORBIDDEN_NAMES = re.compile(r'(?i)(?:^|/)(?:known_hosts.*|authorized_keys|id_(?:rsa|ed25519|ecdsa)(?:\.pub)?|wpa_supplicant.*\.conf'
                             r'|wifi-nvram.*|wifi-hardware-calibration.*|.*\.(?:img|img\.gz|bin|dtb|dtbo|pem|key|pub|log))$')
SKIP_DIRS = {'.git', '__pycache__'}

findings = []
for path in sorted(ROOT.rglob('*')):
    if path.is_dir() or path.resolve() == SELF or SKIP_DIRS & set(path.parts):
        continue
    rel = path.relative_to(ROOT).as_posix()
    if FORBIDDEN_NAMES.search(rel):
        findings.append((rel, 0, 'file type that must not be committed'))
        continue
    data = path.read_bytes()
    if b'\0' in data[:4096]:
        findings.append((rel, 0, 'binary file (only text is published)'))
        continue
    for number, line in enumerate(data.decode('utf-8', 'replace').splitlines(), 1):
        for label, pattern in CHECKS:
            if pattern.search(line):
                findings.append((rel, number, label))
        for match in IPV4.finditer(line):
            if not ALLOWED_IPS.match(match.group(1)):
                findings.append((rel, number, 'private IPv4 address that is not a documented example'))

for rel, number, label in findings:
    print(f'{rel}:{number}: {label}')
print(f'privacy audit: {len(findings)} finding(s) in {ROOT}')
sys.exit(1 if findings else 0)
