"""Run the dependency-free browser workflow harness in an available Chromium browser."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import html
import json
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.request

from workstation_host import DevTools, until


def realtime_browser(browser, profile, url, replay=None):
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen([
            browser, "--headless", "--disable-gpu", "--no-first-run",
            "--no-default-browser-check", f"--user-data-dir={profile}",
            "--remote-debugging-port=0", url,
        ], stdout=log, stderr=log)
        connection = None
        try:
            port_file = Path(profile) / "DevToolsActivePort"
            def ready():
                if process.poll() is not None:
                    raise RuntimeError(f"Browser exited with {process.returncode}")
                try:
                    return port_file.is_file() and port_file.read_text().splitlines()
                except PermissionError:
                    return None  # Chromium briefly holds an exclusive creation handle on Windows.
            port = int(until(ready)[0])
            def target():
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=2) as response:
                    return next((page["webSocketDebuggerUrl"] for page in json.load(response)
                                 if page.get("url") == url), None)
            connection = DevTools(until(target))
            if replay is not None:
                until(lambda: connection.evaluate(
                    "['ready','passed','failed'].includes(document.body?.dataset.result)"), seconds=60)
                if connection.evaluate("document.body.dataset.result") == "failed":
                    raise AssertionError(connection.evaluate("document.getElementById('result').textContent"))
                connection.evaluate(f"window.runLiveReplay({json.dumps(replay, separators=(',', ':'))})")
            until(lambda: connection.evaluate(
                "['passed','failed'].includes(document.body?.dataset.result)"), seconds=120)
            status = connection.evaluate("document.getElementById('result').textContent")
            print(status)
            return 0 if connection.evaluate("document.body.dataset.result") == "passed" else 1
        finally:
            if connection:
                connection.close()
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def run_browser(root, page, budget=10000, realtime=False, replay=None):
    candidates = [
        os.environ.get("TRANSCRIBE_BROWSER", ""),
        shutil.which("msedge") or "",
        shutil.which("chromium") or "",
        shutil.which("google-chrome") or "",
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    ]
    browser = next((candidate for candidate in candidates if candidate and Path(candidate).is_file()), None)
    if not browser:
        print("Chromium browser unavailable; set TRANSCRIBE_BROWSER to enable UI workflow tests")
        return 77
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(SimpleHTTPRequestHandler, directory=str(root)))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="transcriber-browser-") as profile:
            if realtime:
                return realtime_browser(browser, profile,
                    f"http://127.0.0.1:{server.server_port}/{page}", replay)
            result = subprocess.run([
                browser, "--headless", "--disable-gpu", "--no-first-run",
                "--no-default-browser-check", f"--user-data-dir={profile}",
                "--dump-dom", f"--virtual-time-budget={budget}",
                f"http://127.0.0.1:{server.server_port}/{page}",
            ], capture_output=True, text=True, timeout=60, encoding="utf-8", errors="replace")
            if result.returncode or 'data-result="passed"' not in result.stdout:
                status = re.search(r'<pre id="result">(.*?)</pre>', result.stdout, re.DOTALL)
                print(html.unescape(status.group(1)) if status else result.stdout[:2000])
                print(result.stderr[-4000:])
                return 1
            status = re.search(r'<pre id="result">(.*?)</pre>', result.stdout, re.DOTALL)
            print(html.unescape(status.group(1)) if status else "Browser workflow passed")
            return 0
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def live_callback_replay(root, runner, audio):
    expected_path = audio.with_name(f"{audio.stem}.expected.txt")
    expected_pitches = [int(value) for value in re.findall(
        r"\((\d+)\)", expected_path.read_text(encoding="utf-8"))]
    if not expected_pitches:
        raise ValueError(f"No expected pitches found in {expected_path}")

    for delay_ms, overload_probe in ((0, False), (500, True)):
        with tempfile.TemporaryDirectory(prefix="transcriber-live-replay-") as temporary:
            report_path = Path(temporary) / "live-replay.json"
            command = [
                str(runner), str(audio), str(report_path),
                "--backend", "yin", "--pace", "realtime", "--policy", "skip",
                "--callback-replay", "1", "--bridge-replay", "1",
                "--analysis-delay-ms", str(delay_ms),
            ]
            completed = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if completed.returncode:
                raise RuntimeError(
                    f"Live callback replay failed ({completed.returncode}): {completed.stderr}")
            report = json.loads(report_path.read_text(encoding="utf-8"))
            if report.get("state") != "finished" or not report.get("bridge_batches"):
                raise AssertionError("The actual callback transport did not finish with bridge batches")
            replay = {
                "batches": report["bridge_batches"],
                "expected_pitches": expected_pitches,
                "overload_probe": overload_probe,
            }
            print(
                f"callback replay delay={delay_ms}ms, wall={report['live_wall_seconds']:.2f}s, "
                f"batches={len(replay['batches'])}, dropped={report['dropped_blocks']}, "
                f"skipped_frames={report['metrics']['skipped_frames']}")
            result = run_browser(
                root, "tests/ui_workflow.html?live-replay", realtime=True, replay=replay)
            if result:
                return result
    return 0


def main():
    root = Path(__file__).resolve().parents[1]
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live-replay", action="store_true",
                        help="run real-time WAV callback, production bridge, and browser replay checks")
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--audio", type=Path, default=root / "samples" / "guitar_scale.wav")
    args = parser.parse_args()
    if not args.live_replay:
        return run_browser(root, "tests/ui_workflow.html", realtime=True)
    if not args.runner:
        parser.error("--live-replay requires --runner")
    return live_callback_replay(root, args.runner.resolve(), args.audio.resolve())


if __name__ == "__main__":
    sys.exit(main())
