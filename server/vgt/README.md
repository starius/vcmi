# VGT server component

VGT records a game as a readable YAML event transcript and can replay that
transcript through the authoritative server. It is an optional, statically linked
server component. Configure VCMI with `-DENABLE_VGT=ON` to include it; the option is
off by default.

Code outside this directory uses only two narrow facades:

- `Integration.h` contains the recording hooks called from normal server flow. Its
  disabled implementation consists of inline no-ops.
- `CommandLine.h` adds and handles replay-only server command-line options. Its
  disabled implementation does nothing, so non-VGT builds do not expose those
  options.

`Recorder`, `Replay`, and `Discovery` are implementation details of this component.
They are not a general extension or plugin API.

An `ENABLE_VGT=ON` build records every authoritative game automatically. Transcript
pairs live in `Transcripts` beside the user's `Saves` directory. A main filename has
the form `20260801T120000Z_map-name_g0123456789ab.vgt`; its optional tactical detail
uses the adjacent `.vgt.battles.yaml` suffix.

Successful manual saves and autosaves add an ignored, append-only savepoint comment.
Loading that exact save resumes the matching transcript pair. Loading an older save
forks both files at their recorded byte offsets so later history is preserved. If no
savepoint exists, capture continues in a clearly marked `_partial-...` transcript
that cannot replay until its missing prefix is repaired.

Interface readiness, UI pause toggles, and context-free dialog replies are session
protocol rather than game history and are omitted. Replay supplies any required
interface readiness itself. When resuming a transcript produced before this cleanup,
a lifecycle-only tail after the matching savepoint is discarded instead of creating
a false branch.

Battle IDs are transcript-local monotonic indices, not the engine's ephemeral battle
IDs. Their next value is stored in each savepoint, so loading a save cannot reuse an ID.
A cancelled quick-combat attempt is buffered transactionally and discarded; only the
accepted, complete battle is appended to the main transcript and companion.

`SaveLocalState` packets, connection IDs, generated default player names, and the
derived statistics history are session presentation rather than game events and are
omitted. Turn-state comparison canonicalizes those fields before byte comparison. Set
`VCMI_VGT_TURN_STATE_DIR=/path/game.turn-states` only when an exact authoritative
save after every player turn is useful for replay diagnostics; these large debug
saves are not part of the portable transcript.

Use `scripts/vgt_replay.py` to validate YAML and invoke the server replay interface.
The script can replay the complete transcript, only its initialized header, or the
first N player turns. The result can be written as a normal VCMI save.
