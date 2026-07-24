#!/usr/bin/env python3
"""
3DS NAS eShop server — 扫描配置目录下的 .cia/.3ds/.3dsx 文件，
提供 REST API + 简单的 HTML 商城页面（3DS 浏览器可访问）。
"""
import os
import json
import hashlib
import sqlite3
from pathlib import Path
from flask import Flask, jsonify, request, send_from_directory, send_file, render_template_string, Response

# Flask 不带 static_folder（避免和我们的 /static 路由冲突）
app = Flask(__name__, static_folder=None)

# 所有路径都可由环境变量覆盖。默认值适合直接在源码目录试运行；
# 群晖部署示例请见 docs/SYNOLOGY.md。
SERVER_DIR = Path(__file__).resolve().parent
DATA_DIR = Path(os.environ.get("ESHOP_DATA_DIR", str(SERVER_DIR / "data")))
# 游戏库根目录
GAMES_DIR = Path(os.environ.get(
    "ESHOP_GAMES_DIR", str(DATA_DIR / "games")))
# 数据库存元数据（TitleID / 名字 / 封面路径）
DB_PATH = Path(os.environ.get(
    "ESHOP_DB_PATH", str(DATA_DIR / "db" / "games.db")))
# 封面缓存目录
COVERS_DIR = Path(os.environ.get(
    "ESHOP_COVERS_DIR", str(DATA_DIR / "covers")))
COVERS_DIR.mkdir(parents=True, exist_ok=True)

# 允许的扩展名
ALLOWED_EXTS = {".cia", ".3ds", ".3dsx"}

PORT = int(os.environ.get("ESHOP_PORT", "40441"))


def get_db():
    """获取 SQLite 连接，自动建表"""
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("""
        CREATE TABLE IF NOT EXISTS games (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            filename TEXT NOT NULL UNIQUE,
            filepath TEXT NOT NULL,
            size_bytes INTEGER NOT NULL,
            extension TEXT NOT NULL,
            title_id TEXT,
            serial TEXT,
            cover_path TEXT,
            description TEXT,
            region TEXT,
            category TEXT,
            added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            sha256 TEXT
        )
    """)
    conn.execute("CREATE INDEX IF NOT EXISTS idx_title ON games(title)")
    return conn


def scan_games():
    """扫描 GAMES_DIR，更新数据库"""
    if not GAMES_DIR.exists():
        return 0
    conn = get_db()
    added = 0
    import re
    # 匹配纯 16 位 TitleID（不要求在中括号里）
    tid_pat = re.compile(r"([0-9A-Fa-f]{16})")
    for fp in GAMES_DIR.rglob("*"):
        if not fp.is_file() or fp.suffix.lower() not in ALLOWED_EXTS:
            continue
        if any(p.startswith(".") for p in fp.parts):
            continue
        rel = str(fp.relative_to(GAMES_DIR))
        cur = conn.execute("SELECT id FROM games WHERE filename = ?", (rel,))
        if cur.fetchone():
            continue

        # TitleID 识别策略（按优先级）：
        # 1. 中括号 [0004000000053F00]
        # 2. 文件名/父目录名里 16 位 hex
        title_id = None
        m = re.search(r"\[([0-9A-Fa-f]{16})\]", fp.name)
        if m:
            title_id = m.group(1).upper()
        else:
            # 文件名去掉扩展名后查找
            stem = fp.stem
            m = tid_pat.search(stem)
            if m:
                title_id = m.group(1).upper()
            else:
                # 父目录名
                if fp.parent.name and fp.parent.name != GAMES_DIR.name:
                    m = tid_pat.search(fp.parent.name)
                    if m:
                        title_id = m.group(1).upper()

        # 标题策略：
        # 1. 优先用**父目录名**（最干净：rom/马里奥赛车7/...  → "马里奥赛车7"）
        # 2. 否则去括号后缀的 stem
        title = None
        if fp.parent.name and fp.parent.name != GAMES_DIR.name:
            # 父目录名去掉 [v1.0] / (USA) 之类的尾缀
            t = fp.parent.name
            t = re.sub(r"\s*\[[^\]]*\]\s*", " ", t)  # 去 [...] 标签
            t = re.sub(r"\s*\([A-Za-z0-9_.,\s-]+\)\s*", " ", t)  # 去 (USA) (eShop) 之类
            t = re.sub(r"\s+", " ", t).strip()
            if t and not tid_pat.fullmatch(t):  # 别拿纯 TitleID 当标题
                title = t

        if not title:
            title = fp.stem
            title = re.sub(r"\s*\[[0-9A-Fa-f]{16}\]\s*$", "", title)
            title = re.sub(r"\s*\([A-Za-z0-9]+\)\s*$", "", title)
            # 纯 TitleID 的文件名？放弃
            if tid_pat.fullmatch(title):
                title = fp.stem  # 保留原样

        conn.execute("""
            INSERT OR IGNORE INTO games
            (title, filename, filepath, size_bytes, extension, title_id)
            VALUES (?, ?, ?, ?, ?, ?)
        """, (title, rel, str(fp), fp.stat().st_size, fp.suffix.lower(), title_id))
        added += 1
    conn.commit()
    conn.close()
    return added


# === API 路由 ===

@app.route("/")
def index():
    """HTML 商城首页（3DS 浏览器可用）"""
    return render_template_string(HTML_INDEX)


@app.route("/api/scan", methods=["POST", "GET"])
def api_scan():
    """手动触发扫描（GET / POST 都支持）"""
    n = scan_games()
    return jsonify({"ok": True, "added": n})


@app.route("/api/debug/log", methods=["POST", "GET"])
def api_debug_log():
    """3DS 端 debug 上报：写入 /tmp/3ds_debug.log"""
    import os
    from datetime import datetime
    msg = request.args.get("msg", "")
    if request.method == "POST" and request.data:
        try:
            payload = request.get_data(as_text=True)
            if payload:
                msg = payload[:500]
        except:
            pass
    with open("/tmp/3ds_debug.log", "a") as f:
        f.write(f"[{datetime.now().isoformat()}] {msg}\n")
    return jsonify({"ok": True})


@app.route("/api/rescan", methods=["POST", "GET"])
def api_rescan():
    """全量重新扫描：先清空旧数据再扫"""
    conn = get_db()
    conn.execute("DELETE FROM games")
    conn.commit()
    conn.close()
    n = scan_games()
    return jsonify({"ok": True, "added": n, "total": n})


@app.route("/api/games/<int:game_id>/rename", methods=["POST", "GET"])
def api_rename(game_id):
    """手动改游戏名（不改文件）"""
    if request.method == "GET":
        return jsonify({"ok": False, "error": "use POST with JSON body"}), 405
    data = request.get_json() or {}
    new_title = data.get("title", "").strip()
    if not new_title:
        return jsonify({"ok": False, "error": "title required"}), 400
    conn = get_db()
    conn.execute("UPDATE games SET title = ? WHERE id = ?", (new_title, game_id))
    if conn.total_changes == 0:
        conn.close()
        return jsonify({"ok": False, "error": "game not found"}), 404
    conn.commit()
    conn.close()
    return jsonify({"ok": True, "id": game_id, "title": new_title})


@app.route("/api/games/<int:game_id>", methods=["DELETE"])
def api_delete(game_id):
    """从数据库删除（不删文件）"""
    conn = get_db()
    conn.execute("DELETE FROM games WHERE id = ?", (game_id,))
    conn.commit()
    conn.close()
    return jsonify({"ok": True})


@app.route("/api/games/<int:game_id>/cover", methods=["POST", "GET"])
def api_set_cover(game_id):
    """设置封面（两种方式：上传文件 / 指定 URL）"""
    conn = get_db()
    row = conn.execute("SELECT id, title, title_id FROM games WHERE id=?", (game_id,)).fetchone()
    if not row:
        conn.close()
        return jsonify({"ok": False, "error": "game not found"}), 404

    cover_name = None

    # 方式 1：上传文件
    if 'file' in request.files:
        f = request.files['file']
        if f.filename:
            # 用 title_id 当文件名，找不到就用 id
            ext = Path(f.filename).suffix.lower() or ".png"
            if ext not in {".png", ".jpg", ".jpeg", ".bmp", ".webp"}:
                conn.close()
                return jsonify({"ok": False, "error": f"unsupported ext: {ext}"}), 400
            base = (row["title_id"] or f"id{row['id']}").lower()
            cover_name = f"{base}{ext}"
            f.save(str(COVERS_DIR / cover_name))

    # 方式 2：指定 URL
    elif request.is_json:
        data = request.get_json() or {}
        url = data.get("url", "").strip()
        if not url:
            conn.close()
            return jsonify({"ok": False, "error": "url or file required"}), 400
        import urllib.request
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "3DS-eShop/1.0"})
            with urllib.request.urlopen(req, timeout=30) as resp:
                if resp.status != 200:
                    conn.close()
                    return jsonify({"ok": False, "error": f"url returned {resp.status}"}), 400
                ct = resp.headers.get("Content-Type", "")
                data_bytes = resp.read()
            # 从 URL 推后缀
            from urllib.parse import urlparse
            path = urlparse(url).path
            ext = Path(path).suffix.lower() or ".png"
            if ext not in {".png", ".jpg", ".jpeg", ".bmp", ".webp"}:
                ext = ".png"
            base = (row["title_id"] or f"id{row['id']}").lower()
            cover_name = f"{base}{ext}"
            (COVERS_DIR / cover_name).write_bytes(data_bytes)
        except Exception as e:
            conn.close()
            return jsonify({"ok": False, "error": f"download failed: {e}"}), 500
    else:
        conn.close()
        return jsonify({"ok": False, "error": "no file or url provided"}), 400

    conn.execute("UPDATE games SET cover_path=? WHERE id=?", (cover_name, game_id))
    conn.commit()
    conn.close()
    return jsonify({"ok": True, "id": game_id, "cover_path": cover_name,
                    "cover_url": f"/covers/{cover_name}"})


@app.route("/api/games/<int:game_id>/scrape", methods=["POST", "GET"])
def api_scrape_one(game_id):
    """单条游戏尝试自动刮削（多源 fallback）"""
    conn = get_db()
    conn.row_factory = sqlite3.Row
    r = conn.execute("SELECT id, title, title_id FROM games WHERE id=?", (game_id,)).fetchone()
    if not r:
        conn.close()
        return jsonify({"ok": False, "error": "not found"}), 404

    if not r["title_id"]:
        conn.close()
        return jsonify({"ok": False, "error": "no title_id"}), 400

    import urllib.request
    tid = r["title_id"].upper()

    # 候选 URL 列表（依次尝试）
    candidates = [
        f"https://art.gametdb.com/3ds/cover/EN/{tid}.png",  # GameTDB EN
        f"https://art.gametdb.com/3ds/cover/JA/{tid}.png",  # GameTDB JA
        f"https://art.gametdb.com/3ds/cover/US/{tid}.png",  # GameTDB US
        f"https://art.gametdb.com/3ds/cover/EU/{tid}.png",  # GameTDB EU
    ]

    for url in candidates:
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "3DS-eShop/1.0"})
            with urllib.request.urlopen(req, timeout=10) as resp:
                if resp.status == 200:
                    data = resp.read()
                    if len(data) > 100:  # 不是空/错误页
                        cover_name = f"{tid.lower()}.png"
                        (COVERS_DIR / cover_name).write_bytes(data)
                        conn.execute("UPDATE games SET cover_path=? WHERE id=?",
                                     (cover_name, game_id))
                        conn.commit()
                        conn.close()
                        return jsonify({"ok": True, "source": url, "cover_path": cover_name})
        except Exception:
            continue

    conn.close()
    return jsonify({"ok": False, "error": "no source had the cover"}), 404


@app.route("/api/games")
def api_games():
    """获取游戏列表"""
    conn = get_db()
    conn.row_factory = sqlite3.Row
    rows = conn.execute("""
        SELECT id, title, filename, size_bytes, extension,
               title_id, cover_path, description, region
        FROM games
        ORDER BY title
    """).fetchall()
    games = []
    for r in rows:
        games.append({
            "id": r["id"],
            "title": r["title"],
            "filename": r["filename"],
            "size_mb": round(r["size_bytes"] / 1024 / 1024, 1),
            "ext": r["extension"],
            "title_id": r["title_id"],
            "has_cover": bool(r["cover_path"] and (COVERS_DIR / r["cover_path"]).exists()),
            "cover_url": f"/covers/{r['cover_path']}" if r["cover_path"] else None,
            "description": r["description"] or "",
            "region": r["region"] or "",
            "download_url": f"/download/{r['id']}",
        })
    conn.close()
    # 3DS 客户端 httpc 对大 body 处理有 bug：分批传但终止条件不可靠
    # 返回一个轻量版本（无 description 字段）保证能完整收下
    slim = [{
        "id": g["id"], "title": g["title"], "filename": g["filename"],
        "size_mb": g["size_mb"], "ext": g["ext"], "title_id": g["title_id"],
        "has_cover": g["has_cover"], "cover_url": g["cover_url"],
        "download_url": g["download_url"], "description": "", "region": g["region"]
    } for g in games]
    resp = jsonify({"ok": True, "count": len(slim), "games": slim})
    # 显式发 Content-Length，让 3DS 知道该收多少
    data = resp.get_data()
    return Response(data, status=200, mimetype="application/json",
                    headers={"Content-Length": str(len(data)),
                             "Connection": "close"})


@app.route("/api/games/<int:game_id>")
def api_game_detail(game_id):
    """单个游戏详情"""
    conn = get_db()
    conn.row_factory = sqlite3.Row
    r = conn.execute("SELECT * FROM games WHERE id = ?", (game_id,)).fetchone()
    if not r:
        conn.close()
        return jsonify({"ok": False, "error": "not found"}), 404
    detail = dict(r)
    detail["size_mb"] = round(detail["size_bytes"] / 1024 / 1024, 1)
    detail["has_cover"] = bool(detail["cover_path"] and (COVERS_DIR / detail["cover_path"]).exists())
    detail["cover_url"] = f"/covers/{detail['cover_path']}" if detail["cover_path"] else None
    detail["download_url"] = f"/download/{game_id}"
    conn.close()
    return jsonify({"ok": True, "game": detail})


@app.route("/download/<int:game_id>")
def download(game_id):
    """下载 .cia 文件（FBI 可直接装这个 URL）"""
    conn = get_db()
    row = conn.execute("SELECT filepath, filename FROM games WHERE id = ?", (game_id,)).fetchone()
    conn.close()
    if not row:
        return jsonify({"ok": False, "error": "not found"}), 404
    fp = Path(row["filepath"])
    if not fp.exists():
        return jsonify({"ok": False, "error": "file missing on disk"}), 404
    from urllib.parse import quote
    quoted = quote(row["filename"])
    return send_file(str(fp), as_attachment=True, download_name=row["filename"])


@app.route("/static/<path:filename>")
def static_files(filename):
    """静态文件（3DS 客户端、homebrew 等）"""
    return send_from_directory(str(GAMES_DIR / "static"), filename)


@app.route("/covers/<path:cover_name>")
def covers(cover_name):
    """封面图"""
    return send_from_directory(str(COVERS_DIR), cover_name)


@app.route("/api/cover_bmp/<int:game_id>")
def api_cover_bmp(game_id):
    """返回 BMP 格式封面（3DS citro2d 直接用，不用 PNG 解码）"""
    width = int(request.args.get("w", 128))
    conn = get_db()
    row = conn.execute("SELECT cover_path, title FROM games WHERE id=?", (game_id,)).fetchone()
    conn.close()
    if not row or not row["cover_path"]:
        return jsonify({"ok": False, "error": "no cover"}), 404
    cover_path = COVERS_DIR / row["cover_path"]
    if not cover_path.exists():
        return jsonify({"ok": False, "error": "cover missing"}), 404
    try:
        from PIL import Image
        import io
        img = Image.open(cover_path).convert("RGB")
        w, h = img.size
        ratio = width / w
        new_h = int(h * ratio)
        img = img.resize((width, new_h), Image.LANCZOS)
        buf = io.BytesIO()
        img.save(buf, format="BMP")
        buf.seek(0)
        from flask import Response
        return Response(buf.getvalue(), mimetype="image/bmp",
                        headers={"Content-Disposition": f'inline; filename="cover_{game_id}.bmp"'})
    except ImportError:
        return jsonify({"ok": False, "error": "Pillow not installed"}), 500
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500


# === 3DS 浏览器友好的 HTML 商城 ===

HTML_INDEX = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=320">
<title>3DS eShop</title>
<style>
body { font-family: sans-serif; background: #1a1a2e; color: #eee; margin: 0; padding: 8px; font-size: 12px; }
h1 { color: #f39c12; font-size: 16px; text-align: center; margin: 4px 0; }
.game { background: #16213e; border: 1px solid #0f3460; border-radius: 4px;
        margin: 6px 0; padding: 6px; display: flex; gap: 6px; }
.cover { width: 60px; height: 80px; background: #0f3460; flex-shrink: 0;
         display: flex; align-items: center; justify-content: center;
         color: #555; font-size: 10px; text-align: center; overflow: hidden; }
.cover img { width: 100%; height: 100%; object-fit: cover; }
.info { flex: 1; min-width: 0; }
.title { font-weight: bold; color: #f39c12; font-size: 13px;
         white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.meta { color: #888; font-size: 10px; margin-top: 2px; }
.desc { color: #ccc; font-size: 11px; margin-top: 4px;
        display: -webkit-box; -webkit-line-clamp: 3; -webkit-box-orient: vertical;
        overflow: hidden; }
.btn { display: inline-block; background: #e94560; color: #fff; padding: 4px 8px;
       border-radius: 3px; text-decoration: none; margin-top: 4px; margin-right: 4px;
       font-size: 11px; border: none; cursor: pointer; }
.btn-sm { background: #0f3460; color: #f39c12; padding: 3px 6px; border-radius: 3px;
          margin-top: 4px; margin-right: 4px; font-size: 10px; border: 1px solid #f39c12;
          cursor: pointer; }
.loading { text-align: center; color: #888; padding: 20px; }
.scan { background: #0f3460; color: #f39c12; border: 1px solid #f39c12;
        padding: 6px; text-align: center; margin-bottom: 8px; border-radius: 4px; }
</style>
</head>
<body>
<h1>📦 3DS eShop</h1>
<div class="scan" onclick="fetch('/api/scan',{method:'POST'}).then(()=>loadGames())">
  🔄 扫描新游戏
</div>
<div id="list" class="loading">Loading...</div>
<script>
function loadGames() {
  document.getElementById('list').innerHTML = 'Loading...';
  fetch('/api/games').then(r=>r.json()).then(d=>{
    if (!d.ok || d.games.length === 0) {
      document.getElementById('list').innerHTML =
        '<div class="loading">暂无游戏。<br>把 .cia 放进服务端配置的游戏目录，然后点上面"扫描"</div>';
      return;
    }
    let html = '';
    d.games.forEach(g => {
      const cover = g.has_cover
        ? '<img src="' + g.cover_url + '">'
        : '🎮';
      html += '<div class="game">' +
        '<div class="cover">' + cover + '</div>' +
        '<div class="info">' +
          '<div class="title" id="title-' + g.id + '">' + g.title + '</div>' +
          '<div class="meta">' + g.size_mb + 'MB · ' + g.ext +
            (g.region ? ' · ' + g.region : '') +
            (g.title_id ? ' · ' + g.title_id : '') + '</div>' +
          (g.description ? '<div class="desc">' + g.description + '</div>' : '') +
          '<a class="btn" href="' + g.download_url + '">📥 下载 CIA</a>' +
          '<button class="btn-sm" onclick="renameGame(' + g.id + ')">✏️ 改名</button>' +
          '<button class="btn-sm" onclick="uploadCover(' + g.id + ')">🖼️ 传封面</button>' +
          (g.title_id ? '<button class="btn-sm" onclick="scrapeCover(' + g.id + ')">🔍 刮封面</button>' : '') +
        '</div></div>';
    });
    document.getElementById('list').innerHTML = html;
  }).catch(e=>{
    document.getElementById('list').innerHTML = '<div class="loading">加载失败: '+e+'</div>';
  });
}

function renameGame(id) {
  const cur = document.getElementById('title-' + id).innerText;
  const t = prompt('新名字:', cur);
  if (!t || t === cur) return;
  fetch('/api/games/' + id + '/rename', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({title: t})
  }).then(r=>r.json()).then(d=>{
    if (d.ok) loadGames();
    else alert('失败: ' + d.error);
  });
}

function uploadCover(id) {
  const url = prompt('封面图片 URL（http://...），或留空选文件:');
  if (url) {
    fetch('/api/games/' + id + '/cover', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({url: url})
    }).then(r=>r.json()).then(d=>{
      if (d.ok) loadGames();
      else alert('失败: ' + d.error);
    });
  } else {
    const inp = document.createElement('input');
    inp.type = 'file';
    inp.accept = 'image/*';
    inp.onchange = () => {
      const fd = new FormData();
      fd.append('file', inp.files[0]);
      fetch('/api/games/' + id + '/cover', {method: 'POST', body: fd})
        .then(r=>r.json()).then(d=>{
          if (d.ok) loadGames();
          else alert('失败: ' + d.error);
        });
    };
    inp.click();
  }
}

function scrapeCover(id) {
  fetch('/api/games/' + id + '/scrape', {method: 'POST'})
    .then(r=>r.json()).then(d=>{
      if (d.ok) { alert('找到: ' + d.source); loadGames(); }
      else alert('没找到: ' + d.error);
    });
}
loadGames();
</script>
</body>
</html>
"""


if __name__ == "__main__":
    get_db()  # 初始化表
    print(f"📦 3DS NAS eShop server")
    print(f"   games dir : {GAMES_DIR}")
    print(f"   db        : {DB_PATH}")
    print(f"   port      : {PORT}")
    print(f"   browse    : http://<NAS-IP>:{PORT}/")
    app.run(host="0.0.0.0", port=PORT, debug=False, threaded=True)
