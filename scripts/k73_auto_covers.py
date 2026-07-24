#!/usr/bin/env python3
"""Periodically fill missing NAS covers from K73 without changing server code."""

import html
import json
import os
import re
import sys
import time
import unicodedata
import urllib.request
from pathlib import Path


NAS = os.environ.get("ESHOP_NAS", "http://127.0.0.1:40441")
K73_LIST = "http://www.k73.com/down/3ds/list-81-{}-{}.html"
SCRIPT_DIR = Path(__file__).resolve().parent
CATALOG_FILE = Path(os.environ.get(
    "K73_CATALOG_FILE", str(SCRIPT_DIR / "k73-3ds-catalog.json")))
REPORT_FILE = SCRIPT_DIR / "k73-auto-cover-report.json"
CATALOG_MAX_AGE = 7 * 24 * 60 * 60
USER_AGENT = "3DS-eShop-Cover-Matcher/1.0"


def request_bytes(url, data=None, content_type=None, timeout=40):
    headers = {"User-Agent": USER_AGENT}
    if content_type:
        headers["Content-Type"] = content_type
    request = urllib.request.Request(url, data=data, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def request_json(url, payload=None):
    data = None
    content_type = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        content_type = "application/json"
    return json.loads(request_bytes(url, data, content_type).decode("utf-8"))


def strip_tags(value):
    return html.unescape(re.sub(r"<[^>]+>", "", value)).strip()


def parse_catalog_page(page):
    entries = []
    for item in re.findall(
            r'<li\s+class="itemli">([\s\S]*?)</li>', page):
        page_match = re.search(
            r'href="(http://www\.k73\.com/down/3ds/\d+\.html)"', item)
        image_match = re.search(r'<img\s+src="([^"]+)"', item)
        title_match = re.search(r"<strong>([\s\S]*?)</strong>", item)
        desc_match = re.search(
            r'<span\s+class="daodu">([\s\S]*?)</span>', item)
        if not (page_match and image_match and title_match):
            continue
        image_url = image_match.group(1)
        if image_url.startswith("//"):
            image_url = "http:" + image_url
        elif image_url.startswith("/"):
            image_url = "http://www.k73.com" + image_url
        entries.append({
            "title": strip_tags(title_match.group(1)),
            "description": strip_tags(desc_match.group(1))
            if desc_match else "",
            "page_url": page_match.group(1),
            "image_url": image_url,
        })
    return entries


def load_catalog():
    if (CATALOG_FILE.exists() and
            time.time() - CATALOG_FILE.stat().st_mtime < CATALOG_MAX_AGE):
        return json.loads(CATALOG_FILE.read_text("utf-8"))

    entries = {}
    for page_number in range(1, 37):
        url = K73_LIST.format(81, page_number)
        page = request_bytes(url).decode("utf-8", "replace")
        for entry in parse_catalog_page(page):
            entries[entry["page_url"]] = entry
        time.sleep(0.25)
    catalog = list(entries.values())
    CATALOG_FILE.write_text(
        json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", "utf-8")
    return catalog


ANNOTATION_RE = re.compile(r"[\(（\[【][^\)）\]】]*[\)）\]】]")
METADATA_RE = re.compile(
    r"(完美|完全|最终|正式|修正|破解|官方|官译|简体|繁体|中文|汉化|"
    r"日版|美版|欧版|韩版|中文版|汉化版|下载版|下载|补丁|升级|更新|"
    r"本体|游戏版|无需跨区|需字库工具|cia|rom)",
    re.IGNORECASE)
VERSION_RE = re.compile(r"v(?:er)?\.?\d+(?:\.\d+)*", re.IGNORECASE)


def normalize(value):
    value = unicodedata.normalize("NFKC", value).lower()
    value = METADATA_RE.sub("", value)
    value = VERSION_RE.sub("", value)
    return "".join(character for character in value if character.isalnum())


def variants(value):
    candidates = {normalize(value), normalize(ANNOTATION_RE.sub(" ", value))}
    for annotation in re.findall(
            r"[\(（\[【]([^\)）\]】]+)[\)）\]】]", value):
        candidates.add(normalize(annotation))
    return [candidate for candidate in candidates if len(candidate) >= 3]


def dice_coefficient(left, right):
    if left == right:
        return 1.0
    if len(left) < 2 or len(right) < 2:
        return 0.0
    pairs = {}
    for index in range(len(left) - 1):
        pair = left[index:index + 2]
        pairs[pair] = pairs.get(pair, 0) + 1
    overlap = 0
    for index in range(len(right) - 1):
        pair = right[index:index + 2]
        remaining = pairs.get(pair, 0)
        if remaining > 0:
            overlap += 1
            pairs[pair] = remaining - 1
    return 2.0 * overlap / (len(left) + len(right) - 2)


def title_score(local_title, remote_title):
    best = 0.0
    for left in variants(local_title):
        for right in variants(remote_title):
            if left == right:
                return 1.0
            shorter = min(len(left), len(right))
            longer = max(len(left), len(right))
            if shorter >= 4 and (left in right or right in left):
                best = max(best, 0.86 + 0.12 * shorter / longer)
            best = max(best, dice_coefficient(left, right))
    return best


def build_matches(games, catalog):
    matches = []
    skipped = []
    for game in games:
        if game.get("has_cover"):
            continue
        if not any(len(item) >= 4 for item in variants(game["title"])):
            skipped.append({
                "id": game["id"], "title": game["title"],
                "reason": "generic title"})
            continue
        ranked = sorted(
            ((title_score(game["title"], entry["title"]), entry)
             for entry in catalog),
            key=lambda item: item[0],
            reverse=True)
        best_score, best_entry = ranked[0]
        second_score, second_entry = ranked[1]
        ambiguous = (
            best_score != 1.0 and best_score - second_score < 0.025 and
            best_entry["title"] != second_entry["title"])
        if best_score < 0.80 or ambiguous:
            skipped.append({
                "id": game["id"], "title": game["title"],
                "reason": "ambiguous" if ambiguous else "low score",
                "candidate": best_entry["title"],
                "score": round(best_score, 3)})
            continue
        matches.append({
            "id": game["id"],
            "title": game["title"],
            "k73_title": best_entry["title"],
            "score": round(best_score, 3),
            "image_url": best_entry["image_url"],
            "page_url": best_entry["page_url"],
        })
    return matches, skipped


def main(apply_changes=True):
    catalog = load_catalog()
    response = request_json(NAS + "/api/games")
    games = response.get("games", response)
    matches, skipped = build_matches(games, catalog)
    updated = 0
    errors = []
    if apply_changes:
        for match in matches:
            try:
                result = request_json(
                    "{}/api/games/{}/cover".format(NAS, match["id"]),
                    {"url": match["image_url"]})
                if not result.get("ok"):
                    raise RuntimeError(result.get("error", "unknown API error"))
                updated += 1
            except Exception as error:  # Keep the next scheduled run alive.
                errors.append({
                    "id": match["id"],
                    "title": match["title"],
                    "error": str(error),
                })
            time.sleep(0.2)

    report = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "catalog_entries": len(catalog),
        "apply": apply_changes,
        "updated": updated,
        "errors": errors,
        "matches": matches,
        "skipped": skipped,
    }
    REPORT_FILE.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", "utf-8")
    print("K73 auto covers: {} updated, {} skipped, {} errors".format(
        updated, len(skipped), len(errors)))


if __name__ == "__main__":
    if "--daemon" in sys.argv:
        while True:
            try:
                main(True)
            except Exception as error:
                print("K73 auto covers failed: {}".format(error), flush=True)
            time.sleep(6 * 60 * 60)
    else:
        main("--dry-run" not in sys.argv)
