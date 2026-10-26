# Changelog

## 3.2.6 - 2026-08-11

- Added a permanent `HH:MM` clock to the top status bar; normal user operations refresh it through the existing e-paper partial-update path.
- Synchronize China local time from SNTP whenever Wi-Fi connects, write it back to the PCF8563 RTC, and re-evaluate local-deck daily progress immediately after correction.
- Preserve the last known natural day when RTC time is invalid instead of inventing a rolling 24-hour day from boot uptime, and stop factory RTC tests from leaving their fixed test timestamp behind.
- Keep transient button feedback in one stable top-right position instead of moving between cards and screens.
- Compact dense Maimemo meanings horizontally while preserving the normal footer whenever the text still fits.

## 3.2.5 - 2026-08-10

- Reset local-deck daily progress and rebuild the day's review queue when the RTC date changes, including after light-sleep wake, button entry, and periodic UI refresh, without clearing long-term per-word review state.

## 3.2.4 - 2026-08-10

- Added a one-time welcome page for genuinely erased/new devices while silently migrating existing users during firmware updates.
- Kept the system-upgrade popup visible throughout OTA, added a percentage badge and progress bar, and synchronized LVGL rendering before throttled e-paper partial refreshes.
- Delayed the final reboot long enough for the completed OTA state to become physically visible on the e-paper display.

## 3.2.3 - 2026-08-10

- Replaced the sleeping pixel-dog screen with the last useful page plus a compact moon and wake hint.
- Added persistent automatic-sleep presets for 30 minutes, 1 hour, 3 hours, 6 hours, or never.

## 3.2.2 - 2026-08-10

- Kept Maimemo-scheduled reviews on the normal word-front flow even when the word has a prior result; only long Up/Down history browsing now opens the meaning and shows the prior familiar/vague/forgot marker.

## 3.2.1 - 2026-08-10

- Unified local-deck study controls with Maimemo: short Up/Confirm/Down submit Forget/Familiar/Vague after revealing the answer, while long Up/Down navigate without scoring.

## 3.2.0 - 2026-08-10

- Added a sleeping pixel-dog scene before the device enters light sleep.
- Made browser-imported local decks coexist with indexed built-in decks instead of disappearing from the device list.
- Stored uploaded decks under short stable filenames so valid browser imports cannot exceed the SPIFFS filename limit.
- Mounted the assets partition before starting USB management so compatibility checks no longer observe a false startup failure.
- Added the matching browser-side old-partition warning and one-time forced full-flash requirement for devices that cannot support custom deck imports through OTA alone.

## 3.1 - 2026-08-09

- Removed the device-direct AnkiWeb account, sync, collection, and review-journal feature while retaining generic local-deck upload and selection.
- Switched the Maimemo daily-task snapshot to the current official `/memo/study` Open API route, with a 404-only fallback to the legacy route.
- Deferred pending-answer flash persistence until the WebStudy request finishes so SPIFFS garbage collection no longer competes with the latency-sensitive submit round trip.
- Avoided rewriting an empty pending-answer file after every successful online word and added INFO-level timing for WebStudy round trips, extra next-word fetches, audio cancellation, and card publication.
- Prevented pronunciation preloading from blocking Maimemo review submission, and showed the syncing badge immediately after an answer.
- Reset half-open Maimemo WebSocket sessions after request timeouts and automatically retried the read-only next-word request on a fresh connection.
- Remapped Maimemo controls so short up/down submit forgot/vague and long up/down navigate to the previous/next word.
- Remembered each Maimemo review result in a compact persistent journal; navigating back to a reviewed word now shows its meaning and a borderless familiar/vague/forgot marker at the top right, separate from bottom-right action feedback.
- Increased the one-pixel outer frame radius so its rounded corners remain clearly visible on the e-paper display.

## 2.70 - 2026-07-21

- Restored fast Maimemo word transitions by publishing the next card before persistence cleanup and moving offline snapshot and pending-answer writes to a coalescing background worker.
- Submitted WebStudy answers immediately and only applied the server timing delay after an explicit `study duration illegal` rejection.
- Bypassed automatic-pronunciation work completely when it is disabled, while preserving manual pronunciation and safely interrupting audio when leaving a card.
- Added per-transition network, publication, persistence, and automatic-pronunciation diagnostics for device-side latency verification.

## 2.69.2 - 2026-07-20

- Fixed the Maimemo rapid-confirm panic by keeping all e-paper updates on the single UI task instead of a fallback button worker.
- Showed the “正在同步” state through a small top-right badge refresh, avoiding a full word-card redraw while the next word is loading.
- Kept automatic online pronunciation enabled while serializing its network work ahead of WebStudy submission.

## 2.69.1 - 2026-07-20

- Fixed a Wi-Fi reconnect self-deadlock that could freeze vocabulary input after the first word change.
- Kept scan event notifications outside the Wi-Fi manager mutex so repeated button input remains responsive while reconnecting.

## 2.69 - 2026-07-20

- Added transactional USB deck upload commands for browser-imported Anki and custom vocabulary decks.
- Added a downloadable UTF-8 CSV template with Chinese headers and first-use guidance for user-created local decks.
- Enforced deck ID, size, offset, base64, SHA-256, storage-capacity, metadata, and five-column TSV validation before activation.
- Kept interrupted or invalid uploads isolated in temporary files and preserved the previously installed deck during replacement failures.

## 2.68 - 2026-07-19

- Persisted Maimemo answers before submission and replayed them after reconnect only when the server study position still matches.
- Added a Maimemo Open API token setting, daily-task offline snapshots, cached-card browsing, and explicit conflict isolation for divergent progress.
- Initialized the WebStudy session before requesting the public daily-task API, so the device can prepare today's task without requiring the App to be opened first.
- Mapped the Open API snapshot identity to the WebStudy submission identity by uniquely matched spelling, and omitted stale timing data when replaying offline answers in a new session.

## 2.67 - 2026-07-19

- Improved hotspot recovery with immediate rescanning, stale fast-connect cache invalidation, and shorter retry backoff.
- Queued button input while vocabulary state is busy instead of silently dropping user actions.
- Preserved Maimemo actions across reconnects, surfaced queued-operation status, and shortened stalled network retries.
- Preferred local pronunciation assets before network fetches, reduced online timeouts, and added a bounded multi-entry audio cache.
- Kept routine card and settings transitions on partial e-paper refreshes to avoid unnecessary black full-refresh flashes.
- Added Hangul-aware vocabulary rendering, Korean deck generation/release tooling, and support for extended deck rows with empty pronunciation fields.

## 2.65 - 2026-07-16

- Changed automatic light sleep to start after 24 hours without user activity.
- Added stable word-key progress persistence with legacy migration and deck-bound recovery copies.
- Added transactional online deck/index replacement and delayed OTA health confirmation.
- Restored the user-facing American/British pronunciation selector.
- Added runtime reset, heap, uptime, and idle-sleep diagnostics to device management status.
- Prepared the firmware project for public GitHub release as `Egga Word Machine / Egga 单词机`.
- Added source-focused ignore rules for ESP-IDF build output, release artifacts, local config, and private deployment files.
- Added board release metadata for `egga-s3-epaper-4.2`.
