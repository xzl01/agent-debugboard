# Evidence redactions

Before repository publication, user-specific absolute local host and removable-media
path prefixes in text-only build, copy, mount, and upload records were deterministically
replaced with HOME, WORKSPACE, WEST_WORKSPACE, REVIEW_WORKTREE, or REMOVABLE_MEDIA
placeholders. Generic temporary paths inside frozen source-provenance patches remain
unchanged because they contain no user identity and changing them would invalidate patch
reconstruction. No protocol frame, decoded sample, timing, counter, firmware identity,
artifact digest, HTTP result, CDC payload, or HIL verdict was changed. SHA256SUMS was
regenerated after this path-only publication redaction.
