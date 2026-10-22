#include "transcribe/decoding.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace transcribe {

std::string_view evidence_semantics_name(EvidenceSemantics semantics) noexcept {
    switch (semantics) {
    case EvidenceSemantics::Unspecified: return "unspecified";
    case EvidenceSemantics::HeuristicScore: return "heuristic-score";
    case EvidenceSemantics::RelativeActivation: return "relative-activation";
    case EvidenceSemantics::RawActivation: return "raw-activation";
    case EvidenceSemantics::CalibratedProbability: return "calibrated-probability";
    }
    return "unspecified";
}

EvidenceSemantics parse_evidence_semantics(std::string_view value) {
    if (value == "unspecified") return EvidenceSemantics::Unspecified;
    if (value == "heuristic-score") return EvidenceSemantics::HeuristicScore;
    if (value == "relative-activation") return EvidenceSemantics::RelativeActivation;
    if (value == "raw-activation") return EvidenceSemantics::RawActivation;
    if (value == "calibrated-probability") return EvidenceSemantics::CalibratedProbability;
    throw std::invalid_argument("Unknown evidence semantics: " + std::string(value));
}

namespace {

void validate(const TemporalDecodingOptions& o) {
    for (const double value : {o.activation_on, o.activation_off, o.maximum_gap_seconds,
             o.minimum_duration_seconds, o.energy_floor_rms, o.energy_relative_threshold,
             o.spectral_peak_fraction, o.pitch_stability_seconds, o.pitch_tolerance_semitones,
             o.onset_threshold, o.minimum_onset_spacing_seconds, o.onset_energy_rise,
             o.onset_state_drop}) {
        if (!std::isfinite(value) || value < 0) {
            throw std::invalid_argument("Temporal decoding parameters must be finite and nonnegative");
        }
    }
    if (o.activation_on > 1 || o.activation_off > o.activation_on ||
        o.minimum_duration_seconds <= 0 || o.energy_relative_threshold > 1 ||
        o.spectral_peak_fraction > 1 || o.onset_threshold > 1 ||
        o.pitch_tolerance_semitones > 1 || o.onset_state_drop > 1) {
        throw std::invalid_argument("Invalid temporal decoding thresholds");
    }
}

void validate_source(const AudioBuffer& audio, const std::string& source,
                     const std::string& model) {
    if (audio.sample_rate < 2000 || source.empty() || model.empty()) {
        throw std::invalid_argument("Decoding requires source rate and nonempty provenance identifiers");
    }
    if (std::any_of(audio.samples.begin(), audio.samples.end(),
                    [](float value) { return !std::isfinite(value); })) {
        throw std::invalid_argument("Nonfinite source audio");
    }
}

void order_evidence(std::vector<CanonicalNoteEvent>& events) {
    for (auto& event : events) {
        std::stable_sort(event.evidence.begin(), event.evidence.end(), [](const auto& a, const auto& b) {
            return a.time_seconds < b.time_seconds;
        });
    }
}

std::uint64_t sample_at(double seconds, const AudioBuffer& audio) {
    return static_cast<std::uint64_t>(std::llround(std::clamp(
        seconds * audio.sample_rate, 0.0, static_cast<double>(audio.samples.size()))));
}

bool valid_evidence_semantics(EvidenceSemantics semantics) {
    return static_cast<int>(semantics) >=
            static_cast<int>(EvidenceSemantics::Unspecified) &&
        static_cast<int>(semantics) <=
            static_cast<int>(EvidenceSemantics::CalibratedProbability);
}

struct Region { double start; double end; bool strong_at_start{true}; };

bool strong(const FrameEvidenceTrack& track, std::size_t index, double threshold,
            const TemporalDecodingOptions& options) {
    const double strength = track.note_states.empty() ? track.frames[index].confidence :
        std::clamp(track.note_states[index], 0.0, 1.0);
    return strength >= threshold &&
        (track.spectral_peak_fractions.empty() ||
         track.spectral_peak_fractions[index] >= options.spectral_peak_fraction);
}

std::string event_id(const CanonicalNoteEvent& event) {
    return std::to_string(event.audio_source_id.size()) + ":" + event.audio_source_id + ":" +
        std::to_string(event.source_model.size()) + ":" + event.source_model + ":" +
        std::to_string(event.detection_method.size()) + ":" + event.detection_method + ":" +
        std::to_string(static_cast<int>(event.midi_pitch)) + ":" + std::to_string(event.onset_sample);
}

std::vector<Region> energy_regions(const AudioBuffer& audio,
                                   const TemporalDecodingOptions& options) {
    const std::size_t block = std::max<std::size_t>(1, audio.sample_rate / 200);
    std::vector<double> rms;
    double peak = 0;
    for (std::size_t first = 0; first < audio.samples.size(); first += block) {
        const auto end = std::min(first + block, audio.samples.size());
        double energy = 0;
        for (auto i = first; i < end; ++i) {
            const double sample = audio.samples[i];
            if (!std::isfinite(sample)) throw std::invalid_argument("Nonfinite source audio");
            energy += sample * sample;
        }
        rms.push_back(std::sqrt(energy / (end - first)));
        peak = std::max(peak, rms.back());
    }
    const double threshold = std::max(options.energy_floor_rms,
                                     peak * options.energy_relative_threshold);
    std::vector<Region> regions;
    for (std::size_t i = 0; i < rms.size();) {
        if (rms[i] < options.energy_floor_rms || rms[i] == 0) { ++i; continue; }
        const auto first = i++;
        while (i < rms.size() && rms[i] >= options.energy_floor_rms && rms[i] > 0) ++i;
        const Region region{static_cast<double>(first * block) / audio.sample_rate,
            static_cast<double>(std::min(i * block, audio.samples.size())) / audio.sample_rate,
            rms[first] >= std::max(threshold, peak * .5)};
        if (!regions.empty() && region.start - regions.back().end <= options.maximum_gap_seconds) {
            regions.back().end = region.end;
        } else regions.push_back(region);
    }
    return regions;
}

void validate_track(const FrameEvidenceTrack& track) {
    if (!std::isfinite(track.window_seconds) || track.window_seconds < 0 ||
        !std::isfinite(track.hop_seconds) || track.hop_seconds <= 0 ||
        track.method.empty() || track.evidence_kind.empty() ||
        track.raw_midi_pitch < -1 || track.raw_midi_pitch > 127 ||
        !valid_evidence_semantics(track.selected_strength_semantics) ||
        !valid_evidence_semantics(track.raw_value_semantics)) {
        throw std::invalid_argument("Invalid frame time geometry or evidence semantics");
    }
    for (const auto* values : {&track.raw_values, &track.spectral_peak_fractions,
                              &track.onset_strengths, &track.note_states, &track.contour_midi}) {
        if (!values->empty() && values->size() != track.frames.size()) {
            throw std::invalid_argument("Evidence arrays must align with pitch frames");
        }
        for (const double value : *values) {
            if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite frame evidence");
        }
    }
    double previous_time = -1;
    for (const auto& frame : track.frames) {
        if (!std::isfinite(frame.time_seconds) || frame.time_seconds < previous_time ||
            frame.time_seconds < 0 || !std::isfinite(frame.confidence) ||
            !std::isfinite(frame.voicing_probability) || frame.midi_pitch < -1 ||
            frame.midi_pitch > 127 || (frame.fractional_midi &&
                (!std::isfinite(*frame.fractional_midi) ||
                 *frame.fractional_midi < 0 || *frame.fractional_midi > 127))) {
            throw std::invalid_argument("Invalid frame evidence");
        }
        previous_time = frame.time_seconds;
    }
    for (const double value : track.spectral_peak_fractions) {
        if (value < 0 || value > 1) throw std::invalid_argument("Invalid spectral evidence");
    }
}

void set_times(CanonicalNoteEvent& event, double first, double last, const AudioBuffer& audio) {
    event.onset_sample = sample_at(first, audio);
    event.offset_sample = sample_at(last, audio);
    event.onset_seconds = static_cast<double>(event.onset_sample) / audio.sample_rate;
    event.offset_seconds = static_cast<double>(event.offset_sample) / audio.sample_rate;
    double energy = 0;
    for (auto i = event.onset_sample; i < event.offset_sample; ++i) {
        energy += static_cast<double>(audio.samples[i]) * audio.samples[i];
    }
    if (event.offset_sample > event.onset_sample) {
        event.intensity_rms = std::sqrt(energy / (event.offset_sample - event.onset_sample));
    }
}
}

void validate_temporal_decoding_options(const TemporalDecodingOptions& options) {
    validate(options);
}

std::vector<int> stabilize_pitch_activity(
    const FrameEvidenceTrack& track, const TemporalDecodingOptions& options) {
    validate(options);
    validate_track(track);
    std::vector<int> pitches;
    pitches.reserve(track.frames.size());
    int anchor = -1;
    double excursion_start = -1;
    for (std::size_t i = 0; i < track.frames.size(); ++i) {
        const auto& frame = track.frames[i];
        int pitch = frame.midi_pitch;
        if (!track.note_states.empty() && track.raw_midi_pitch >= 0 &&
            (pitch >= 0 || options.activation_off < options.activation_on) &&
            strong(track, i, options.activation_off, options)) pitch = track.raw_midi_pitch;
        if (track.method == "yin" && frame.fractional_midi && pitch >= 0 && anchor >= 0 &&
            options.pitch_stability_seconds > 0 &&
            std::abs(*frame.fractional_midi - anchor) <= options.pitch_tolerance_semitones) {
            if (pitch != anchor) {
                if (excursion_start < 0) excursion_start = frame.time_seconds;
                if (frame.time_seconds - excursion_start < options.pitch_stability_seconds) {
                    pitch = anchor;
                }
            } else excursion_start = -1;
        } else {
            excursion_start = -1;
        }
        pitches.push_back(pitch);
        if (pitch != anchor) excursion_start = -1;
        anchor = pitch;
    }
    return pitches;
}

std::vector<PitchActivitySegment> detect_pitch_activity_regions(
    const FrameEvidenceTrack& track, const TemporalDecodingOptions& options) {
    auto pitches = stabilize_pitch_activity(track, options);
    std::map<int, std::vector<std::size_t>> active;
    for (std::size_t i = 0; i < pitches.size(); ++i) {
        if (pitches[i] >= 0 && strong(track, i, options.activation_off, options)) {
            active[pitches[i]].push_back(i);
        }
    }
    std::vector<PitchActivitySegment> segments;
    const double center = track.window_seconds / 2;
    for (const auto& [pitch, indices] : active) {
        for (std::size_t begin = 0; begin < indices.size();) {
            if (!strong(track, indices[begin], options.activation_on, options)) {
                ++begin;
                continue;
            }
            std::size_t end = begin + 1;
            while (end < indices.size() &&
                track.frames[indices[end]].time_seconds -
                    track.frames[indices[end - 1]].time_seconds - track.hop_seconds <=
                        options.maximum_gap_seconds + 1e-12) ++end;
            PitchActivitySegment region;
            region.midi_pitch = pitch;
            region.frame_indices.assign(indices.begin() + begin, indices.begin() + end);
            region.onset_seconds = track.frames[indices[begin]].time_seconds + center;
            region.offset_seconds = track.frames[indices[end - 1]].time_seconds +
                center + track.hop_seconds;
            segments.push_back(std::move(region));
            begin = end;
        }
    }
    return segments;
}

std::vector<PitchActivitySegment> segment_pitch_activity(
    const FrameEvidenceTrack& track, const TemporalDecodingOptions& options,
    std::span<const OnsetEvidence> onsets) {
    const auto regions = detect_pitch_activity_regions(track, options);
    double previous_onset = -1;
    for (const auto& onset : onsets) {
        if (!std::isfinite(onset.time_seconds) || onset.time_seconds < 0 ||
            onset.time_seconds < previous_onset || !std::isfinite(onset.strength) ||
            onset.strength < 0 || !std::isfinite(onset.energy_rise) || onset.energy_rise < 0 ||
            !std::isfinite(onset.spectral_flux) || onset.spectral_flux < 0 ||
            !std::isfinite(onset.high_frequency_content) || onset.high_frequency_content < 0) {
            throw std::invalid_argument("Invalid onset evidence");
        }
        previous_onset = onset.time_seconds;
    }
    std::vector<PitchActivitySegment> segments;
    const double center = track.window_seconds / 2;
    for (const auto& region : regions) {
        const auto& indices = region.frame_indices;
        bool starts_at_attack = false;
        for (std::size_t begin = 0; begin < indices.size();) {
            std::size_t end = begin + 1;
            const double start = track.frames[indices[begin]].time_seconds + center;
            double boundary = 0;
            double peak_state = track.note_states.empty() ? 0 :
                track.note_states[indices[begin]];
            for (; end < indices.size(); ++end) {
                const auto prior = indices[end - 1], next = indices[end];
                const double left = track.frames[prior].time_seconds + center;
                const double right = track.frames[next].time_seconds + center;
                if (!track.note_states.empty()) {
                    peak_state = std::max(peak_state, track.note_states[prior]);
                }
                if (right - start < std::max(options.minimum_onset_spacing_seconds,
                                             options.minimum_duration_seconds)) continue;
                if (region.offset_seconds - right < options.minimum_duration_seconds) continue;
                bool attack = false;
                if (options.use_model_onsets && !track.onset_strengths.empty()) {
                    const double strength = track.onset_strengths[next];
                    const double before = next ? track.onset_strengths[next - 1] : 0;
                    const double after = next + 1 < track.frames.size() ?
                        track.onset_strengths[next + 1] : 0;
                    double trough = track.note_states.empty() ? 0 : track.note_states[next];
                    if (!track.note_states.empty()) {
                        for (std::size_t i = next; i > indices[begin];) {
                            --i;
                            if (track.frames[next].time_seconds - track.frames[i].time_seconds >
                                std::max({options.minimum_onset_spacing_seconds,
                                          options.minimum_duration_seconds,
                                          track.hop_seconds})) break;
                            trough = std::min(trough, track.note_states[i]);
                        }
                    }
                    attack = strength >= options.onset_threshold && strength > before &&
                        strength >= after &&
                        (track.note_states.empty() ||
                         (peak_state - trough >= options.onset_state_drop &&
                          track.note_states[next] - trough >= options.onset_state_drop / 2)) &&
                        strong(track, next, options.activation_on, options);
                }
                if (!attack && track.method == "yin") {
                    const auto onset = std::upper_bound(onsets.begin(), onsets.end(), left,
                        [](double time, const auto& candidate) {
                            return time < candidate.time_seconds;
                        });
                    attack = onset != onsets.end() && onset->time_seconds <= right &&
                        onset->energy_rise >= options.onset_energy_rise;
                }
                if (attack) { boundary = right; break; }
            }
            PitchActivitySegment segment;
            segment.midi_pitch = region.midi_pitch;
            segment.frame_indices.assign(indices.begin() + begin, indices.begin() + end);
            segment.onset_seconds = start;
            segment.offset_seconds = boundary > 0 ? boundary :
                track.frames[indices[end - 1]].time_seconds + center + track.hop_seconds;
            segment.onset_from_attack = starts_at_attack;
            segment.offset_from_attack = boundary > 0;
            segments.push_back(std::move(segment));
            starts_at_attack = boundary > 0;
            begin = end;
        }
    }
    return segments;
}

EventDiagnostics summarize_event_evidence(const CanonicalNoteEvent& event, double hop_seconds,
                                         double window_seconds) {
    if (!std::isfinite(hop_seconds) || hop_seconds <= 0 ||
        !std::isfinite(window_seconds) || window_seconds < 0 ||
        !std::isfinite(event.onset_seconds) || event.onset_seconds < 0 ||
        !std::isfinite(event.offset_seconds) || event.offset_seconds < event.onset_seconds ||
        !std::isfinite(event.midi_pitch) || event.midi_pitch < 0 || event.midi_pitch > 127) {
        throw std::invalid_argument("Invalid evidence summary geometry");
    }
    EventDiagnostics summary;
    std::size_t stable = 0;
    std::vector<double> times;
    for (const auto& evidence : event.evidence) {
        if (!std::isfinite(evidence.time_seconds) || evidence.time_seconds < 0 ||
            (evidence.onset_strength && !std::isfinite(*evidence.onset_strength)) ||
            (evidence.note_state && !std::isfinite(*evidence.note_state))) {
            throw std::invalid_argument("Nonfinite or negative evidence timestamp");
        }
        times.push_back(evidence.time_seconds + window_seconds / 2);
        if (evidence.midi_pitch == static_cast<int>(std::lround(event.midi_pitch))) ++stable;
        if (evidence.onset_strength) summary.onset_strength =
            std::max(summary.onset_strength.value_or(*evidence.onset_strength), *evidence.onset_strength);
        if (evidence.note_state) summary.maximum_note_state =
            std::max(summary.maximum_note_state.value_or(*evidence.note_state), *evidence.note_state);
    }
    if (!times.empty()) {
        summary.pitch_stability = static_cast<double>(stable) / times.size();
        std::sort(times.begin(), times.end());
        double stop = -1;
        for (const double time : times) {
            const double first = std::max(time, event.onset_seconds);
            const double last = std::min(time + hop_seconds, event.offset_seconds);
            if (last > std::max(first, stop)) {
                summary.activity_duration_seconds += last - std::max(first, stop);
                stop = last;
            }
        }
    }
    return summary;
}

namespace {
DecodedTranscription decode_note_events_impl(const AudioBuffer& audio,
    std::span<const FrameEvidenceTrack> tracks, const TemporalDecodingOptions& options,
    const std::string& audio_source_id, const std::string& source_model, bool continuation) {
    validate(options);
    validate_source(audio, audio_source_id, source_model);
    DecodedTranscription result;
    result.raw_tracks.assign(tracks.begin(), tracks.end());
    const auto regions = energy_regions(audio, options);
    auto onset_evidence = detect_onset_evidence(audio);
    for (auto& onset : onset_evidence) onset.time_seconds += 512.0 / audio.sample_rate;
    std::vector<double> onsets;
    for (const auto& onset : onset_evidence) onsets.push_back(onset.time_seconds);
    const double duration = static_cast<double>(audio.samples.size()) / audio.sample_rate;
    for (const auto& track : tracks) validate_track(track);
    std::vector<std::map<int, std::pair<double, double>>> activity(regions.size());
    for (std::size_t r = 0; r < regions.size(); ++r) {
        for (const auto& track : tracks) {
            for (std::size_t i = 0; i < track.frames.size(); ++i) {
                const auto& f = track.frames[i];
                if (f.midi_pitch < 0 || !strong(track, i, options.activation_on, options) ||
                    f.time_seconds + track.window_seconds + track.hop_seconds <= regions[r].start ||
                    f.time_seconds >= regions[r].end) continue;
                const double start = f.time_seconds + track.window_seconds / 2;
                const double stop = start + track.hop_seconds;
                auto [it, inserted] = activity[r].try_emplace(f.midi_pitch, start, stop);
                if (!inserted) {
                    it->second.first = std::min(it->second.first, start);
                    it->second.second = std::max(it->second.second, stop);
                }
            }
        }
    }
    for (const auto& track : tracks) {
        auto corrected_track = track;
        for (std::size_t i = 0; i < track.frames.size(); ++i) {
            const auto& frame = track.frames[i];
            int pitch = frame.midi_pitch;
            if (options.correct_isolated_octaves && track.method == "yin" &&
                i > 0 && i + 1 < track.frames.size() &&
                track.hop_seconds <= options.maximum_gap_seconds) {
                const auto& left = track.frames[i - 1];
                const auto& right = track.frames[i + 1];
                if (left.midi_pitch >= 0 && left.midi_pitch == right.midi_pitch &&
                    std::abs(pitch - left.midi_pitch) == 12 &&
                    left.confidence >= options.activation_on &&
                    right.confidence >= options.activation_on) {
                    const auto onset = std::lower_bound(onsets.begin(), onsets.end(),
                                                        left.time_seconds + track.window_seconds / 2);
                    if (onset == onsets.end() ||
                        *onset > right.time_seconds + track.window_seconds / 2) {
                        pitch = left.midi_pitch;
                    }
                }
            }
            corrected_track.frames[i].midi_pitch = pitch;
        }
        for (const auto& segment : segment_pitch_activity(corrected_track, options, onset_evidence)) {
                const int pitch = segment.midi_pitch;
                CanonicalNoteEvent event;
                event.source_sample_rate = audio.sample_rate;
                event.audio_source_id = audio_source_id;
                event.source_model = source_model;
                event.detection_method = track.method;
                event.evidence_kind = track.evidence_kind;
                event.midi_pitch = pitch;
                const double center = track.window_seconds / 2;
                double start = segment.onset_seconds;
                double stop = segment.offset_seconds;
                for (const auto index : segment.frame_indices) {
                    const auto& f = track.frames[index];
                    event.evidence.push_back({f.time_seconds, f.midi_pitch, f.confidence,
                        f.voicing_probability, f.fractional_midi,
                        track.raw_values.empty() ? std::nullopt :
                            std::optional<double>(track.raw_values[index]),
                            track.raw_midi_pitch < 0 ? f.midi_pitch : track.raw_midi_pitch,
                            track.selected_strength_semantics,
                            track.raw_value_semantics,
                            track.spectral_peak_fractions.empty() ? std::nullopt :
                                std::optional<double>(track.spectral_peak_fractions[index]),
                            track.onset_strengths.empty() ? std::nullopt :
                                std::optional<double>(track.onset_strengths[index]),
                            track.note_states.empty() ? std::nullopt :
                                std::optional<double>(track.note_states[index])});
                    if (track.onset_strengths.empty()) {
                        const double time = f.time_seconds + center;
                        const auto onset = std::lower_bound(onset_evidence.begin(),
                            onset_evidence.end(), time - track.hop_seconds / 2,
                            [](const auto& candidate, double at) {
                                return candidate.time_seconds < at;
                            });
                        if (onset != onset_evidence.end() &&
                            onset->time_seconds <= time + track.hop_seconds / 2) {
                            event.evidence.back().onset_strength = onset->strength;
                        }
                    }
                }
                std::vector<Region> pieces;
                if (options.refine_with_audio) {
                    for (std::size_t r = 0; r < regions.size(); ++r) {
                        const auto& region = regions[r];
                        if (region.end <= start || region.start >= stop) continue;
                        const double tolerance = center + track.hop_seconds;
                        bool outer_start = true, outer_end = true;
                        // A continuation chunk's left edge is not an observed acoustic onset.
                        if (continuation && region.start == 0) outer_start = false;
                        for (const auto& [other_pitch, bounds] : activity[r]) {
                            if (other_pitch == pitch || bounds.second - bounds.first <
                                options.minimum_duration_seconds) continue;
                            const double guard = 2 * track.hop_seconds;
                            if (bounds.first + guard < start) outer_start = false;
                            if (bounds.second - guard > stop) outer_end = false;
                        }
                        double refined_start = outer_start &&
                            std::abs(region.start - start) <= tolerance ?
                            region.start : std::max(start, region.start);
                        if (region.start < start - tolerance ||
                            (region.start == 0 && !region.strong_at_start &&
                             start > track.hop_seconds)) {
                            const auto onset = std::lower_bound(onsets.begin(), onsets.end(),
                                                                 start - track.hop_seconds);
                            if (onset != onsets.end() && *onset <= start + track.hop_seconds) {
                                refined_start = *onset;
                            }
                        }
                        const double refined_stop =
                            outer_end && std::abs(region.end - stop) <= tolerance ? region.end :
                                                                      std::min(stop, region.end);
                        pieces.push_back({
                            segment.onset_from_attack ? std::max(start, refined_start) : refined_start,
                            segment.offset_from_attack ? std::min(stop, refined_stop) : refined_stop});
                    }
                } else pieces.push_back({start, stop});
                if (pieces.empty()) {
                    set_times(event, start, stop, audio);
                    event.id = event_id(event);
                    event.state = EventLifecycle::Rejected;
                    result.rejected_events.push_back(std::move(event));
                }
                for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
                    auto refined = event;
                    set_times(refined, pieces[piece].start,
                              std::min(duration, pieces[piece].end), audio);
                    refined.id = event_id(refined);
                    std::erase_if(refined.evidence, [&](const auto& evidence) {
                        return evidence.time_seconds + track.window_seconds + track.hop_seconds <=
                            refined.onset_seconds || evidence.time_seconds >= refined.offset_seconds;
                    });
                    refined.state = options.refine_with_audio ? EventLifecycle::Refined :
                                                                 EventLifecycle::Stabilized;
                    if (refined.offset_seconds - refined.onset_seconds <
                            options.minimum_duration_seconds ||
                        !refined.intensity_rms || *refined.intensity_rms < options.energy_floor_rms) {
                        refined.state = EventLifecycle::Rejected;
                        result.rejected_events.push_back(std::move(refined));
                    } else result.events.push_back(std::move(refined));
                }
        }
    }
    std::sort(result.events.begin(), result.events.end(), [](const auto& a, const auto& b) {
        if (a.onset_sample != b.onset_sample) return a.onset_sample < b.onset_sample;
        return a.midi_pitch < b.midi_pitch;
    });
    std::vector<CanonicalNoteEvent> unique;
    std::map<std::pair<double, std::string>, std::size_t> previous;
    for (auto& event : result.events) {
        const auto key = std::make_pair(event.midi_pitch, event.detection_method);
        const auto found = previous.find(key);
        if (found != previous.end() && event.onset_sample < unique[found->second].offset_sample) {
            auto& prior = unique[found->second];
            prior.evidence.insert(prior.evidence.end(), event.evidence.begin(), event.evidence.end());
            set_times(prior, prior.onset_seconds, std::max(prior.offset_seconds, event.offset_seconds), audio);
        } else {
            previous[key] = unique.size();
            unique.push_back(std::move(event));
        }
    }
    result.events = std::move(unique);
    order_evidence(result.events);
    return result;
}
}

DecodedTranscription decode_note_events(const AudioBuffer& audio,
    std::span<const FrameEvidenceTrack> tracks, const TemporalDecodingOptions& options,
    const std::string& audio_source_id, const std::string& source_model) {
    return decode_note_events_impl(audio, tracks, options, audio_source_id, source_model, false);
}

DecodedTranscription transcribe_note_events(const AudioBuffer& audio,
    const AnalysisOptions& analysis, const TemporalDecodingOptions& decoding,
    const std::string& audio_source_id) {
    validate(decoding);
    validate_source(audio, audio_source_id,
                    analysis.polyphonic ? "builtin:spectral" : "builtin:yin");
    std::vector<FrameEvidenceTrack> tracks;
    auto options = analysis;
    options.evidence_callback = [&](const auto& track) {
        tracks.push_back(track);
        if (analysis.evidence_callback) analysis.evidence_callback(track);
    };
    (void)transcribe_monophonic(audio, options);
    return decode_note_events(audio, tracks, decoding, audio_source_id,
                              analysis.polyphonic ? "builtin:spectral" : "builtin:yin");
}

DecodedTranscription transcribe_note_events_with_onnx(const AudioBuffer& audio,
    const std::filesystem::path& model, const OnnxOptions& analysis,
    const TemporalDecodingOptions& decoding, const std::string& audio_source_id,
    const std::string& model_identifier) {
#ifdef TRANSCRIBE_ENABLE_ONNX
    validate(decoding);
    validate_source(audio, audio_source_id, model_identifier);
    std::vector<FrameEvidenceTrack> tracks;
    auto options = analysis;
    options.evidence_callback = [&](const auto& track) {
        tracks.push_back(track);
        if (analysis.evidence_callback) analysis.evidence_callback(track);
    };
    (void)transcribe_with_onnx(audio, model, options);
    return decode_note_events(audio, tracks, decoding, audio_source_id, model_identifier);
#else
    (void)audio; (void)model; (void)analysis; (void)decoding;
    (void)audio_source_id; (void)model_identifier;
    throw std::runtime_error("ONNX support is disabled in this build");
#endif
}

DecodedTranscription transcribe_note_events_wav_streaming(const std::filesystem::path& path,
    const StreamingOptions& streaming, const TemporalDecodingOptions& decoding,
    const std::string& audio_source_id) {
    validate(decoding);
    if (audio_source_id.empty()) throw std::invalid_argument("Empty audio source identifier");
    if (!std::isfinite(streaming.chunk_seconds) || streaming.chunk_seconds < 1 ||
        streaming.chunk_seconds > 300 || !std::isfinite(streaming.overlap_seconds) ||
        streaming.overlap_seconds < .25 || streaming.overlap_seconds >= streaming.chunk_seconds) {
        throw std::invalid_argument("Invalid canonical streaming chunk geometry");
    }
    if (streaming.notes_callback) {
        throw std::invalid_argument("Canonical streaming does not deliver legacy Note callbacks");
    }
    DecodedTranscription result;
    AudioBuffer chunk;
    std::size_t origin = 0, capacity = 0, overlap = 0;
    std::size_t total_samples = 0;
    double progress = 0;
    const auto process = [&] {
        if (chunk.samples.empty()) return;
        auto analysis = streaming.analysis;
        const double shift = static_cast<double>(origin) / chunk.sample_rate;
        if (analysis.progress_callback) {
            analysis.progress_callback = [&](double value) {
                progress = std::max(progress, std::min(1.0,
                    (origin + value * chunk.samples.size()) / std::max<std::size_t>(1, total_samples)));
                streaming.analysis.progress_callback(progress);
            };
        }
        std::vector<FrameEvidenceTrack> tracks;
        analysis.evidence_callback = [&](const auto& track) {
            tracks.push_back(track);
            if (streaming.analysis.evidence_callback) {
                auto global = track;
                for (auto& frame : global.frames) frame.time_seconds += shift;
                streaming.analysis.evidence_callback(global);
            }
        };
        (void)transcribe_monophonic(chunk, analysis);
        auto decoded = decode_note_events_impl(chunk, tracks, decoding, audio_source_id,
            analysis.polyphonic ? "builtin:spectral" : "builtin:yin", origin > 0);
        const auto translate = [&](CanonicalNoteEvent& event) {
            event.onset_sample += origin;
            event.offset_sample += origin;
            event.onset_seconds = static_cast<double>(event.onset_sample) / chunk.sample_rate;
            event.offset_seconds = static_cast<double>(event.offset_sample) / chunk.sample_rate;
            event.id = event_id(event);
            for (auto& evidence : event.evidence) evidence.time_seconds += shift;
        };
        for (auto& event : decoded.events) {
            translate(event);
            auto same = std::find_if(result.events.rbegin(), result.events.rend(),
                [&](const auto& prior) {
                    return prior.midi_pitch == event.midi_pitch &&
                        std::min(prior.offset_seconds, event.offset_seconds) >
                        std::max(prior.onset_seconds, event.onset_seconds);
                });
            if (same != result.events.rend()) {
                same->offset_sample = std::max(same->offset_sample, event.offset_sample);
                same->offset_seconds = static_cast<double>(same->offset_sample) / chunk.sample_rate;
                same->evidence.insert(same->evidence.end(), event.evidence.begin(), event.evidence.end());
                same->intensity_rms.reset();
            } else result.events.push_back(std::move(event));
        }
        for (auto& event : decoded.rejected_events) {
            translate(event);
            result.rejected_events.push_back(std::move(event));
        }
        for (auto& track : decoded.raw_tracks) {
            for (auto& frame : track.frames) frame.time_seconds += shift;
            result.raw_tracks.push_back(std::move(track));
        }
    };
    read_wav_blocks(path, 4096, [&](const AudioBlockView& block) {
        if (chunk.sample_rate == 0) {
            chunk.sample_rate = block.sample_rate;
            total_samples = block.total_frames;
            capacity = static_cast<std::size_t>(std::llround(streaming.chunk_seconds * block.sample_rate));
            overlap = static_cast<std::size_t>(std::llround(streaming.overlap_seconds * block.sample_rate));
            chunk.samples.reserve(capacity);
        }
        if (streaming.audio_callback) streaming.audio_callback(block);
        std::size_t offset = 0;
        while (offset < block.samples.size()) {
            const auto count = std::min(capacity - chunk.samples.size(), block.samples.size() - offset);
            chunk.samples.insert(chunk.samples.end(), block.samples.begin() + offset,
                                 block.samples.begin() + offset + count);
            offset += count;
            if (chunk.samples.size() == capacity) {
                process();
                chunk.samples.erase(chunk.samples.begin(), chunk.samples.end() - overlap);
                origin += capacity - overlap;
            }
        }
    });
    if (chunk.samples.size() > overlap || origin == 0) process();
    if (streaming.analysis.progress_callback) streaming.analysis.progress_callback(1.0);
    std::sort(result.events.begin(), result.events.end(), [](const auto& a, const auto& b) {
        if (a.onset_sample != b.onset_sample) return a.onset_sample < b.onset_sample;
        return a.midi_pitch < b.midi_pitch;
    });
    order_evidence(result.events);
    return result;
}
}
