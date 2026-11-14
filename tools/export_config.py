#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Export a portable runner_config.json from the unpacked PC game tree.

The C++ runner must not hard-code this game's paths, resolution, entry
script, fonts, character offsets or BGM names.  This tool extracts all of
those from the game's own configuration tables.

Usage:
    .\\.venv\\Scripts\\python.exe cpp_runner\\tools\\export_config.py
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from runner.assets import AssetIndex, parse_ini
from runner.luatable import LuaTable, load_file


DEFAULT_ROOT = PROJECT_ROOT / "build_temp" / "pc_unpacked"
DEFAULT_OUT = PROJECT_ROOT / "cpp_runner" / "runner_config.json"


def _join(base: str, relative: str) -> str:
    return (base.rstrip("/") + "/" + relative.lstrip("/")).replace("//", "/")


def _read_text(path: Path) -> str:
    data = path.read_bytes()
    for encoding in ("utf-8-sig", "utf-8", "cp932"):
        try:
            return data.decode(encoding)
        except UnicodeDecodeError:
            continue
    return data.decode("latin-1")


def _fgpos_to_json(table: LuaTable) -> dict[str, dict[str, list[int]]]:
    result: dict[str, dict[str, list[int]]] = {}
    for head, entries in table.named.items():
        if not isinstance(entries, LuaTable):
            continue
        faces: dict[str, list[int]] = {}
        for key, value in entries.named.items():
            if isinstance(value, LuaTable):
                faces[key] = [int(value.get("x", 0)), int(value.get("y", 0))]
        for key, value in entries.numeric.items():
            if isinstance(value, LuaTable):
                faces[str(key)] = [int(value.get("x", 0)), int(value.get("y", 0))]
        result[head] = faces
    return result


def _extra_bgm_to_json(csv_table: LuaTable, index: AssetIndex,
                       sound_base: str, sound_ext: str) -> dict[str, Any]:
    extra = csv_table.get("extra_bgm")
    if not isinstance(extra, LuaTable):
        return {}
    result: dict[str, Any] = {}
    for name, entry in extra.named.items():
        if not isinstance(entry, LuaTable):
            continue
        strings = [value for value in entry.array if isinstance(value, str)]
        if not strings:
            continue
        intro_name = strings[0]
        intro_path = _join(sound_base, f"bgm/{intro_name}{sound_ext}")
        loop_name = None
        if intro_name.endswith("_a"):
            candidate = intro_name[:-2] + "_b"
            candidate_path = _join(sound_base, f"bgm/{candidate}{sound_ext}")
            if index.exists_exact(candidate_path):
                loop_name = candidate + sound_ext
        item: dict[str, Any] = {"intro": intro_name + sound_ext}
        if loop_name:
            item["loop"] = loop_name
        item["intro_exists"] = index.exists_exact(intro_path)
        result[name] = item
    return result


def build_config(root: Path) -> dict[str, Any]:
    index = AssetIndex.build(root)
    system_ini_path = index.resolve("system.ini", kind="system.ini")
    system_ini = parse_ini(_read_text(system_ini_path))
    windows = system_ini.get("WINDOWS", {})
    table_path = index.resolve("system/table/list_windows.tbl", kind="config")
    assignments = load_file(table_path)
    init = assignments.get("init")
    csv = assignments.get("csv")
    fgpos = assignments.get("fgpos")
    if not all(isinstance(value, LuaTable) for value in (init, csv, fgpos)):
        raise ValueError("list_windows.tbl is missing init/csv/fgpos")

    system = init.get("system")
    mpath = init.get("mpath")
    lang = init.get("lang")
    if not all(isinstance(value, LuaTable) for value in (system, mpath, lang)):
        raise ValueError("init.system/init.mpath/init.lang is missing")

    image_base = str(system.get("image_path", "image/xga/"))
    sound_base = str(system.get("sound_path", "sound/ogg/"))
    ui_base = str(system.get("ui_path", "pc/"))
    movie_base = str(system.get("movie_path", "movie/"))
    lang_dir = str(lang.get("ja", "uija/"))
    sound_ext = str(system.get("sound_ext", ".ogg"))
    fg_ext = str(system.get("fg_ext", ".png"))
    rule_ext = str(system.get("rule_ext", ".png"))
    script_base = "script/"

    width = int(windows.get("WIDTH", system.get("game_width", 1024)))
    height = int(windows.get("HEIGHT", system.get("game_height", 768)))

    image_magic = mpath.get("image")
    sound_magic = mpath.get("sound")
    if not isinstance(image_magic, LuaTable) or not isinstance(sound_magic, LuaTable):
        raise ValueError("init.mpath.image/sound is missing")

    path_mappings: dict[str, str] = {
        "script": script_base,
        "image": image_base,
        "sound": sound_base,
        "ui": _join(ui_base, lang_dir),
        "movie": movie_base,
        "font": "font/",
        ":ui/": _join(ui_base, lang_dir),
        ":script/": script_base,
    }
    for key, relative in image_magic.named.items():
        if isinstance(relative, str):
            path_mappings[f":{key}/"] = _join(image_base, relative + "/")
    for key, relative in sound_magic.named.items():
        if isinstance(relative, str):
            path_mappings[f":{key}/"] = _join(sound_base, relative + "/")

    title = init.get("game_title") or windows.get("TITLE") or "Artemis Game"
    font = init.get("font01")
    if not isinstance(font, str):
        font = "font/GenJyuuGothic-Monospace-Bold.ttf"

    # The textbox image is named by the game's own csv.mw table.
    ui_textbox = None
    mw = csv.get("mw")
    if isinstance(mw, LuaTable):
        bg01 = mw.get("bg01")
        if isinstance(bg01, LuaTable):
            file_name = bg01.get("file")
            if isinstance(file_name, str):
                ui_textbox = _join(path_mappings[":ui/"], file_name + ".png")

    font_layout = csv.get("font")
    font_layout_json = font_layout.to_plain() if isinstance(font_layout, LuaTable) else {}

    entry = "gamestart.ast"
    if not index.exists_exact(script_base + entry):
        raise ValueError(f"entry script not found: {script_base + entry}")

    scripts = sorted(
        path.relative_to(root).as_posix()
        for path in (root / "script").glob("*.ast")
    )

    config: dict[str, Any] = {
        "schema_version": 1,
        "window_title": str(title),
        "logical_width": width,
        "logical_height": height,
        "entry_script": entry,
        "script_base": script_base,
        "default_font": font,
        "fgpos_table": _fgpos_to_json(fgpos),
        "extra_bgm": _extra_bgm_to_json(csv, index, sound_base, sound_ext),
        "path_mappings": dict(sorted(path_mappings.items())),
        "font_layout": font_layout_json,
        "ui": {
            "textbox": ui_textbox,
            "language_dir": lang_dir,
        },
        "text": {
            "encoding": str(windows.get("CHARSET", "UTF-8")),
            "language": "ja",
            "name_font": "font01",
            "body_font": "font01",
            "name_layout": "name01",
            "body_layout": "adv01",
        },
        "audio": {
            "sound_ext": sound_ext,
            "voice_channels": [0],
            "se_channels": [1, 2, 3, 4, 5, 6, 7],
            "bgm_intro_suffix": "_a",
            "bgm_loop_suffix": "_b",
        },
        "extensions": {
            "image": ".png",
            "fg": fg_ext,
            "rule": rule_ext,
            "movie": str(system.get("movie_ext", ".wmv")),
        },
        "color_tones": {
            key: value
            for key, value in init.named.items()
            if isinstance(value, str)
            and (key == "sepia" or key.startswith("zone_"))
        },
        "game": {
            "title": str(title),
            "author": str(init.get("game_author", "")),
            "version_windows": str(init.get("game_ver_windows", "")),
        },
        "scripts": scripts,
    }
    return config


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError, ValueError):
            pass
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(DEFAULT_ROOT))
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    config = build_config(root)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8")

    print("=== runner_config export ===")
    print(f"root        : {root}")
    print(f"output      : {out}")
    print(f"title       : {config['window_title']}")
    print(f"resolution  : {config['logical_width']}x{config['logical_height']}")
    print(f"entry       : {config['entry_script']}")
    print(f"font        : {config['default_font']}")
    print(f"fgpos heads : {len(config['fgpos_table'])}")
    print(f"bgm entries : {len(config['extra_bgm'])}")
    print(f"mappings    : {len(config['path_mappings'])}")
    print(f"scripts     : {len(config['scripts'])}")
    print(f"ui textbox  : {config['ui']['textbox']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
