# Explicit separator policy for lazy path completion

Softline 0.16.0; requested for CAI on 2026-11-07.

CAI should insert directory names without a trailing slash so Tab can cycle all entries in the current directory.
Only typing `/` should enter the selected directory's completion list.
Backspacing out of that directory should restore its parent list.
Trigger stripping, undo, queue editing and candidate ownership must remain inside Softline.

## Public API gap

The current `sl_completion_provider_t.expand` callback runs before Tab rotates a completed candidate.
Its `completed` argument is the candidate, possibly an ancestor of a partially typed child.
Softline caches the result, including an empty or failed attempt, by that candidate until activation ends.

Removing CAI's automatically appended slash alone would disable directory continuation.
Expanding slashless directories unconditionally would continue descending on Tab, preserving the original bug.
Inspecting the draft and returning an empty result until `/` is typed also fails: that empty result is already cached when the next Tab should rotate the parent list.
Reinstalling providers per key, emitting synthetic candidates, or bypassing the native completion state would give CAI responsibility for editor behavior it must not own.

## Requested policy

Add an optional `expand_separator` character to each provider, with zero preserving the current generic continuation behavior.
CAI would set it to `/` for `@` path completion and stop appending `/` to directory candidates.

With that policy, an exact completed candidate remains in its parent cycle.
Do not invoke or cache a continuation attempt until the actual draft contains the separator immediately after that candidate.
Pass the separator-inclusive prefix to `expand`, so CAI can list precisely that directory through its existing callback.
Match partially typed children against that list and preserve the existing visited-parent behavior after Backspace.
An empty directory, unreadable directory or failed callback must not consume a parent-list rotation or corrupt the draft.

The policy belongs to the frontend completion API, independent of CAI's backend queue and transport.
It must also work with stripped triggers, spaces in filenames and directory names, bracketed paste, undo/redo, and editing queued or steered entries.

## Acceptance checks

Use a directory containing two files and two nonempty subdirectories.
Repeated Tab must cycle all four names and return to the initial draft without adding `/` or traversing a child.
Type `/` after a selected directory, then Tab: its children become available on demand.
Repeat at multiple depths, remove the separator with Backspace, and cycle the parent again.
Cover empty/unreadable/deleted directories, a regular file followed by `/`, symlinks, Unicode and spaced names, no matches, callback failure, and provider replacement.
Submitting a selected path must strip exactly the introductory `@` and preserve the explicitly typed path separators.

CAI's existing nested completion remains unchanged until this policy is available.
