# Privacy: user experience improvement program

[中文](privacy.md)

Zhiyi IME's user experience improvement program has two tiers. **Both are off by default**; nothing
is recorded unless you check them in the installer or in Settings > Privacy.

| Tier | Option | Setting (`%USERPROFILE%\zhiyi\default.json`) |
|---|---|---|
| 1 | Join the user experience improvement program | `privacy.experience_program` |
| 2 | Allow collecting your input | `privacy.collect_input` |

- Tier 2 needs tier 1: checking tier 2 asks for confirmation and checks tier 1 too; unchecking tier
  1 unchecks tier 2.
- **Everything stays on your computer and is never uploaded.** Others see it only if you send the
  files yourself (for example, a `collect_diagnostics.ps1 -IncludeLogs` bundle sent to the
  developers).
- Unchecking stops recording at once and keeps the existing records; "Delete all records" in
  Settings > Privacy deletes the records of both tiers.
- The records are written by the background service `zhiyi-server.exe`; see
  `engine/src/experience_log.cc`.

## Tier 1: basic information

File: `%USERPROFILE%\zhiyi\logs\experience.jsonl`, one JSON record per line; at 1 MB it is renamed
to `experience.1.jsonl` (only that one old file is kept). Every record has `t` (UTC time) and
`event` (the record type).

| Record | Written | Contents |
|---|---|---|
| `start` | when the background service starts | `version` IME version; `windows` Windows version number; `cpu_count` logical CPUs; `memory_gb` memory size (GB) |
| `config` | when first enabled and when settings change | `settings`: Chinese input (pinyin / Wubi), pinyin scheme, initials mode, fuzzy pinyin and its pairs, self-learning, candidates per page, font size, theme, interface language, recommendation model on/off, English spelling correction, English word mode, renderer, horizontal / vertical layout, whether input collection is allowed, the update reminder switch, the 4 switch keys and the tap Shift / Ctrl setting |
| `health` | checked every 30 minutes, written only after errors or when the recommendation model state changes | `errors` responses that could not be built (the key then reaches the program unhandled); `laya` recommendation model state (off / ready / failed) |

Tier 1 does **not** include anything you type, any keys, the programs you use, or usage counts.

## Tier 2: input

Files: `%USERPROFILE%\zhiyi\logs\input-YYYYMMDD.jsonl` (one per UTC day). Only the last 7 days are
kept, and 10 MB in all at most: the oldest files are deleted first.

### `input`: one record per input

From the first key the IME handles for an input (e.g. the first pinyin letter) to its commit or
cancel:

| Field | Contents |
|---|---|
| `t` | time written (UTC) |
| `app` | file name of the program typed into, e.g. `WINWORD.EXE` |
| `window_title` | title of its foreground window (often the document or page title) |
| `mode` | input mode: pinyin / wubi / mixed / english / symbol |
| `keys` | the keys the IME handled in this input, e.g. `["n", "i", "SPACE"]` (including backspace, arrows, paging and digit picks) |
| `code` | the typed code, e.g. the pinyin `nihao` |
| `candidates` | the page of candidates shown before the commit, and which one was recommended |
| `picked` | the position picked (from 0; -1 when not picked from the candidates) |
| `committed` | the committed text (empty when cancelled) |
| `laya_context` | the context given to the recommendation model: the text before the caret when this input started (up to 128 Chinese / 192 English characters). It is read from the input box, so it may contain text not typed with Zhiyi IME, such as pasted text or what the box already held; where the program does not allow reading it, the text committed with Zhiyi IME before |
| `duration_ms` | how long the input took, in milliseconds |

### `stats`: usage statistics every 30 minutes

`minutes` period length; `keys` keys handled by the IME; `latency_ms_buckets` key handling time
distribution (counts under 5, 10, 20, 50, 100, 200 ms and 200 ms or more); `max_latency_ms` longest
time; `picks_by_position` picks of candidates 1 to 10; `picks_with_recommendation` picks while a
recommendation was shown; `recommendation_picked` of those, picks of the recommendation;
`laya_calls`, `laya_reordered`, `laya_last_ms` recommendation model calls, reorderings and the
latest inference time.

### Never recorded, even in tier 2

- **Keys the IME does not handle**: letters typed straight into the program in English mode,
  shortcuts (such as Ctrl+C), and other keys pressed outside an input.
  (If such text is before the caret, it appears in `laya_context` as context.)
- **Password fields**: Windows turns the IME off in password fields, so those keys never reach it;
  the IME also never reads the text of password fields or other fields marked private.
- Identity information such as user name, computer name, IP address or hardware serial numbers,
  and full file paths.

## Context read by the recommendation model (not part of this program)

To recommend candidates by context, the IME reads up to 256 characters before the caret when you
start typing and gives them to the recommendation model running on your computer. The text is only
used in memory: **nothing is sent over the network or written to a file**; only tier 2 writes it to
`laya_context` as listed above. Password fields and fields marked private are never read. Turning
the recommendation model off (`laya.enable`) stops the reading too.
When a graphics card is chosen as the device on the General page, the model runs on it through
Microsoft's DirectML component; the text is still only processed in this computer's memory and
video memory. The IME turns off ONNX Runtime's own usage events; under its license terms DirectML
may collect usage information through Windows diagnostic data, which the Windows "Diagnostics &
feedback" settings control. It does not go through the IME and does not contain what you type.

## Update check (not part of this program)

"Tell me when a new version is available" in Settings > Updates (on by default, setting
`update.notify`) downloads the version information of the latest release (`latest.json`) from
GitHub when Settings opens; "Check for updates" downloads it too. This is a plain file download:
**no IME data or records are sent**; GitHub sees only the visitor's IP address and the program
version (User-Agent). The installer is downloaded only after "Update now", and is started only
after its signature and SHA-256 are verified.

Language packs in Settings > Learning work the same way: the pack list (`glossary.json`) is
downloaded when Settings opens (with the reminder above on) and on "Check" or "Check all", a pack
file on "Download" or "Update to vN", and both are used only after their signature and SHA-256
are verified. Translations are looked up on this computer; nothing you type is sent.

## Backups (not part of this program)

A backup made with "Export…" in Settings > Dictionary holds the settings, the lexicon and the
learning records, so it holds words you typed; it is written only to the folder you choose and is
never uploaded. The two privacy choices are not in it: importing a backup from another computer
does not change this computer's choices.

## Developer diagnostics (not part of this program)

`diagnostics.trace_mode` (default `off`) is a detailed developer trace that records typed codes,
written as `*-trace.jsonl` in the same `logs` folder. Settings has no switch for it, and the user
experience improvement program never turns it on.
