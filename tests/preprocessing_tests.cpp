#include "transcribe/preprocessing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <array>
#include <fstream>

namespace {

struct Fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("transcriber-isolation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { std::filesystem::create_directory(directory); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(directory, error); }
    std::filesystem::path wav(const std::string& name, std::uint32_t frames = 44100,
                              std::uint32_t rate = 44100) {
        const auto path = directory / name;
        std::ofstream out(path, std::ios::binary);
        const auto integer = [&](std::uint32_t value, unsigned bytes) {
            for (unsigned i = 0; i < bytes; ++i) out.put(static_cast<char>((value >> (8 * i)) & 255));
        };
        out.write("RIFF", 4); integer(36 + frames * 2, 4); out.write("WAVEfmt ", 8);
        integer(16, 4); integer(1, 2); integer(1, 2); integer(rate, 4);
        integer(rate * 2, 4); integer(2, 2); integer(16, 2); out.write("data", 4); integer(frames * 2, 4);
        for (std::uint32_t i = 0; i < frames; ++i) {
            // A late impulse verifies that validation never shifts or trims silence.
            integer(i == frames - 1 ? 12000 : 0, 2);
        }
        return path;
    }
};

struct Backend : transcribe::AudioPreprocessor {
    std::function<std::filesystem::path(std::stop_token)> run;
    std::filesystem::path isolate(const transcribe::PreprocessingRequest&, std::stop_token token) override {
        return run(token);
    }
};

} // namespace

TEST_CASE("optional isolation preserves original coordinates and bypasses all work when disabled",
          "[preprocessing][alignment]") {
    Fixture fixture;
    const auto original = fixture.wav("mixture.wav");
    const auto stem = fixture.wav("bass.wav");
    const auto disabled = transcribe::prepare_audio({fixture.directory / "not-opened.wav", {}, "", false});
    REQUIRE_FALSE(disabled.isolated);
    REQUIRE(disabled.identities.empty());
    for (const auto target : {"guitar", "bass", "piano", "voice"}) {
        const auto prepared = transcribe::prepare_audio({original, stem, target, false});
        REQUIRE(prepared.isolated);
        REQUIRE(prepared.analysis_path == stem);
        REQUIRE(prepared.identities.size() == 2);
        const auto audio = transcribe::read_wav(prepared.analysis_path);
        REQUIRE(audio.samples.size() == 44100);
        REQUIRE(audio.samples.front() == 0);
        REQUIRE(audio.samples.back() > 0);
        REQUIRE_NOTHROW(transcribe::verify_prepared_audio(prepared));
    }
    REQUIRE_THROWS_AS(transcribe::prepare_audio({original, stem, "", false}), std::invalid_argument);
    REQUIRE_THROWS_AS(transcribe::prepare_audio({original, original, "bass", false}), std::invalid_argument);
    const std::array blocked{stem};
    REQUIRE_THROWS_AS(transcribe::protect_preprocessing_inputs({original, stem, "bass", false}, blocked),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(transcribe::prepare_audio({original, fixture.wav("short.wav", 44099), "bass", true}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(transcribe::prepare_audio({original, fixture.wav("rate.wav", 44100, 48000), "bass", true}),
                      std::invalid_argument);
}

TEST_CASE("preprocessing fallback is explicit and cannot mask cancellation corruption or inference failure",
          "[preprocessing][cancellation]") {
    Fixture fixture;
    const auto original = fixture.wav("mixture.wav");
    const auto missing = fixture.directory / "missing.wav";
    REQUIRE_THROWS_AS(transcribe::prepare_audio({original, missing, "voice", false}),
                      transcribe::PreprocessingUnavailable);
    const auto fallback = transcribe::prepare_audio({original, missing, "voice", true});
    REQUIRE_FALSE(fallback.isolated);
    REQUIRE(fallback.analysis_path == original);
    REQUIRE(fallback.diagnostic.find("explicit fallback") != std::string::npos);
    Backend backend;
    backend.run = [](std::stop_token) -> std::filesystem::path {
        throw transcribe::PreprocessingUnavailable("CUDA provider is unavailable");
    };
    REQUIRE_FALSE(transcribe::prepare_audio({original, {}, "voice", true}, {}, &backend).isolated);
    backend.run = [](std::stop_token) -> std::filesystem::path {
        throw std::runtime_error("Failed model inference: invalid output tensor");
    };
    REQUIRE_THROWS_AS(transcribe::prepare_audio({original, {}, "voice", true}, {}, &backend),
                      std::runtime_error);
    std::stop_source cancellation;
    backend.run = [&](std::stop_token token) -> std::filesystem::path {
        cancellation.request_stop();
        REQUIRE(token.stop_requested());
        throw transcribe::PreprocessingUnavailable("Provider lost");
    };
    REQUIRE_THROWS_AS(transcribe::prepare_audio({original, {}, "voice", true},
        cancellation.get_token(), &backend), transcribe::PreprocessingCancelled);
    REQUIRE_THROWS_AS(transcribe::prepare_audio({original, {}, "", false}, cancellation.get_token()),
                      transcribe::PreprocessingCancelled);
    const auto corrupt = fixture.directory / "broken.wav";
    { std::ofstream out(corrupt); out << "invalid"; }
    REQUIRE_THROWS(transcribe::prepare_audio({original, corrupt, "voice", true}));
}

TEST_CASE("changed sources invalidate preprocessing before results are published",
          "[preprocessing][source-change]") {
    Fixture fixture;
    const auto original = fixture.wav("mixture.wav");
    const auto stem = fixture.wav("stem.wav");
    auto prepared = transcribe::prepare_audio({original, stem, "piano", false});
    { std::ofstream out(stem, std::ios::app | std::ios::binary); out.put(0); }
    REQUIRE_THROWS(transcribe::verify_prepared_audio(prepared));
    Backend backend;
    backend.run = [&](std::stop_token) {
        { std::ofstream out(original, std::ios::app | std::ios::binary); out.put(0); }
        return stem;
    };
    REQUIRE_THROWS(transcribe::prepare_audio({original, {}, "guitar", true}, {}, &backend));
}
