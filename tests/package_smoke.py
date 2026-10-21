"""Smoke-test an extracted CPack artifact without using the build tree."""

import argparse
import os
import shutil
import stat
import subprocess
import tarfile
import tempfile
import zipfile
from contextlib import contextmanager
from pathlib import Path, PurePosixPath


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def safe_destination(root, archive_name):
    require("\\" not in archive_name, f"Unexpected backslash in package path: {archive_name}")
    member = PurePosixPath(archive_name)
    require(not member.is_absolute() and ".." not in member.parts,
            f"Unsafe archive path: {archive_name}")
    destination = root.joinpath(*member.parts).resolve()
    require(destination == root or root in destination.parents,
            f"Archive path escapes extraction directory: {archive_name}")
    return destination


def extract_zip(archive, root):
    with zipfile.ZipFile(archive) as package:
        for member in package.infolist():
            destination = safe_destination(root, member.filename)
            mode = member.external_attr >> 16
            require(not stat.S_ISLNK(mode), f"Unexpected symlink in package: {member.filename}")
            if member.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
            else:
                destination.parent.mkdir(parents=True, exist_ok=True)
                with package.open(member) as source, destination.open("wb") as target:
                    shutil.copyfileobj(source, target)


def extract_tar(archive, root):
    with tarfile.open(archive, "r:*") as package:
        for member in package.getmembers():
            destination = safe_destination(root, member.name)
            require(member.isdir() or member.isfile(),
                    f"Unexpected link or special file in package: {member.name}")
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            source = package.extractfile(member)
            require(source is not None, f"Could not read package member: {member.name}")
            with source, destination.open("wb") as target:
                shutil.copyfileobj(source, target)


@contextmanager
def package_directory(package):
    if package.is_dir():
        yield package.resolve()
        return
    with tempfile.TemporaryDirectory(prefix="transcriber-package-") as temporary:
        root = Path(temporary).resolve()
        if zipfile.is_zipfile(package):
            extract_zip(package, root)
        else:
            extract_tar(package, root)
        yield root


def find_named(root, name):
    return [path for path in root.rglob("*")
            if path.is_file() and path.name.casefold() == name.casefold()]


def require_named(root, name, description=None):
    matches = find_named(root, name)
    require(matches, f"Package is missing {description or name}")
    return matches[0]


def run(command, cwd, environment, timeout=90):
    completed = subprocess.run(
        [str(part) for part in command],
        cwd=cwd,
        env=environment,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    require(
        completed.returncode == 0,
        f"Command failed ({completed.returncode}): {' '.join(map(str, command))}\n"
        f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
    )
    return completed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path, help="CPack archive or already-extracted package")
    parser.add_argument("--expect-webview2", action="store_true")
    parser.add_argument("--expect-verovio", action="store_true")
    parser.add_argument("--expect-model", action="store_true")
    parser.add_argument("--expect-ffmpeg", action="store_true")
    parser.add_argument("--expect-msvc-runtime", action="store_true")
    parser.add_argument(
        "--isolated-runtime",
        action="store_true",
        help="Do not inherit LD_LIBRARY_PATH or DYLD_LIBRARY_PATH",
    )
    args = parser.parse_args()
    require(args.package.exists(), f"Package does not exist: {args.package}")

    with package_directory(args.package) as root:
        executable_name = "transcriber.exe" if os.name == "nt" else "transcriber"
        executable = require_named(root, executable_name, "the transcriber CLI")
        license_file = require_named(root, "LICENSE", "the project MIT license")
        require("MIT License" in license_file.read_text(encoding="utf-8"),
                "Packaged project license is not MIT")
        docs = require_named(root, "dependency-licenses.md", "the dependency license inventory")
        require(docs.stat().st_size > 0, "The dependency license inventory is empty")
        require_named(root, "release-checklist.md", "the release checklist")
        audio = require_named(root, "single_a4.wav", "the installed synthetic smoke fixture")

        if args.expect_msvc_runtime:
            require(os.name == "nt", "MSVC runtime validation is only applicable on Windows")
            for name in ("msvcp140.dll", "vcruntime140.dll"):
                runtime = require_named(root, name, f"the MSVC runtime {name}")
                require(runtime.parent == executable.parent,
                        f"MSVC runtime {name} is not installed beside the CLI")

        if args.expect_webview2:
            gui = require_named(root, "transcriber-gui.exe", "the WebView2 GUI executable")
            for name, description in (
                ("WebView2Loader.dll", "the WebView2 loader DLL"),
                ("MicrosoftEdgeWebView2Setup.exe", "the WebView2 runtime bootstrapper"),
                ("LICENSE.txt", "the WebView2 SDK license"),
                ("NOTICE.txt", "the WebView2 SDK notice"),
            ):
                resource = require_named(root, name, description)
                require(resource.parent == gui.parent,
                        f"{description} is not installed beside the GUI executable")
            ui_index = require_named(root, "index.html", "the installed WebView2 UI")
            require(ui_index.parent.name.casefold() == "ui", "UI assets were not installed beside the GUI")
            require(gui.parent == ui_index.parent.parent,
                    "WebView2 UI assets are not located beside the GUI executable")

        if args.expect_verovio:
            module = require_named(root, "verovio-module.mjs", "the Verovio WebAssembly module")
            vendor = module.parent
            for name, description in (
                ("verovio.mjs", "the Verovio JavaScript module"),
                ("COPYING", "the Verovio LGPL license"),
                ("COPYING.LESSER", "the Verovio LGPL supplement"),
                ("NOTICE.txt", "the Verovio notice"),
            ):
                require((vendor / name).is_file(), f"Missing {description} beside Verovio assets")

        model = None
        if args.expect_model:
            model = require_named(root, "basic_pitch.onnx", "the bundled Basic Pitch model")
            require_named(root, "LICENSE-basic-pitch.txt", "the Basic Pitch model license")
            if os.name == "nt":
                runtime = require_named(root, "onnxruntime.dll", "the ONNX Runtime DLL")
                require(runtime.parent == executable.parent,
                        "ONNX Runtime DLL is not installed beside the CLI")
            else:
                runtime = require_named(
                    root, "libonnxruntime.so.1", "the staged ONNX Runtime shared library")
                require(runtime.parent == executable.parent.parent / "lib",
                        "ONNX Runtime shared library is not installed in the package lib directory")
            runtime_docs = executable.parent.parent / "share" / "doc" / "transcriber" / "third-party" / "onnxruntime"
            require((runtime_docs / "LICENSE.txt").is_file(),
                    "ONNX Runtime license is missing from the package documentation")
            require((runtime_docs / "ThirdPartyNotices.txt").is_file(),
                    "ONNX Runtime third-party notices are missing from the package documentation")

        environment = os.environ.copy()
        if args.isolated_runtime:
            environment.pop("LD_LIBRARY_PATH", None)
            environment.pop("DYLD_LIBRARY_PATH", None)
        help_result = run([executable, "--help"], executable.parent, environment)
        require("Transcriber 0.1.5" in help_result.stdout,
                "Packaged CLI version does not match the current release")

        with tempfile.TemporaryDirectory(prefix="transcriber-package-output-") as temporary:
            output = Path(temporary)
            command = [
                executable, audio, "--quiet",
                "--musicxml", output / "score.musicxml",
                "--tab", output / "score.tab",
                "--json", output / "notes.json",
            ]
            run(command, executable.parent, environment)
            for name in ("score.musicxml", "score.tab", "notes.json"):
                artifact = output / name
                require(artifact.is_file() and artifact.stat().st_size > 0,
                        f"Packaged CLI did not produce {name}")

            if model is not None:
                run([
                    executable, audio, "--quiet", "--polyphonic",
                    "--provider", "cpu", "--model", model,
                    "--json", output / "onnx-notes.json",
                ], executable.parent, environment, timeout=180)
                require((output / "onnx-notes.json").is_file(),
                        "Packaged ONNX inference did not produce JSON output")

            if args.expect_ffmpeg:
                ffmpeg = shutil.which("ffmpeg")
                require(ffmpeg is not None, "FFmpeg feature smoke needs the ffmpeg executable")
                compressed = output / "single_a4.flac"
                run([
                    ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                    "-i", audio, "-c:a", "flac", compressed,
                ], output, environment)
                run([
                    executable, compressed, "--quiet",
                    "--musicxml", output / "ffmpeg.musicxml",
                ], executable.parent, environment)
                require((output / "ffmpeg.musicxml").is_file(),
                        "Packaged FFmpeg-enabled CLI did not decode the FLAC fixture")

    print("Package extraction, required payload, CLI help, and smoke exports passed.")


if __name__ == "__main__":
    main()
