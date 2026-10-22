"""Exercise actual Windows GUI executables and the production C++ export bridge."""
import argparse
import base64
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET


def until(check, seconds=40):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = check()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError("Timed out waiting for GUI state")


class DevTools:
    def __init__(self, url):
        target = urlsplit(url)
        if target.hostname not in ("127.0.0.1", "localhost"):
            raise ValueError("Test debugging must remain local")
        self.connection = socket.create_connection((target.hostname, target.port), timeout=30)
        self.sequence = 0
        key = base64.b64encode(os.urandom(16)).decode()
        self.connection.sendall((
            f"GET {target.path} HTTP/1.1\r\nHost: {target.hostname}:{target.port}\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        header = bytearray()
        while not header.endswith(b"\r\n\r\n"):
            header.extend(self.read(1))
            if len(header) > 16384:
                raise RuntimeError("Unexpected debugging handshake")
        accept = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        if not header.startswith(b"HTTP/1.1 101") or accept not in header:
            raise RuntimeError(f"Debugging handshake failed: {header!r}")

    def read(self, count):
        data = bytearray()
        while len(data) < count:
            part = self.connection.recv(count - len(data))
            if not part:
                raise RuntimeError("GUI debugging connection closed")
            data.extend(part)
        return bytes(data)

    def send(self, payload, opcode=1):
        mask = os.urandom(4)
        size = len(payload)
        header = bytes((0x80 | opcode, 0x80 | (size if size < 126 else 126 if size < 65536 else 127)))
        if size >= 126:
            header += struct.pack("!H" if size < 65536 else "!Q", size)
        self.connection.sendall(header + mask + bytes(
            value ^ mask[index % 4] for index, value in enumerate(payload)))

    def evaluate(self, expression):
        self.sequence += 1
        self.send(json.dumps(dict(id=self.sequence, method="Runtime.evaluate", params=dict(
            expression=expression, awaitPromise=True, returnByValue=True))).encode())
        message = bytearray()
        while True:
            first, second = self.read(2)
            opcode, size = first & 15, second & 127
            if size == 126:
                size = struct.unpack("!H", self.read(2))[0]
            elif size == 127:
                size = struct.unpack("!Q", self.read(8))[0]
            if second & 128:
                raise RuntimeError("Unexpected masked debugging response")
            if size > 32 * 1024 * 1024:
                raise RuntimeError("Debugging response exceeded test limit")
            payload = self.read(size)
            if opcode == 9:
                self.send(payload, 10)
                continue
            if opcode == 8:
                raise RuntimeError("GUI closed debugging connection")
            message.extend(payload)
            if not first & 128:
                continue
            response = json.loads(message)
            message.clear()
            if response.get("id") != self.sequence:
                continue
            if "error" in response or response.get("result", {}).get("exceptionDetails"):
                raise AssertionError(response)
            return response["result"]["result"].get("value")

    def close(self):
        self.connection.close()


def exports(directory):
    xml = ET.parse(directory / "transcription.musicxml").getroot()
    assert xml.findall(".//note/pitch"), "Export lacks pitched score notes"
    assert (directory / "transcription.mid").read_bytes().startswith(b"MThd")
    assert len((directory / "transcription.txt").read_text(encoding="utf-8")) > 20


def webview(executable, audio, directory):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    environment = dict(os.environ,
        WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=f"--remote-debugging-port={port}",
        WEBVIEW2_USER_DATA_FOLDER=str(directory / "profile"),
        LOCALAPPDATA=str(directory / "appdata"))
    process = subprocess.Popen([str(executable)], env=environment)
    connection = None
    try:
        def target():
            if process.poll() is not None:
                raise RuntimeError("WebView application exited before startup")
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=1) as response:
                    return next((page.get("webSocketDebuggerUrl") for page in json.load(response)
                                 if "appassets.transcribe/index.html" in page.get("url", "")), None)
            except (OSError, ValueError):
                return None
        connection = DevTools(until(target))
        until(lambda: connection.evaluate("Boolean(window.transcribeConfig)"))
        assert connection.evaluate("""(() => {
          const guide = document.getElementById('journey-guide');
          return !guide.hidden && !document.getElementById('instrument-prior').closest('details');
        })()"""), "Actual host did not expose first-run setup"
        values = {"input-path": str(audio), "model-path": "", "musicxml-path": str(directory / "transcription.musicxml"),
                  "tab-path": str(directory / "transcription.txt"), "midi-path": str(directory / "transcription.mid")}
        connection.evaluate(f"""(() => {{
          for (const [id, value] of Object.entries({json.dumps(values)}))
            document.getElementById(id).value = value;
          document.getElementById('instrument-prior').value = 'guitar';
          document.getElementById('quality-mode').value = 'quality';
          document.getElementById('transcribe-button').click();
        }})()""")
        until(lambda: connection.evaluate("""(() => {
          const result = document.getElementById('result-message');
          if (result.classList.contains('error')) throw Error(result.textContent);
          return document.getElementById('activity-panel').dataset.state === 'completed' &&
            !document.getElementById('export-edits').disabled &&
            document.querySelectorAll('.note-row').length > 0;
        })()"""))
        exports(directory)
        if connection.evaluate("Boolean(window.transcribeConfig.verovio)"):
            until(lambda: connection.evaluate("""(() => {
              const status = document.getElementById('engraving-status').textContent;
              if (/failed|unavailable/i.test(status)) throw Error(status);
              return document.querySelectorAll('.engraving-page').length > 0;
            })()"""))
            connection.evaluate("document.getElementById('engraving-host').scrollIntoView({block:'center'})")
            until(lambda: connection.evaluate(
                "document.querySelectorAll('.engraving-page svg').length > 0"))
        assert connection.evaluate("""(() => {
          return document.getElementById('activity-panel').dataset.state === 'completed' &&
            document.getElementById('spectrogram').getAttribute('aria-label').includes('Hz');
        })()"""), "Actual engine did not publish completed state/frequency metadata"
        assert connection.evaluate("""(() => {
          document.querySelector('.note-row').click();
          document.getElementById('edit-pitch').value = '70';
          document.getElementById('apply-note-edit').click();
          return document.getElementById('detected-pitch').textContent.includes('70');
        })()"""), "Actual-host pitch transaction failed"
        assert connection.evaluate("""(() => {
          const dirty = document.getElementById('save-state').textContent === 'Unsaved edits';
          document.getElementById('undo-button').click();
          const undo = document.getElementById('edit-pitch').value !== '70';
          document.getElementById('redo-button').click();
          document.getElementById('help-button').click();
          const help = document.getElementById('help-dialog').open;
          document.getElementById('close-help').click();
          return dirty && undo && help && document.getElementById('edit-pitch').value === '70';
        })()"""), "Actual-host unsaved/undo/redo/help journey failed"
        connection.evaluate("document.getElementById('export-edits').click()")
        until(lambda: connection.evaluate("document.getElementById('export-edits').textContent.includes('Exported')"))
        xml = ET.parse(directory / "transcription.musicxml")
        assert any(pitch.findtext("step") == "A" and pitch.findtext("alter") == "1"
                   for pitch in xml.findall(".//pitch")), "Actual C++ export did not retain edited A#"
        project = directory / "reviewed.transcribe.json"
        connection.evaluate("""(() => {
          document.getElementById('project-annotation').value = 'Reviewed guitar phrase.';
          document.getElementById('project-annotation').dispatchEvent(new Event('input'));
          document.getElementById('quantization-mode').value = 'evidence';
          document.getElementById('quantization-strength').value = '0.35';
        })()""")
        connection.evaluate(f"""window.dispatchEvent(new CustomEvent('transcribe:project-save',
          {{detail:{{path:{json.dumps(str(project))},scoreOnly:false}}}}))""")
        until(lambda: project.is_file() and connection.evaluate(
            "document.getElementById('save-state').textContent === 'No unsaved edits'"))
        connection.evaluate(f"""window.chrome.webview.postMessage('open-project|' +
          new URLSearchParams({{path:{json.dumps(str(project))}}}).toString())""")
        until(lambda: connection.evaluate(
            "document.getElementById('result-message').textContent.startsWith('Opened reviewed.transcribe.')"))
        assert connection.evaluate("""(() => {
          document.querySelector('.note-row').click();
          return document.getElementById('edit-pitch').value === '70' &&
            document.getElementById('project-annotation').value === 'Reviewed guitar phrase.' &&
            document.getElementById('quantization-mode').value === 'evidence' &&
            document.getElementById('quantization-strength').value === '0.35';
        })()"""), "Saved/reopened actual project lost edited pitch"
        portable_directory = directory / "portable"
        portable_directory.mkdir()
        portable = portable_directory / "owned.transcriber"
        connection.evaluate(f"""window.dispatchEvent(new CustomEvent('transcribe:project-save',
          {{detail:{{path:{json.dumps(str(portable))},copyAudio:true}}}}))""")
        until(lambda: portable.is_file() and connection.evaluate(
            "document.getElementById('save-state').textContent === 'No unsaved edits'"))
        owned = json.loads(portable.read_text(encoding="utf-8"))
        assert owned["project_schema_version"] == 2
        assert owned["audio"]["policy"] == "project-copy"
        asset_name = owned["audio"]["reference_path"]
        assert (portable_directory / asset_name).read_bytes() == audio.read_bytes()
        moved = directory / "moved"
        portable_directory.rename(moved)
        portable = moved / portable.name
        relocated_audio = moved / "located-original.wav"
        (moved / asset_name).rename(relocated_audio)
        connection.evaluate(f"""window.chrome.webview.postMessage('open-project|' +
          new URLSearchParams({{path:{json.dumps(str(portable))}}}).toString())""")
        until(lambda: connection.evaluate(
            "document.getElementById('result-message').textContent.startsWith('Opened owned.')"))
        assert connection.evaluate(
            "document.getElementById('result-message').textContent.includes('Locate audio')"), \
            "Missing project audio did not provide an actionable relocation workflow"
        connection.evaluate(f"""window.dispatchEvent(new CustomEvent('transcribe:project-relink',
          {{detail:{{path:{json.dumps(str(relocated_audio))},generation:100}}}}))""")
        until(lambda: connection.evaluate(
            "document.getElementById('status-text').textContent.startsWith('Recording relocated.')"))
        assert connection.evaluate("""(() => {
          document.querySelector('.note-row').click();
          return document.getElementById('edit-pitch').value === '70' &&
            document.getElementById('save-state').textContent === 'Unsaved edits' &&
            document.getElementById('project-annotation').value === 'Reviewed guitar phrase.';
        })()"""), "Real audio relink erased edits or failed to mark the reference unsaved"
        connection.evaluate("document.getElementById('save-project').click()")
        until(lambda: connection.evaluate(
            "document.getElementById('save-state').textContent === 'No unsaved edits'"))
        relinked = json.loads(portable.read_text(encoding="utf-8"))
        assert relinked["audio"]["reference_path"] == relocated_audio.name
        assert relinked["audio"]["policy"] == "external-reference"
        corrupt = moved / "corrupt.transcriber"
        corrupted = portable.read_text(encoding="utf-8").replace('"title":"owned"', '"title":"altered"', 1)
        assert corrupted != portable.read_text(encoding="utf-8")
        corrupt.write_text(corrupted, encoding="utf-8")
        connection.evaluate(f"""window.chrome.webview.postMessage('open-project|' +
          new URLSearchParams({{path:{json.dumps(str(corrupt))}}}).toString())""")
        until(lambda: connection.evaluate(
            "document.getElementById('activity-panel').dataset.state === 'error'"))
        assert connection.evaluate(
            "document.getElementById('result-message').textContent.includes('integrity check failed')"), \
            "Real host did not surface project corruption"
        connection.evaluate("""(() => {
          document.getElementById('transcribe-button').click();
          document.getElementById('cancel-button').click();
        })()""")
        until(lambda: connection.evaluate(
            "document.getElementById('activity-panel').dataset.state === 'canceled'"))
        assert connection.evaluate("""(() => {
          document.querySelector('.note-row').click();
          return document.getElementById('edit-pitch').value === '70' &&
            !document.getElementById('result-message').classList.contains('error');
        })()"""), "Actual C++ cancellation destroyed accepted edits or appeared as failure"
        connection.evaluate(f"""(() => {{
          document.getElementById('input-path').value = {json.dumps(str(directory / 'missing.wav'))};
          document.getElementById('transcribe-button').click();
        }})()""")
        until(lambda: connection.evaluate(
            "document.getElementById('activity-panel').dataset.state === 'error'"))
        assert connection.evaluate(
            "!document.getElementById('transcribe-button').disabled"), "Actual decoder error left workstation locked"
        print("Actual WebView2/C++ journey: setup, engine, edits/undo, export, v2 save/reopen, annotations/settings, explicit audio copy, missing-audio relocation, corruption rejection, cancellation and decoder errors passed")
    except Exception:
        if connection:
            print("Actual-host failure state:", connection.evaluate("""({
              activity: document.getElementById('activity-panel').dataset.state,
              status: document.getElementById('status-text').textContent,
              result: document.getElementById('result-message').textContent,
              engraving: document.getElementById('engraving-status').textContent,
              title: document.getElementById('project-title').textContent
            })"""))
        raise
    finally:
        if connection:
            # Ask the real native host to quiesce its WebView before profile cleanup.
            connection.evaluate("window.chrome.webview.postMessage('stop-playback|')")
            connection.close()
        user = ctypes.WinDLL("user32", use_last_error=True)
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        def close(handle, _):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(handle, ctypes.byref(pid))
            if pid.value == process.pid:
                user.PostMessageW(handle, 0x0010, 0, 0)
            return True
        user.EnumWindows(callback_type(close), 0)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        time.sleep(2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--webview", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt":
        return 77
    audio = Path(__file__).resolve().parents[1] / "samples" / "single_a4.wav"
    temporary = tempfile.TemporaryDirectory(prefix="transcriber-host-")
    try:
        directory = Path(temporary.name) / "Unicode \u00e9 output"
        directory.mkdir()
        webview(args.webview.resolve(), audio, directory)
    finally:
        def cleanup():
            try:
                temporary.cleanup()
                return True
            except PermissionError:
                # WebView child processes release profile handles after host exit.
                return False
        until(cleanup, seconds=20)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
