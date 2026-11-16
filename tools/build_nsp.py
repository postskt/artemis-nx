#!/usr/bin/env python3
"""Stage an Artemis-NX Switch NSP build.

The tool always produces a complete installable layout:
  output/nsp_build/exefs/main
  output/nsp_build/exefs/main.npdm
  output/nsp_build/romfs/...
  output/nsp_build/control/control.nacp
  output/nsp_build/control/icon.jpg

If prod.keys and a supported packer are available, it invokes the packer;
otherwise it writes output/nsp_build/build_nsp_with_keys.bat.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DEVKITPRO = Path(os.environ.get("DEVKITPRO", r"C:\msys64\opt\devkitpro"))
TITLE_ID = "0100999012340000"
TITLE_NAME = "アイベヤ"
AUTHOR = "Artemis-NX"
VERSION = "1.0.0"


def run(cmd: list[str], *, check: bool = True) -> subprocess.CompletedProcess:
    print("+ " + " ".join(str(x) for x in cmd))
    return subprocess.run([str(x) for x in cmd], check=check)


def locate_tool(name: str) -> Path | None:
    candidates = [
        ROOT / "tools" / "bin" / name,
        ROOT / "tools" / "bin" / "ffmpeg" / name,
        ROOT / "build_temp" / "tools" / name,
        ROOT / "build_temp" / "hacbrew" / "hacBrewPack-master" / name,
        ROOT / "output" / "nsp_build" / name,
        DEFAULT_DEVKITPRO / "tools" / "bin" / name,
        Path(name),
    ]
    found = shutil.which(name)
    if found:
        candidates.append(Path(found))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def find_keys(explicit: str | None) -> Path | None:
    if explicit:
        path = Path(explicit)
        return path if path.is_file() else None
    candidates = [
        ROOT / "prod.keys",
        ROOT / "output" / "nsp_build" / "prod.keys",
        ROOT / "build_temp" / "prod.keys",
        Path.home() / ".switch" / "prod.keys",
        Path.home() / "AppData" / "Roaming" / "Ryujinx" / "system" / "prod.keys",
        Path.home() / "AppData" / "Roaming" / "yuzu" / "keys" / "prod.keys",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def find_packer(explicit: str | None) -> Path | None:
    if explicit:
        path = Path(explicit)
        return path if path.is_file() else None
    for name in ("hacbrewpack.exe", "hacbrewpack", "hacpack.exe", "hacpack"):
        tool = locate_tool(name)
        if tool:
            return tool
    return None


def make_icon(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        from PIL import Image

        image = Image.open(source).convert("RGBA")
        side = min(image.width, image.height)
        image = image.crop((0, 0, side, side)).resize((256, 256), Image.Resampling.LANCZOS)
        background = Image.new("RGB", image.size, (255, 255, 255))
        background.paste(image, mask=image.getchannel("A"))
        # The source eyecatch is intentionally low-contrast artwork.  Horizon
        # renders the home-menu icon small, where it reads as washed-out gray;
        # restore a little colour/contrast for the 256px icon only.
        from PIL import ImageEnhance
        background = ImageEnhance.Color(background).enhance(1.35)
        background = ImageEnhance.Contrast(background).enhance(1.15)
        background.save(target, "JPEG", quality=95, progressive=False, optimize=True)
    except ImportError:
        ffmpeg = locate_tool("ffmpeg.exe") or locate_tool("ffmpeg")
        if not ffmpeg:
            raise RuntimeError("Pillow is unavailable and ffmpeg was not found")
        run([
            ffmpeg, "-y", "-i", source,
            "-vf", "crop=768:768:0:0,scale=256:256:flags=lanczos",
            "-frames:v", "1", "-q:v", "2", "-pix_fmt", "yuvj420p", target,
        ])
    print(f"icon: {target}")


def patch_nacp_titles(path: Path, title: str) -> None:
    """nacptool on this Windows host stores the Japanese name as EUC-JP.

    Horizon expects UTF-8 name fields, so normalize all 16 language slots
    (0x300-byte entries, 0x200-byte name + 0x100-byte publisher).
    """
    data = bytearray(path.read_bytes())
    encoded = title.encode("utf-8")[:0x1FF]
    for index in range(16):
        offset = index * 0x300
        if offset + 0x200 > len(data):
            break
        data[offset:offset + 0x200] = b"\x00" * 0x200
        data[offset:offset + len(encoded)] = encoded
    path.write_bytes(data)


def write_npdm_json(path: Path, title_id: str) -> None:
    npdm = {
        "name": "artemis_runner",
        "title_id": f"0x{title_id}",
        "title_id_range_min": "0x0100000000000000",
        "title_id_range_max": "0x01ffffffffffffff",
        "main_thread_stack_size": "0x00800000",
        "main_thread_priority": 44,
        "default_cpu_id": 0,
        "process_category": 0,
        "is_retail": True,
        "pool_partition": 0,
        "is_64_bit": True,
        "address_space_type": 3,
        "filesystem_access": {"permissions": "0xffffffffffffffff"},
        # NPDM service strings may contain the wildcard '*' (switchbrew NPDM:
        # "The service string can contain a wildcard* character"), and
        # npdmtool copies the string verbatim, so ["*"] grants every service.
        # This is valid on hardware; emulators ignore the ACI0 list anyway.
        "service_access": ["*"],
        "service_host": ["*"],
        "kernel_capabilities": [
            {
                "type": "kernel_flags",
                "value": {
                    "highest_thread_priority": 63,
                    "lowest_thread_priority": 24,
                    "lowest_cpu_id": 0,
                    "highest_cpu_id": 3,
                },
            },
            {
                # npdmtool emits an empty SVC whitelist when this capability
                # is omitted.  The result is that ordinary SVCs such as
                # svcGetSystemTick (0x1e) and svcReturnFromException (0x28)
                # raise 2168-0008 on real hardware.  Grant the same standard
                # application SVC set used by working NSPs.
                "type": "syscalls",
                "value": {
                    f"svc{i:02x}": f"0x{i:02x}"
                    for i in range(0x01, 0x80)
                },
            },
            {"type": "min_kernel_version", "value": "0x0030"},
            {"type": "handle_table_size", "value": 1023},
            {
                "type": "debug_flags",
                "value": {
                    "allow_debug": False,
                    "force_debug": False,
                    "force_debug_prod": False,
                },
            },
            {
                # npdmtool defaults application_type to 0 (sysmodule-like),
                # which gives the process a restricted SVC whitelist.  The
                # installed NSP must be launched as a normal application.
                "type": "application_type",
                "value": 1,
            },
        ],
    }
    path.write_text(json.dumps(npdm, ensure_ascii=False, indent=2), encoding="utf-8")


def make_batch(out: Path, title_id: str) -> None:
    batch = f"""@echo off
setlocal EnableExtensions
rem Place prod.keys next to this file, then double-click.
rem hacBrewPack cannot handle this repository's non-ASCII path, so the
rem packer runs through the short ASCII junction C:\\artc_nsp.
set "ROOT=%~dp0"
set "STAGE=C:\\artc_nsp"
set "KEYS=%ROOT%prod.keys"
if not exist "%KEYS%" (
  echo [ERROR] Put prod.keys in %ROOT% and run again.
  pause
  exit /b 1
)

set "HACBREW=%ROOT%hacbrewpack.exe"
if not exist "%HACBREW%" (
  echo [ERROR] hacbrewpack.exe was not found in %ROOT%.
  pause
  exit /b 1
)

if exist "%STAGE%\\" rmdir "%STAGE%"
mklink /J "%STAGE%" "%ROOT:~0,-1%" >nul
if errorlevel 1 (
  echo [ERROR] Could not create %STAGE% junction.
  pause
  exit /b 1
)
copy /Y "%HACBREW%" "%STAGE%\\hacbrewpack.exe" >nul
copy /Y "%KEYS%" "%STAGE%\\prod.keys" >nul

if exist "%STAGE%\\nca" rmdir /s /q "%STAGE%\\nca"
if exist "%STAGE%\\nsp" rmdir /s /q "%STAGE%\\nsp"
if exist "%STAGE%\\hacbrewpack_temp" rmdir /s /q "%STAGE%\\hacbrewpack_temp"

pushd "%STAGE%"
"%STAGE%\\hacbrewpack.exe" -k "%STAGE%\\prod.keys" ^
  --exefsdir "%STAGE%\\exefs" ^
  --romfsdir "%STAGE%\\romfs" ^
  --controldir "%STAGE%\\control" ^
  --ncadir "%STAGE%\\nca" ^
  --nspdir "%STAGE%\\nsp" ^
  --nologo
set "PACK_RESULT=%ERRORLEVEL%"
popd
if not "%PACK_RESULT%"=="0" (
  echo [ERROR] hacBrewPack failed.
  pause
  exit /b 1
)

for %%F in ("%STAGE%\\nsp\\*.nsp") do (
  move /Y "%%F" "%ROOT%..\\aibeya_switch.nsp" >nul
)
echo [OK] output\\aibeya_switch.nsp
pause
"""
    (out / "build_nsp_with_keys.bat").write_text(batch, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage an Artemis-NX NSP build")
    parser.add_argument("--source", default=str(ROOT / "build_temp" / "pc_unpacked"))
    parser.add_argument("--out", default=str(ROOT / "output" / "nsp_build"))
    parser.add_argument("--elf", default=str(ROOT / "cpp_runner" / "build-switch" / "src" / "platform" / "artemis_runner.elf"))
    parser.add_argument("--title-id", default=TITLE_ID)
    parser.add_argument("--title", default=TITLE_NAME)
    parser.add_argument("--author", default=AUTHOR)
    parser.add_argument("--version", default=VERSION)
    parser.add_argument("--keys")
    parser.add_argument("--packer")
    parser.add_argument("--icon-source")
    parser.add_argument("--with-chs", action="store_true")
    parser.add_argument("--chs-dir")
    parser.add_argument("--refresh-romfs", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()

    source = Path(args.source)
    out = Path(args.out)
    romfs = out / "romfs"
    exefs = out / "exefs"
    control = out / "control"
    out.mkdir(parents=True, exist_ok=True)
    exefs.mkdir(parents=True, exist_ok=True)
    control.mkdir(parents=True, exist_ok=True)

    if args.refresh_romfs or not (romfs / "runner_config.json").is_file():
        run([
            sys.executable,
            ROOT / "scripts" / "prepare_romfs.py",
            "--source", source,
            "--output", romfs,
            "--report", out / "romfs_report.json",
            "--clean",
        ])
    else:
        print(f"romfs: reuse {romfs}")

    if args.with_chs:
        chs_dir = Path(args.chs_dir) if args.chs_dir else None
        if chs_dir and chs_dir.is_dir():
            for path in chs_dir.rglob("*"):
                if path.is_file():
                    relative = path.relative_to(chs_dir)
                    target = romfs / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, target)
            print(f"chs overlay: {chs_dir}")
        else:
            cpk = source / "aibeya.cpk"
            if cpk.is_file():
                print("[warn] aibeya.cpk found but no CPK extractor is installed.")
                print("[warn] extract it to a directory and pass --chs-dir <dir>.")
            else:
                print("[warn] --with-chs requested but aibeya.cpk/chs overlay was not found")

    elf = Path(args.elf)
    if not elf.is_file():
        if args.skip_build:
            raise SystemExit(f"missing Switch ELF: {elf}")
        print("Switch ELF missing; invoking build_switch.sh")
        bash = Path(r"C:\msys64\usr\bin\bash.exe")
        run([bash, ROOT / "cpp_runner" / "tools" / "build_switch.sh"])
    if not elf.is_file():
        raise SystemExit(f"Switch ELF still missing: {elf}")

    elf2nso = locate_tool("elf2nso.exe")
    if not elf2nso:
        raise SystemExit("elf2nso.exe not found; install switch-dev")
    run([elf2nso, elf, exefs / "main"])

    npdmtool = locate_tool("npdmtool.exe")
    if not npdmtool:
        raise SystemExit("npdmtool.exe not found; install switch-dev")
    npdm_json = out / "main.npdm.json"
    write_npdm_json(npdm_json, args.title_id)
    run([npdmtool, npdm_json, exefs / "main.npdm"])

    nacptool = locate_tool("nacptool.exe")
    if not nacptool:
        raise SystemExit("nacptool.exe not found; install switch-dev")
    run([
        nacptool, "--create", args.title, args.author, args.version,
        control / "control.nacp", f"--titleid={args.title_id}",
    ])
    patch_nacp_titles(control / "control.nacp", args.title)

    icon_source = Path(args.icon_source) if args.icon_source else source / "pc" / "uija" / "title" / "char.png"
    if not icon_source.is_file():
        icon_source = source / "image" / "xga" / "cg" / "eyecatch.png"
    if not icon_source.is_file():
        icon_source = source / "Cover.png"
    if not icon_source.is_file():
        raise SystemExit("no icon source (pc/uija/title/char.png / Cover.png / image/xga/cg/eyecatch.png)")
    make_icon(icon_source, control / "icon.jpg")
    shutil.copy2(control / "icon.jpg", control / "icon_AmericanEnglish.dat")

    (out / "build_info.json").write_text(json.dumps({
        "title": args.title,
        "author": args.author,
        "version": args.version,
        "title_id": args.title_id,
        "icon": "control/icon.jpg",
        "exefs": "exefs/main",
        "romfs": "romfs",
    }, ensure_ascii=False, indent=2), encoding="utf-8")

    keys = find_keys(args.keys)
    packer = find_packer(args.packer)
    if packer and "hacbrew" in packer.name.lower():
        local_packer = out / "hacbrewpack.exe"
        if packer.resolve() != local_packer.resolve():
            shutil.copy2(packer, local_packer)
        packer = local_packer
    make_batch(out, args.title_id)
    if keys:
        print(f"keys: {keys}")
    else:
        print("keys: NOT FOUND (staged build only)")
    if packer:
        print(f"packer: {packer}")
    else:
        print("packer: NOT FOUND (staged build only)")

    if keys and packer:
        # hacBrewPack is the recommended one-step packer; hacPack requires
        # a separate NCA/meta sequence and is intentionally not guessed here.
        if "hacbrew" in packer.name.lower():
            # hacBrewPack cannot handle this repository's non-ASCII path.
            # Pack through a short ASCII junction while keeping output in place.
            stage = Path(r"C:\artc_nsp")
            if os.name == "nt":
                if stage.exists():
                    subprocess.run(["cmd", "/c", "rmdir", str(stage)], check=False,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                subprocess.run(["cmd", "/c", "mklink", "/J", str(stage), str(out)],
                               check=True)
            else:
                stage = out
            stage_packer = stage / packer.name
            if not stage_packer.exists() or stage_packer.resolve() != packer.resolve():
                shutil.copy2(packer, stage_packer)
            stage_keys = stage / "prod.keys"
            shutil.copy2(keys, stage_keys)
            nca = stage / "nca"
            nsp = stage / "nsp"
            shutil.rmtree(nca, ignore_errors=True)
            shutil.rmtree(nsp, ignore_errors=True)
            run([
                stage_packer, "-k", stage_keys,
                "--exefsdir", stage / "exefs",
                "--romfsdir", stage / "romfs",
                "--controldir", stage / "control",
                "--ncadir", nca,
                "--nspdir", nsp,
                "--nologo",
            ])
            produced = list(nsp.glob("*.nsp"))
            if produced:
                # hacBrewPack never writes a ticket, so this NSP cannot launch
                # on hardware (Eden still runs it). Keep it out of the final
                # artifact path so it can never silently replace the hacPack
                # build produced by scripts/build_nsp_hacpack.ps1.
                staging_nsp = ROOT / "output" / "aibeya_switch_staging.nsp"
                shutil.copy2(produced[0], staging_nsp)
                print(f"staging NSP (NO ticket, hardware cannot launch): {staging_nsp}")
            else:
                print("[warn] packer produced no NSP")
            stage_keys.unlink(missing_ok=True)
        else:
            print("[warn] hacPack requires an explicit NCA/meta/NSP sequence; use the batch helper or hacBrewPack")
    else:
        print(f"staged: {out}")
        print(f"batch:  {out / 'build_nsp_with_keys.bat'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"build_nsp error: {exc}", file=sys.stderr)
        raise
