"""カタログ（SQLite）とサイドカー（.rdev）。

サイドカーが「正」、カタログは検索・絞り込み用のキャッシュ。
- 現像パラメータ・★・ラベル・フラグ：<元ファイル名>.rdev（JSON）
- カタログ：%APPDATA%/RawDeveloper/catalog.db
"""
from __future__ import annotations

import json
import os
import sqlite3
import threading
import time

from ..engine.params import merged


def app_dir() -> str:
    base = os.environ.get("APPDATA") or os.path.expanduser("~/.config")
    d = os.path.join(base, "RawDeveloper")
    os.makedirs(d, exist_ok=True)
    return d


def sidecar_path(path: str) -> str:
    return path + ".rdev"


def load_sidecar(path: str) -> dict | None:
    sp = sidecar_path(path)
    if not os.path.exists(sp):
        return None
    try:
        with open(sp, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def save_sidecar(path: str, data: dict):
    sp = sidecar_path(path)
    tmp = sp + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    os.replace(tmp, sp)


class Catalog:
    def __init__(self, path: str | None = None):
        self.path = path or os.path.join(app_dir(), "catalog.db")
        self._lock = threading.Lock()
        self.db = sqlite3.connect(self.path, check_same_thread=False)
        self.db.execute("""CREATE TABLE IF NOT EXISTS photos(
            path TEXT PRIMARY KEY, rating INTEGER DEFAULT 0, label TEXT, flag INTEGER DEFAULT 0,
            edited INTEGER DEFAULT 0, mtime REAL, added REAL)""")
        self.db.execute("""CREATE TABLE IF NOT EXISTS links(
            src TEXT, dst TEXT, created REAL, PRIMARY KEY(src, dst))""")
        self.db.execute("CREATE INDEX IF NOT EXISTS links_dst ON links(dst)")
        self.db.commit()

    # ---- photos
    def get(self, path: str):
        with self._lock:
            r = self.db.execute("SELECT rating,label,flag,edited FROM photos WHERE path=?", (path,)).fetchone()
        if r is None:
            return None
        return {"rating": r[0] or 0, "label": r[1], "flag": r[2] or 0, "edited": bool(r[3])}

    def upsert(self, path: str, rating=None, label="__keep__", flag=None, edited=None):
        cur = self.get(path) or {"rating": 0, "label": None, "flag": 0, "edited": False}
        if rating is not None:
            cur["rating"] = rating
        if label != "__keep__":
            cur["label"] = label
        if flag is not None:
            cur["flag"] = flag
        if edited is not None:
            cur["edited"] = edited
        with self._lock:
            self.db.execute(
                "INSERT INTO photos(path,rating,label,flag,edited,mtime,added) VALUES(?,?,?,?,?,?,?) "
                "ON CONFLICT(path) DO UPDATE SET rating=excluded.rating,label=excluded.label,"
                "flag=excluded.flag,edited=excluded.edited,mtime=excluded.mtime",
                (path, cur["rating"], cur["label"], cur["flag"], int(cur["edited"]), time.time(), time.time()))
            self.db.commit()
        return cur

    # ---- links（元 RAW → 書き出しファイル）
    def add_link(self, src: str, dst: str):
        with self._lock:
            self.db.execute("INSERT OR REPLACE INTO links(src,dst,created) VALUES(?,?,?)", (src, dst, time.time()))
            self.db.commit()

    def exports_of(self, src: str) -> list[str]:
        with self._lock:
            return [r[0] for r in self.db.execute("SELECT dst FROM links WHERE src=? ORDER BY created", (src,))]

    def source_of(self, dst: str) -> str | None:
        with self._lock:
            r = self.db.execute("SELECT src FROM links WHERE dst=?", (dst,)).fetchone()
        return r[0] if r else None

    def relink(self, old_dst: str, new_dst: str):
        with self._lock:
            self.db.execute("UPDATE links SET dst=? WHERE dst=?", (new_dst, old_dst))
            self.db.commit()

    def remove_link(self, src: str, dst: str):
        with self._lock:
            self.db.execute("DELETE FROM links WHERE src=? AND dst=?", (src, dst))
            self.db.commit()


class PhotoState:
    """1 枚の写真の状態（パラメータ＋評価）。"""

    def __init__(self, path: str):
        self.path = path
        self.params = None       # dict（未編集なら None）
        self.rating = 0
        self.label = None
        self.flag = 0            # 1=採用, -1=除外, 0=なし
        self.history = []        # [(説明, params)]
        self.history_pos = -1
        self.snapshots = []      # [(名前, params)]

    def to_json(self) -> dict:
        d = {"version": 1, "rating": self.rating, "label": self.label,
             "flag": {1: "pick", -1: "reject"}.get(self.flag)}
        if self.params is not None:
            d["params"] = self.params
        if self.snapshots:
            d["snapshots"] = [{"name": n, "params": p} for n, p in self.snapshots]
        return d

    @classmethod
    def from_sidecar(cls, path: str) -> "PhotoState":
        st = cls(path)
        d = load_sidecar(path)
        if d:
            st.rating = int(d.get("rating") or 0)
            st.label = d.get("label")
            st.flag = {"pick": 1, "reject": -1}.get(d.get("flag"), 0)
            if d.get("params"):
                st.params = merged(d["params"])
            for s in d.get("snapshots", []):
                st.snapshots.append((s.get("name", "スナップショット"), merged(s.get("params"))))
        return st
