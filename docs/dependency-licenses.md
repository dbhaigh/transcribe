# Dependency and model license inventory

This is the repository's current distribution inventory, not legal advice or
approval to redistribute. The license text shipped with each dependency and
the terms for the exact build must be checked again for every release.

| Component | Use and version/provenance | License evidence and package status |
|---|---|---|
| Transcriber source | This repository | MIT License in the root [`LICENSE`](../LICENSE); CMake installs it into release packages. |
| Spotify Basic Pitch model | Bundled ONNX file from upstream commit [`fa5997af0a8210982619003269994a1be25eddf3`](https://github.com/spotify/basic-pitch/commit/fa5997af0a8210982619003269994a1be25eddf3) | Apache-2.0; `python/models/LICENSE-basic-pitch.txt` is installed beside the model in ONNX packages. The model's provenance and adapter contract are documented in [the model catalog](../python/models/README.md). |
| ONNX Runtime | Optional native inference dependency; CI currently exercises CPU 1.20.1 on Linux | MIT. ONNX-enabled packages stage the CPU runtime library, license, and third-party notices when the supplied runtime distribution contains them. GPU provider libraries and their transitive dependencies are not bundled or hardware-qualified by CI. |
| FFmpeg | Optional dynamically linked compressed-audio decoder; version and build options come from the host development package | FFmpeg's applicable LGPL/GPL obligations depend on the exact build configuration. The current package does not bundle FFmpeg shared libraries or their notices; users/distributors must inventory the linked build and provide the required runtime libraries and notices. |
| RtAudio / ASIO SDK | Optional Windows capture build; supplied by the developer | RtAudio and ASIO SDK terms are external to this source package. The ASIO SDK may have separate redistribution terms. The current CI does not build or redistribute ASIO dependencies. |
| Microsoft WebView2 SDK | Optional Windows UI; CI fetches `Microsoft.Web.WebView2` NuGet package `1.0.4258.31` | The SDK's `LICENSE.txt` and `NOTICE.txt`, `WebView2Loader.dll`, and Evergreen bootstrapper are installed into the WebView2 ZIP. The Evergreen Runtime is a separate system installation. |
| MSVC runtime | Windows builds using MSVC | CMake's `InstallRequiredSystemLibraries` stages the detected compiler runtime DLLs beside executables. Verify the exact redistributable terms and required notices for the toolset used to create the release. |
| Verovio | Optional local `verovio@6.3.0` npm package used by the WebView2 UI | LGPL-3.0-or-later; the package is only enabled when its metadata matches the pinned version/license. `COPYING`, `COPYING.LESSER`, and `NOTICE.txt` are installed beside its assets. The renderer is not included in the default build. |
| Catch2 | Test-only FetchContent dependency pinned to `v3.7.1` in CMake | Boost Software License 1.0. It is fetched for test builds and is not installed in product packages. |
| GuitarSet | External real-audio evaluation dataset, v1.1.0 | CC BY 4.0. Dataset archives are not included. Evaluation results and potential Basic Pitch training-data overlap are discussed in [the evaluation documentation](evaluation.md) and [the model evaluation notes](../python/README.md). |

## Release audit rules

- Keep model weights and external recordings out of packages unless their
  exact source, license, attribution, and redistribution rights are reviewed.
- Check the notices in the actual CPack archive, not only the source tree.
  `tests/package_smoke.py` verifies the expected model and UI notice files for
  the corresponding optional configurations.
- Record the exact runtime package, build options, and external runtime
  dependencies used to create every optional-feature artifact.
- A passing build/package smoke test checks payload presence and execution; it
  does not establish that optional dependency terms are compatible with every
  distribution or deployment model.
