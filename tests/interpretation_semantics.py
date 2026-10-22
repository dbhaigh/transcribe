"""Compare interpreted/exported timelines with hand-annotated reference passages.

These are semantic assertions, not XML well-formedness or audio-accuracy tests.
Only the Python standard library and the repository's notation test runner are used.
"""
import argparse
import hashlib
import json
import math
from fractions import Fraction as F
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def parse_part(part, meter, pickup):
    divisions = 1
    measure_start = F(0)
    notes, rests, tempos = [], [], []
    for measure_index, measure in enumerate(part.findall("measure")):
        cursor, previous_start = F(0), F(0)
        open_beams, open_tuplets = set(), set()
        bar = F(str(pickup)) if measure_index == 0 and pickup else F(meter[0] * 4, meter[1])
        covered = {}
        for item in measure:
            if item.tag == "attributes":
                divisions = int(item.findtext("divisions", str(divisions)))
            elif item.tag == "backup":
                cursor -= F(int(item.findtext("duration")), divisions)
                require(cursor >= 0, "backup precedes the measure")
            elif item.tag == "direction" and item.find("sound") is not None:
                sound = item.find("sound")
                if sound.get("tempo") is not None:
                    tempos.append((measure_start + cursor + F(int(item.findtext("offset", "0")), divisions),
                                   float(sound.get("tempo"))))
            elif item.tag == "note":
                duration = F(int(item.findtext("duration")), divisions)
                chord = item.find("chord") is not None
                start = previous_start if chord else cursor
                voice = int(item.findtext("voice"))
                for beam in item.findall("beam"):
                    key = voice, int(beam.get("number"))
                    if beam.text == "begin":
                        require(key not in open_beams, "beam begins twice")
                        open_beams.add(key)
                    elif beam.text in ("continue", "end"):
                        require(key in open_beams, "beam continuation/end without begin")
                        if beam.text == "end":
                            open_beams.remove(key)
                    else:
                        require(beam.text in ("forward hook", "backward hook") and key[1] > 1,
                                "invalid beam hook")
                for tuplet in item.findall("notations/tuplet"):
                    key = voice, tuplet.get("number")
                    if tuplet.get("type") == "start":
                        require(key not in open_tuplets, "tuplet begins twice")
                        open_tuplets.add(key)
                    else:
                        require(tuplet.get("type") == "stop" and key in open_tuplets,
                                "tuplet stop without begin")
                        open_tuplets.remove(key)
                require(start >= 0 and start + duration <= bar, "note/rest crosses an unsplit barline")
                if not chord:
                    covered[voice] = covered.get(voice, F(0)) + duration
                    previous_start = start
                    cursor += duration
                pitch = item.find("pitch")
                record = dict(onset=measure_start + start, duration=duration, voice=voice,
                              element=item, divisions=divisions)
                if pitch is None:
                    require(item.find("rest") is not None, "note has no pitch or rest")
                    rests.append(record)
                else:
                    step = pitch.findtext("step")
                    alter, octave = int(pitch.findtext("alter", "0")), int(pitch.findtext("octave"))
                    record["midi"] = (octave + 1) * 12 + dict(C=0, D=2, E=4, F=5, G=7, A=9, B=11)[step] + alter
                    record["spelling"] = [step, alter, octave]
                    notes.append(record)
                note_type = item.findtext("type")
                if note_type:
                    denominator = {"whole": 1, "half": 2, "quarter": 4, "eighth": 8,
                                   "16th": 16, "32nd": 32, "64th": 64, "128th": 128}[note_type]
                    dots = len(item.findall("dot"))
                    symbolic = F(4, denominator) * sum((F(1, 2**i) for i in range(dots + 1)), F(0))
                    actual = int(item.findtext("time-modification/actual-notes", "1"))
                    normal = int(item.findtext("time-modification/normal-notes", "1"))
                    symbolic *= F(normal, actual)
                    require(abs(symbolic - duration) <= F(1, 2 * divisions),
                            "symbolic type/dots/tuplet disagrees with duration")
        require(cursor == bar, f"measure {measure_index + 1} ends at {cursor}, expected {bar}")
        require(not open_beams and not open_tuplets, "beam/tuplet left open at barline")
        require(all(value == bar for value in covered.values()), "voice does not fill its measure")
        measure_start += bar
    # Reconstruct sounding events from ties rather than counting XML fragments as notes.
    merged, active = [], {}
    for note in sorted(notes, key=lambda n: (n["onset"], n["voice"], n["midi"])):
        ties = {t.get("type") for t in note["element"].findall("tie")}
        notated = {t.get("type") for t in note["element"].findall("notations/tied")}
        require(ties == notated, "playback ties differ from visual ties")
        key = note["voice"], note["midi"]
        if "stop" in ties:
            require(key in active, "tie stop without a matching start")
            previous = active[key]
            require(previous["onset"] + previous["duration"] == note["onset"], "noncontiguous tie")
            previous["duration"] += note["duration"]
        else:
            previous = dict(note)
            merged.append(previous)
        if "start" in ties:
            active[key] = previous
        else:
            active.pop(key, None)
    require(not active, "unclosed ties")
    return merged, notes, rests, tempos


def generated_references():
    cases = []
    for actual, normal, count in [(3, 2, 9), (5, 4, 10), (7, 4, 14)]:
        q = F(normal, 2 * actual)  # eighth-note tuplets
        cases.append(dict(id=f"tuplet-{actual}-{normal}", tempo=120, meter=[4, 4],
                          fifths=0, mode="evidence", ratio=[actual, normal],
                          notes=[[60 + i % 5, float(i*q/2), float((i+1)*q/2),
                                  float(i*q), float(q), 1] for i in range(count)]))
    cases.append(dict(id="partial-triplet", tempo=120, meter=[4, 4], fifths=0,
                      mode="legacy", ratio=[3, 2], exporter_fixture="partial-tuplet",
                      notes=[[60+i, i/6, (i+1)/6, i/3, 1/3, 1] for i in range(2)]))
    onsets = [0, .5, 1, 1.5, 2, 2+2/3, 2+4/3, 4, 4+2/3, 4+4/3]
    cases.append(dict(id="inferred-tempo-change", tempo=0, meter=[4, 4], fifths=0, mode="expressive",
                      notes=[[60+i, t, t+.3, i, .6 if i < 4 else .45, 1]
                             for i, t in enumerate(onsets)],
                      tempos=[[0, 120], [4, 90]], expressive=True))
    cases.append(dict(id="mid-note-tempo-direction", tempo=120, meter=[4, 4], fifths=0,
                      mode="legacy", notes=[[60, 0, 2, 0, 4, 1]],
                      exporter_fixture="mid-note-tempo", tempos=[[0, 120], [1.5, 90]]))
    cases.append(dict(id="ambiguous-sparse", tempo=120, meter=[4, 4], fifths=0,
                      mode="evidence", notes=[[60, .013, .489, .026, .952, 1]],
                      expressive=True))
    cases.append(dict(id="partial-strength", tempo=120, meter=[4, 4], fifths=0,
                      mode="legacy", strength=.5,
                      notes=[[60, .013, .489, .013, .976, 1]], expressive=True))
    cases.append(dict(id="jittered-quarter-passage", tempo=120, meter=[4, 4], fifths=0,
                      mode="evidence", notes=[[60+i, i*.5+.005, (i+1)*.5+.005, i, 1, 1]
                                              for i in range(8)]))
    cases.append(dict(id="ambiguous-rolled-chord", tempo=120, meter=[4, 4], fifths=0,
                      mode="evidence", expressive=True,
                      notes=[[60, .01, .51, .02, 1, 1], [64, .03, .53, .06, 1, 2]]))
    cases.append(dict(id="edited-pitch-fallback-spelling", tempo=120, meter=[4, 4], fifths=-1,
                      mode="legacy", exporter_fixture="unspelled",
                      notes=[[70, 0, .5, 0, 1, 1]], spellings=[["B", -1, 4]]))
    cases.append(dict(id="double-accidental-spelling", tempo=120, meter=[4, 4], fifths=0,
                      mode="legacy", exporter_fixture="double-accidentals",
                      notes=[[67, 0, .5, 0, 1, 1], [69, .5, 1, 1, 1, 1]],
                      spellings=[["F", 2, 4], ["B", -2, 4]],
                      accidentals=["double-sharp", "flat-flat"]))
    cases.append(dict(id="irregular-rest-decomposition", tempo=120, meter=[4, 4], fifths=0,
                      mode="legacy", notes=[[60, 1.25, 1.5, 2.5, .5, 1]],
                      symbolic_rests=True))
    cases.append(dict(id="independent-release-voices", tempo=120, meter=[4, 4], fifths=0,
                      mode="legacy",
                      notes=[[48, 0, 2, 0, 4, 1], [60, 0, .5, 0, 1, 2],
                             [62, .5, 1, 1, 1, 2], [64, 1, 1.5, 2, 1, 2],
                             [65, 1.5, 2, 3, 1, 2]]))
    cases.append(dict(id="guitar-written-octave", tempo=120, meter=[4, 4], fifths=0,
                      mode="legacy", exporter_fixture="guitar-written-octave",
                      written_transposition=12, notes=[[40, 0, .5, 0, 1, 1],
                                                      [64, .5, 1, 1, 1, 1]]))
    for meter, count, starts in [([4, 4], 8, 4), ([6, 8], 6, 2)]:
        cases.append(dict(id=f"beam-groups-{meter[0]}-{meter[1]}", tempo=120,
                          meter=meter, fifths=0, mode="legacy", beam_starts=starts,
                          notes=[[60+i % 5, i*.25, (i+1)*.25, i*.5, .5, 1]
                                 for i in range(count)]))
    return cases


def render_case(runner, directory, case):
    prefix = directory / case["id"]
    header = [case["tempo"], *case["meter"], case["fifths"], case["mode"],
              case.get("strength", 1), .65, case.get("pickup", 0)]
    payload = " ".join(map(str, header)) + "\n"
    payload += "".join(" ".join(map(str, note[:3])) + "\n" for note in case["notes"])
    command = [str(runner), str(prefix)]
    if case.get("exporter_fixture"):
        command.append(case["exporter_fixture"])
    subprocess.run(command, input=payload, text=True, capture_output=True, check=True)
    return json.loads(prefix.with_suffix(".json").read_text()), prefix


def check_case(runner, directory, case):
    score, prefix = render_case(runner, directory, case)
    require(score["key"]["user_constrained"] and score["meter"]["user_constrained"],
            "explicit context lost override provenance")
    require(all(segment["user_constrained"] == bool(case["tempo"]) for segment in score["tempo_map"]),
            "tempo inference and user override provenance were conflated")
    for source, reference in zip(score["source_events"], case["notes"]):
        require(abs(source["onset_seconds"] - reference[1]) < 1e-9 and
                abs(source["offset_seconds"] - reference[2]) < 1e-9,
                "notation changed measured detector timing")
    for suffix in (".musicxml", ".roundtrip.musicxml"):
        root = ET.parse(str(prefix) + suffix).getroot()
        for part in root.findall("part"):
            bar = case["meter"][0]*4/case["meter"][1]
            pickup = case.get("pickup", 0)
            reference_end = max(n[3]+n[4] for n in case["notes"])
            measures = max(1, math.ceil(max(0, reference_end-pickup-1e-8)/bar) + bool(pickup))
            require(len(part.findall("measure")) == measures, f"{case['id']}: wrong bar count")
            merged, fragments, rests, tempos = parse_part(part, case["meter"], case.get("pickup", 0))
            require(part.find("measure[last()]/barline/bar-style").text == "light-heavy",
                    "missing final barline")
            if case.get("symbolic_rests"):
                require(all(r["element"].find("type") is not None for r in rests),
                        "representable rest lacks a written duration")
            if case.get("beam_starts"):
                require(len(part.findall(".//beam[@number='1'][.='begin']")) == case["beam_starts"],
                        "beam groups differ from meter")
            expected = sorted(case["notes"], key=lambda n: (n[3], n[5], n[0]))
            require(len(merged) == len(expected), f"{case['id']}: wrong event count after tying")
            for actual, reference in zip(merged, expected):
                midi, _, _, onset, duration, voice = reference
                if part.get("id") == "P1":
                    midi += case.get("written_transposition", 0)
                require(actual["midi"] == midi and actual["voice"] == voice, f"{case['id']}: pitch/voice mismatch")
                tolerance = F(1, actual["divisions"])
                require(abs(actual["onset"] - F(str(onset))) <= tolerance,
                        f"{case['id']}: wrong onset {actual['onset']} != {onset}")
                require(abs(actual["duration"] - F(str(duration))) <= tolerance,
                        f"{case['id']}: wrong duration {actual['duration']} != {duration}")
                for rest in rests:
                    if rest["voice"] == voice:
                        require(rest["onset"] >= actual["onset"] + actual["duration"] or
                                rest["onset"] + rest["duration"] <= actual["onset"],
                                "rest overlaps sounding note in the same voice")
            first = part.find("measure/attributes")
            if case.get("written_transposition") and part.get("id") == "P1":
                require(int(first.findtext("transpose/chromatic")) == -case["written_transposition"],
                        "guitar written pitch lost sounding transposition")
                require(int(first.findtext("transpose/diatonic")) == -7,
                        "guitar octave transposition lost diatonic interval")
            require([int(first.findtext("time/beats")), int(first.findtext("time/beat-type"))] == case["meter"],
                    "meter differs from reference")
            require(int(first.findtext("key/fifths")) == case["fifths"], "key differs from reference")
            for actual, reference in zip(tempos, case.get("tempos", [[0, case["tempo"]]])):
                require(abs(float(actual[0])-reference[0]) < .001 and abs(actual[1]-reference[1]) < .1,
                        "tempo direction has incorrect score position or BPM")
            require(len(tempos) == len(case.get("tempos", [[0, case["tempo"]]])), "missing/extra tempo directions")
            if part.get("id") == "P1":
                if case.get("accidentals"):
                    require([n["element"].findtext("accidental") for n in fragments] == case["accidentals"],
                            "accidental repeat/cancellation/bar reset differs from reference")
                if case.get("spellings"):
                    require([n["spelling"] for n in fragments] == case["spellings"], "wrong enharmonic spelling")
                if case.get("dots"):
                    require(any(n["element"].find("dot") is not None for n in fragments), "missing dotted note")
                if case.get("ties"):
                    require(len(fragments) > len(merged), "expected barline tie")
                if case.get("chord"):
                    require(sum(n["element"].find("chord") is not None for n in fragments) == 2, "wrong chord members")
                if case.get("ratio"):
                    require(sum(n["element"].find("notations/tuplet[@type='start']") is not None
                                for n in fragments) ==
                            math.ceil(len(fragments) / case["ratio"][0]),
                            f"{case['id']}: uniform tuplets lack bracket boundaries")
                    for note in fragments:
                        element = note["element"]
                        require([int(element.findtext("time-modification/actual-notes", "0")),
                                 int(element.findtext("time-modification/normal-notes", "0"))] == case["ratio"],
                                "wrong tuplet ratio")
                if case.get("expressive"):
                    require(all(n["element"].find("type") is None for n in fragments),
                            "unquantized evidence forced to a symbolic type")
    return score


def reference_errors(score, case):
    by_source = {source: note for note in score["notes"] for source in note["source_event_ids"]}
    onset, duration, voices = [], [], 0
    for index, reference in enumerate(case["notes"]):
        note = by_source[f"reference-{index}"]
        onset.append(abs(note["onset_quarters"] - reference[3]))
        duration.append(abs(note["duration_quarters"] - reference[4]))
        voices += note["voice"] != reference[5]
    return {"onset_mae_quarters": sum(onset)/len(onset),
            "duration_mae_quarters": sum(duration)/len(duration),
            "onset_max_error_quarters": max(onset),
            "duration_max_error_quarters": max(duration),
            "voice_mismatches": voices}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runner", type=Path)
    parser.add_argument("fixtures", type=Path)
    parser.add_argument("--output-json", type=Path)
    parser.add_argument("--output-markdown", type=Path)
    args = parser.parse_args()
    runner, fixtures = args.runner.resolve(), args.fixtures
    cases = json.loads(fixtures.read_text())["passages"] + generated_references()
    rows = []
    with tempfile.TemporaryDirectory(prefix="transcriber-semantics-") as temporary:
        directory = Path(temporary)
        for case in cases:
            score = check_case(runner, directory, case)
            # The legacy control receives identical measured notes and user context.
            legacy = dict(case, id=case["id"]+"-legacy", mode="legacy", strength=1)
            baseline, _ = render_case(runner, directory, legacy)
            rows.append(dict(passage=case["id"], mode=case["mode"], notes=len(case["notes"]),
                             legacy=reference_errors(baseline, case), selected=reference_errors(score, case)))
        invalid = dict(id="unequal-chord-rejected", tempo=120, meter=[4, 4], fifths=0,
                       mode="legacy", exporter_fixture="unequal-chord",
                       notes=[[60, 0, .5], [67, 0, 1]])
        try:
            render_case(runner, directory, invalid)
        except subprocess.CalledProcessError as error:
            require("require independent voices" in error.stderr,
                    "unequal chord failure lacks actionable explanation")
            require(not (directory / "unequal-chord-rejected.musicxml").exists(),
                    "rejected chord left a partial output file")
        else:
            raise AssertionError("unequal-duration same-voice chord was silently exported")
    report = dict(schema_version=1, provenance=json.loads(fixtures.read_text())["provenance"],
                  scope="Notation-only hand-authored references; no audio detection or real-score corpus accuracy claim",
                  runner_sha256=hashlib.sha256(runner.read_bytes()).hexdigest(),
                  fixtures_sha256=hashlib.sha256(fixtures.read_bytes()).hexdigest(),
                  test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  passages=rows)
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    lines = ["# Annotated notation comparisons", "", report["scope"], "",
             "Errors are in quarter notes, separately reported per passage. Legacy uses the current build, "
             "not a historical pre-change binary. Selected-mode exports also pass semantic assertions.", "",
             "| Passage | Mode | Legacy onset MAE | Selected onset MAE | Legacy duration MAE | Selected duration MAE | Voice mismatches legacy / selected |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        a, b = row["legacy"], row["selected"]
        lines.append(f"| {row['passage']} | {row['mode']} | {a['onset_mae_quarters']:.6f} | "
                     f"{b['onset_mae_quarters']:.6f} | {a['duration_mae_quarters']:.6f} | "
                     f"{b['duration_mae_quarters']:.6f} | {a['voice_mismatches']} / {b['voice_mismatches']} |")
    text = "\n".join(lines)+"\n"
    if args.output_markdown:
        args.output_markdown.parent.mkdir(parents=True, exist_ok=True)
        args.output_markdown.write_text(text, encoding="utf-8")
    print(f"{len(cases)} annotated passages: pitch/onset/duration/voice/rest/tie/tuplet/context semantics passed "
          "(direct and canonical round-trip exports, both parts)")
    print(text)


if __name__ == "__main__":
    main()
