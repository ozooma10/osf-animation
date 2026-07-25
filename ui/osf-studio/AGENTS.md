# OSF Studio agent guide

This directory owns the standalone, local-first authoring app. Generated output under
`../../build/osf-studio/` is disposable and must not be edited.

Run `npm run verify` after source changes.

Keep OSF JSON documents lossless at the object-field level: structured forms may update known fields,
but must preserve unknown fields and unsupported graph/track data. The browser owns only drafts and
explicit user-initiated imports/exports. Future folder and game connections belong behind a workspace
adapter rather than directly in components.
