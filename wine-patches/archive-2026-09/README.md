# Historical snapshot, 2026-09-09 .. 09-12

`git format-patch` output from an earlier session, kept because it is the only record of that state
of the Wine work. **Do not treat it as current** - it predates the current base by two weeks, and
`0003` in particular is 2.7 MB and touches files that are now the other session's
(`dlls/kernelbase`, `dlls/ntdll`). The live export is the per-file set one directory up; regenerate
it with `export-wine-patches.sh`.

Kept rather than deleted because a snapshot of a tree nobody can reconstruct is worth more than a
tidy directory.
