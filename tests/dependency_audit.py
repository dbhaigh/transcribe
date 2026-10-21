"""Check that pinned dependencies and bundled license evidence stay aligned."""

import sys
from pathlib import Path


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def require_text(path, text, description):
    require(path.is_file(), f"Missing {description}: {path}")
    contents = path.read_text(encoding="utf-8", errors="replace")
    require(text in contents, f"{description} does not contain {text!r}: {path}")
    return contents


def main():
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    cmake = require_text(root / "CMakeLists.txt", "project(transcriber VERSION 0.1.5",
                         "project version")
    require("GIT_TAG v3.7.1" in cmake, "Catch2 pin is missing")
    require("InstallRequiredSystemLibraries" in cmake,
            "MSVC compiler runtime deployment is not configured")
    require("6.3.0" in cmake and "LGPL-3.0-or-later" in cmake,
            "Verovio version/license validation is missing from CMake")
    require("install(FILES" in cmake and "ONNXRUNTIME_THIRD_PARTY_NOTICES" in cmake,
            "ONNX Runtime notices are not staged by the install rules")

    workflow = require_text(
        root / ".github" / "workflows" / "ci.yml",
        "1.0.4258.31",
        "WebView2 SDK pin",
    )
    require("verovio@6.3.0" in workflow, "CI does not pin the tested Verovio package")

    inventory = require_text(
        root / "docs" / "dependency-licenses.md",
        "MIT License",
        "license inventory",
    )
    for dependency in ("Apache-2.0", "LGPL-3.0-or-later", "Boost Software License 1.0",
                       "FFmpeg", "RtAudio", "ONNX Runtime", "WebView2", "MSVC runtime"):
        require(dependency in inventory, f"License inventory omits {dependency}")

    model_readme = require_text(
        root / "python" / "models" / "README.md",
        "fa5997af0a8210982619003269994a1be25eddf3",
        "Basic Pitch model provenance",
    )
    require("Apache-2.0" in model_readme, "Basic Pitch license is not documented")
    model_license = require_text(
        root / "python" / "models" / "LICENSE-basic-pitch.txt",
        "Apache License",
        "Basic Pitch license text",
    )
    require("Version 2.0" in model_license, "Basic Pitch license text is not Apache-2.0")

    for name in ("COPYING", "COPYING.LESSER", "NOTICE.txt"):
        require((root / "third_party" / "verovio" / name).is_file(),
                f"Verovio distribution notice is missing: {name}")

    readme = require_text(root / "README.md", "MIT License",
                          "README license status")
    require("workflow uploads evaluation JSON/text reports" in readme,
            "README does not describe benchmark artifact policy")
    license_text = require_text(root / "LICENSE", "MIT License",
                                "project MIT license")
    require("Copyright (c) 2026 Transcriber contributors" in license_text,
            "Project MIT license copyright notice is missing")
    main_cpp = require_text(root / "src" / "main.cpp", "TRANSCRIBE_VERSION",
                            "CLI version output")
    require("TRANSCRIBE_VERSION" in cmake and "0.1.5" in cmake,
            "CLI version is not sourced from the CMake project version")

    print("Dependency pins and bundled license evidence are consistent with the inventory.")


if __name__ == "__main__":
    main()
