# Play Mode — Phase 1 Implementation Plan (Claude Code)

## Context

Tennis monitoring wearable on a 2" / 500×400 display, sensor QMI8658. Test mode and hit detection are already coded and working. This plan adds **Play mode**, reached from the PLAY button on the main screen. Play mode reuses the existing hit detector and Test-mode file-logging code, and adds a play/pause state plus live point-outcome tagging.

**Do not modify** the existing hit-detection logic or Test-mode internals — wrap and reuse them.

Target hardware reminder: all text large and high-contrast; favor a few big elements over many small ones. Scale fonts relative to the 500×400 screen.

------

## 1. Screen layout

- A **status circle** centered on the screen, showing the current mode as text: `PAUSE` or `PLAY`. On entering Play mode the state starts at **PAUSE**.
- A **pentagon surrounding the circle**, divided into 5 equal tappable slices. Each slice maps to one point outcome:
  1. Good hit
  2. Out
  3. Unforced
  4. First serve
  5. Lost point
- Each slice shows its label and ideally its live count. Slices must be visually distinct (different colors) and large enough to tap reliably on the small screen.
- Provide a back-to-Main control consistent with the Test-mode screen.

------

## 2. Play / Pause control (GPIO 10)

- A physical button on **GPIO 10** toggles between PLAY and PAUSE. Wire a **debounced** interrupt/handler on GPIO 10.
- State starts at **PAUSE** when Play mode opens.
- The center circle's text reflects the current state and updates immediately on toggle.
- State gating:
  - **PLAY**: hit detection is active (data appended to the hit file) and slice taps increment counters.
  - **PAUSE**: hit detection and counter tagging are suspended. Pause does NOT finalize or close anything — it only halts (see section 5).

------

## 3. Session folder and files

When the user enters Play mode, create **one folder for the whole session**:

- Folder name: `PlaySession_<CurrentDateAndHour>`
  - Use a filesystem-safe format: `YYYY-MM-DD_HH-MM` (e.g. `PlaySession_2026-06-13_14-32`). No `:` or other characters that some filesystems reject.
- Both session files live inside that folder:

**File A — hit-detection log**

- One file for the entire session.
- Same format and logic as the Test-mode recording file (raw hit events / sensor data). Reuse the existing Test-mode logging code path; do not rewrite the detector.
- Appended to continuously while in PLAY. Do **not** create a new hit file on each pause/resume.

**File B — outcome counters**

- Holds the 5 counters: good hit, out, unforced, first serve, lost point.
- Created (or its path reserved) on entering Play mode, but written only at session end — see section 5.

Keep the session-number / naming scheme consistent with Test mode where it applies, so logs stay cross-referenceable.

------

## 4. Counter behavior

- Keep the 5 counters **in memory** during the session.
- While in **PLAY**, each slice tap increments its counter in memory and updates the on-screen count immediately.
- Do **not** write File B on every tap. File B is written **once, at session end**, with the final counter values (see section 5).
- File A (hit log) is written continuously during PLAY.

------

## 5. Session folder, files, and end-of-session rules

### Single hit-detection file

Keep just one hit-detection file (File A) for the entire session, appended continuously while in PLAY. No new hit file on pause/resume.

### End-of-session — three triggers, any one closes the session

The session ends, and at that moment **both files are finalized and closed**, when **any** of these occur:

1. The user **returns to the main page**.
2. Battery drops to **3% or less**.
3. **No activity for more than 30 minutes** — "activity" is defined explicitly as: no hit detected **AND** no slice tap **AND** no GPIO 10 press for a continuous 30-minute window. Use a timer that resets on any of those three events.

### On any trigger

- Flush the in-memory 5 counters to **File B** (write the final values once).
- Ensure **File A** is fully written and closed (flush any buffered hit data).
- Close both file handles cleanly so nothing is left open or corrupted.
- For the **low-battery** and **inactivity** triggers, also move the UI back to the main screen (or a safe idle state) after closing the files, so the device isn't left in an active Play state with closed files.

### Implementation notes

- Make session-close a **single function** (e.g. `endPlaySession()`) that all three triggers call, so finalization logic lives in exactly one place and can't diverge.
- **Guard it to run only once per session** even if two triggers fire close together (e.g. user hits back just as battery reaches 3%). Use a flag.
- The **inactivity timer and battery check run regardless of play/pause state** — a paused session left untouched for 30 min, or one that hits 3% while paused, must still close and finalize.

> Note: Pause does not finalize anything. Only these three triggers close the session and write File B. This is intentional and supersedes any earlier pause-finalizes behavior.

------

## 6. Constraints / reuse

- Reuse the existing **hit detector** and **Test-mode file-logging** code unchanged. Play mode wraps them with the play/pause gate and the outcome-tagging UI.
- Keep the session-naming scheme consistent with Test mode so files and any on-screen identifiers match.
- Scale fonts to the 500×400 screen; the center status text and slice labels must be readable at 2".

------

## 7. Scope boundary (Phase 1)

In scope: the Play-mode screen (center status circle + 5-slice pentagon), the GPIO 10 play/pause toggle, per-game session folder, the two files, in-memory counters, and finalize-on-end via the three triggers.

Out of scope (later phases): stats/summary screens, scoring logic, analysis of the recorded data, stroke classification.