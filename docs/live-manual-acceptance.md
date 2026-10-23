# Live ASIO manual acceptance

Hardware results: **Not yet validated**.

Use this checklist with an ASIO interface attached to a Windows workstation
build configured with `TRANSCRIBE_ENABLE_ASIO=ON`,
`TRANSCRIBE_ENABLE_WEBVIEW2=ON`, and `TRANSCRIBE_BUILD_TESTS=ON`. Record the
driver/device and workstation build before testing. Do not treat the
hardware-independent callback replay as a substitute for these checks.

## Test record

- Tester: ______________________________
- Date: ________________________________
- Workstation build/commit: ______________
- ASIO driver and version: _______________
- Interface/device: _____________________
- Device firmware: ______________________
- Overall result: ________________________

## Checklist

For each step, record **Pass**, **Fail**, or **Not run**, and include observed
details. Attach logs or screenshots when useful.

| # | Check | Result | Notes |
|---|---|---|---|
| 1 | Launch the workstation with the interface connected. Confirm the intended ASIO device appears by name and can be selected. | __________ | ______________________________ |
| 2 | Select a supported sample rate and input channel count. Start capture and confirm the selected format is used; an unsupported format must fail explicitly rather than silently selecting another one. | __________ | ______________________________ |
| 3 | Play a slow, clean scale into the selected input. Confirm provisional notes appear, stabilize in place, and finalize after Stop. Check notation, tablature and timeline stay aligned while following live. | __________ | ______________________________ |
| 4 | While playing, unplug or disable the interface. Confirm capture enters the interrupted/reconnecting state, reports the input gap, and does not claim missing audio was captured. | __________ | ______________________________ |
| 5 | Reconnect or re-enable the same device within the retry window. Confirm capture resumes or reports a clear failure after the retry limit, and that notes before and after the interruption are not silently joined across the gap. | __________ | ______________________________ |
| 6 | Change the device buffer size, then start a new capture. Confirm the negotiated buffer period and driver latency are reported accurately, or latency is shown as unknown when the driver does not report it. | __________ | ______________________________ |
| 7 | Stop a capture with a note still sounding. Confirm queued audio drains, the note receives finalization, and the capture status becomes Finished. | __________ | ______________________________ |
| 8 | Run **Refine recording**. Confirm a proposal is presented for review and the live/accepted score is not replaced without approval. | __________ | ______________________________ |
| 9 | Accept appropriate proposal changes and export MusicXML, tablature, MIDI and JSON. Confirm exports reflect the accepted document and the original live event stream remains available. | __________ | ______________________________ |
| 10 | Cancel a separate capture. Confirm it is discarded without finalized-note claims and the previously accepted score is restored. | __________ | ______________________________ |

## Defects and follow-up

- Defect/issue references: ________________________________________________
- Logs/screenshots location: _____________________________________________
- Retest date and result: ________________________________________________
