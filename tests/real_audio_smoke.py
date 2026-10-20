import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    clip = os.environ.get("TRANSCRIBER_REAL_AUDIO_CLIP") or os.environ.get(
        "TRANSCRIBE_REAL_AUDIO_CLIP"
    )
    if not clip:
        print("Set TRANSCRIBER_REAL_AUDIO_CLIP to run an external guitar/vocal clip smoke test")
        return 77

    executable = Path(sys.argv[1])
    audio = Path(clip).expanduser().resolve()
    if not audio.is_file():
        raise FileNotFoundError(f"Real-audio test clip does not exist: {audio}")

    with tempfile.TemporaryDirectory(prefix="transcribe-real-audio-") as directory:
        notes_path = Path(directory) / "notes.json"
        result = subprocess.run(
            [
                str(executable),
                str(audio),
                "--quality",
                "high",
                "--json",
                str(notes_path),
                "--quiet",
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise RuntimeError(
                f"Real-audio transcription failed for {audio.name}:\n{result.stderr}"
            )
        notes = json.loads(notes_path.read_text(encoding="utf-8"))["notes"]
        if not notes:
            raise RuntimeError(f"No note events were detected in {audio.name}")
    print(f"Real-audio smoke test passed: {audio.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
