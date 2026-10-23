# Incremental smoke and release work

The smoke production stage builds the engine; the browser stage builds the addon
once. SDK and browser-boundary checks retain content-keyed successful receipts.
SDK provenance still keys on resolved sibling tags and the actual locked Git blobs.

Browser dependencies are reused only when package/lock/configuration, Node/npm,
and the installed package files match (excluding generated `.vite`, `.vite-temp`
and `.cache` directories). `BuildBrowserAddon.ps1
-CleanDependencies` forces `npm ci`. Browser tests, typecheck and production asset
build still execute on every browser-stage invocation.

Smoke/release workflow self-tests share successful evidence for identical project,
harness and Git inputs. Runtime-enabled contract checks have a separate cache from
offline-only checks. Both entrypoints accept `-ForceVerification` to rerun those
self-tests. The direct SDK/boundary scripts accept `-Force`.

Caches live under `build/workflow-cache`; logs identify the original passing run.
Input/output edits, additions, missing files or corrupt receipts invalidate them.
Failures invalidate the previous receipt before work starts. Native tests, package
checks, reinstall tests and game cases are not skipped by these caches.
