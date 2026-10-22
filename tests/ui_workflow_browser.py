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


def realtime_browser(browser, profile, url):
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
            until(lambda: connection.evaluate(
                "['passed','failed'].includes(document.body?.dataset.result)"), seconds=60)
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


def run_browser(root, page, budget=10000, realtime=False):
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
                    f"http://127.0.0.1:{server.server_port}/{page}")
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


def main():
    return run_browser(Path(__file__).resolve().parents[1], "tests/ui_workflow.html", realtime=True)


if __name__ == "__main__":
    sys.exit(main())
