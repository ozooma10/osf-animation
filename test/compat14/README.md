# Published 1.4 compatibility fixtures

The five `fixture_*.osf.json` files are byte-for-byte copies from the `v1.4.0`
tag, `test/fixtures/Data/OSF`. Keep them frozen. `legacy.osf.json` and
`a-cross.osf.json` cover translation boundaries (linear timing, shorthand, graph
routes, cross-file use, inference and legacy policies). The production registry
loads these from `ContentRegistryTest`; no game or vanilla assets are required.
