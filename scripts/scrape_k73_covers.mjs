#!/usr/bin/env node

import fs from "node:fs/promises";
import path from "node:path";

const K73_LIST = "http://www.k73.com/down/3ds/list-81-";
const NAS = process.env.ESHOP_NAS ?? "http://127.0.0.1:40441";
const ROOT = path.resolve(import.meta.dirname, "..");
const DATA_DIR = path.join(ROOT, "data");
const CATALOG_FILE = path.join(DATA_DIR, "k73-3ds-catalog.json");
const REPORT_FILE = path.join(DATA_DIR, "k73-cover-report.json");
const APPLY = process.argv.includes("--apply");
const REFRESH = process.argv.includes("--refresh");

const sleep = (milliseconds) =>
    new Promise((resolve) => setTimeout(resolve, milliseconds));

function decodeHtml(value) {
    return value
        .replace(/<[^>]+>/g, "")
        .replace(/&nbsp;/g, " ")
        .replace(/&amp;/g, "&")
        .replace(/&quot;/g, "\"")
        .replace(/&#39;|&apos;/g, "'")
        .replace(/&#(\d+);/g, (_, code) =>
            String.fromCodePoint(Number(code)))
        .trim();
}

function absoluteK73Url(value) {
    if (value.startsWith("//")) return `http:${value}`;
    if (value.startsWith("/")) return `http://www.k73.com${value}`;
    return value;
}

function parseCatalogPage(html) {
    const entries = [];
    for (const match of html.matchAll(
        /<li\s+class="itemli">([\s\S]*?)<\/li>/g)) {
        const item = match[1];
        const page = item.match(
            /href="(http:\/\/www\.k73\.com\/down\/3ds\/\d+\.html)"/);
        const image = item.match(/<img\s+src="([^"]+)"/);
        const title = item.match(/<strong>([\s\S]*?)<\/strong>/);
        const description = item.match(
            /<span\s+class="daodu">([\s\S]*?)<\/span>/);
        if (!page || !image || !title) continue;
        entries.push({
            title: decodeHtml(title[1]),
            description: description ? decodeHtml(description[1]) : "",
            page_url: page[1],
            image_url: absoluteK73Url(image[1]),
        });
    }
    return entries;
}

async function fetchText(url) {
    const response = await fetch(url, {
        headers: {"User-Agent": "3DS-eShop-Cover-Matcher/1.0"},
        redirect: "follow",
    });
    if (!response.ok) {
        throw new Error(`${response.status} ${response.statusText}: ${url}`);
    }
    return response.text();
}

async function loadCatalog() {
    if (!REFRESH) {
        try {
            return JSON.parse(await fs.readFile(CATALOG_FILE, "utf8"));
        } catch {
            // No cache yet.
        }
    }

    const byPage = new Map();
    for (let page = 1; page <= 36; ++page) {
        const url = `${K73_LIST}81-${page}.html`;
        const entries = parseCatalogPage(await fetchText(url));
        for (const entry of entries) byPage.set(entry.page_url, entry);
        process.stdout.write(
            `K73 catalog ${page}/36 (${byPage.size} unique entries)\r`);
        await sleep(250);
    }
    process.stdout.write("\n");
    const catalog = [...byPage.values()];
    await fs.mkdir(DATA_DIR, {recursive: true});
    await fs.writeFile(CATALOG_FILE, `${JSON.stringify(catalog, null, 2)}\n`);
    return catalog;
}

function withoutAnnotations(value) {
    return value.replace(
        /[\(\（\[\【][^\)\）\]\】]*[\)\）\]\】]/g, " ");
}

function normalize(value) {
    return value
        .normalize("NFKC")
        .toLowerCase()
        .replace(
            /(完美|完全|最终|正式|修正|破解|官方|官译|简体|繁体|中文|汉化|日版|美版|欧版|韩版|中文版|汉化版|下载版|下载|补丁|升级|更新|本体|游戏版|无需跨区|需字库工具|cia|rom)/g,
            "")
        .replace(/v(?:er)?\.?\d+(?:\.\d+)*/g, "")
        .replace(/[^\p{L}\p{N}]+/gu, "");
}

function variants(value) {
    const values = new Set([normalize(value), normalize(withoutAnnotations(value))]);
    for (const match of value.matchAll(
        /[\(\（\[\【]([^\)\）\]\】]+)[\)\）\]\】]/g)) {
        values.add(normalize(match[1]));
    }
    return [...values].filter((item) => item.length >= 3);
}

function diceCoefficient(left, right) {
    if (left === right) return 1;
    if (left.length < 2 || right.length < 2) return 0;
    const pairs = new Map();
    for (let index = 0; index + 1 < left.length; ++index) {
        const pair = left.slice(index, index + 2);
        pairs.set(pair, (pairs.get(pair) ?? 0) + 1);
    }
    let overlap = 0;
    for (let index = 0; index + 1 < right.length; ++index) {
        const pair = right.slice(index, index + 2);
        const remaining = pairs.get(pair) ?? 0;
        if (remaining > 0) {
            overlap += 1;
            pairs.set(pair, remaining - 1);
        }
    }
    return (2 * overlap) / (left.length + right.length - 2);
}

function titleScore(localTitle, remoteTitle) {
    let best = 0;
    for (const left of variants(localTitle)) {
        for (const right of variants(remoteTitle)) {
            if (left === right) return 1;
            const shorter = Math.min(left.length, right.length);
            const longer = Math.max(left.length, right.length);
            if (shorter >= 4 && (left.includes(right) || right.includes(left))) {
                best = Math.max(best, 0.86 + 0.12 * (shorter / longer));
            }
            best = Math.max(best, diceCoefficient(left, right));
        }
    }
    return best;
}

function buildMatches(games, catalog) {
    const matches = [];
    const unmatched = [];
    for (const game of games) {
        if (game.has_cover) continue;
        const localVariants = variants(game.title);
        if (!localVariants.some((value) => value.length >= 4)) {
            unmatched.push({id: game.id, title: game.title, reason: "generic title"});
            continue;
        }
        const ranked = catalog
            .map((entry) => ({
                entry,
                score: titleScore(game.title, entry.title),
            }))
            .sort((a, b) => b.score - a.score);
        const best = ranked[0];
        const second = ranked[1];
        const exact = best?.score === 1;
        const ambiguous = !exact && second &&
            best.score - second.score < 0.025 &&
            best.entry.title !== second.entry.title;
        if (!best || best.score < 0.80 || ambiguous) {
            unmatched.push({
                id: game.id,
                title: game.title,
                reason: ambiguous ? "ambiguous" : "low score",
                candidate: best?.entry.title,
                score: best ? Number(best.score.toFixed(3)) : 0,
            });
            continue;
        }
        matches.push({
            id: game.id,
            title: game.title,
            k73_title: best.entry.title,
            score: Number(best.score.toFixed(3)),
            image_url: best.entry.image_url,
            page_url: best.entry.page_url,
        });
    }
    return {matches, unmatched};
}

async function applyMatches(matches) {
    let updated = 0;
    const errors = [];
    for (const [index, match] of matches.entries()) {
        try {
            const response = await fetch(
                `${NAS}/api/games/${match.id}/cover`,
                {
                    method: "POST",
                    headers: {"Content-Type": "application/json"},
                    body: JSON.stringify({url: match.image_url}),
                });
            const result = await response.json();
            if (!response.ok || !result.ok) {
                throw new Error(result.error ?? `HTTP ${response.status}`);
            }
            updated += 1;
        } catch (error) {
            errors.push({
                id: match.id,
                title: match.title,
                error: String(error.message ?? error),
            });
        }
        process.stdout.write(
            `NAS covers ${index + 1}/${matches.length} (${updated} updated)\r`);
        await sleep(200);
    }
    process.stdout.write("\n");
    return {updated, errors};
}

const catalog = await loadCatalog();
const gamesResponse = await fetch(`${NAS}/api/games`).then((response) =>
    response.json());
const games = gamesResponse.games ?? gamesResponse;
const report = buildMatches(games, catalog);
let result = {updated: 0, errors: []};

console.log(
    `Matched ${report.matches.length}, unmatched ${report.unmatched.length}, ` +
    `already covered ${games.filter((game) => game.has_cover).length}.`);
if (APPLY) result = await applyMatches(report.matches);

await fs.mkdir(DATA_DIR, {recursive: true});
await fs.writeFile(REPORT_FILE, `${JSON.stringify({
    generated_at: new Date().toISOString(),
    source: "http://www.k73.com/down/3ds/list-81-1.html",
    catalog_entries: catalog.length,
    apply: APPLY,
    ...result,
    ...report,
}, null, 2)}\n`);

console.log(
    APPLY
        ? `Finished: ${result.updated} covers updated, ${result.errors.length} errors.`
        : "Dry run only. Re-run with --apply after reviewing the report.");
