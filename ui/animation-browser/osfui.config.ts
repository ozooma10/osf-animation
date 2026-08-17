import { defineConfig } from "@osfui/cli";

export default defineConfig({
  modId: "osf.animation",
	// Dedicated CLI-owned tree. Do not point this at the legacy unmarked build/osfui directory.
	outDir: "../../build/osfui-animation-browser",
  views: [{
    id: "browser",
    title: "OSF Animation Browser",
    description: "Animation and scene browser",
    kind: "menu",
    width: 1600,
    height: 900,
    pausesGame: false,
    transparent: true,
    // The view uses the 2.0 helper transport; older hosts cannot satisfy this contract.
    targetVersion: "2.0.0",
    permissions: {
      nativeBridge: true,
    },
  }],
});
