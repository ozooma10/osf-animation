import { defineConfig } from "@osfui/cli";

export default defineConfig({
  modId: "osf.animation",
  outDir: "../../build/osfui",
  views: [{
    id: "browser",
    title: "OSF Animation Browser",
    description: "Animation, emote, and scene browser",
    kind: "menu",
    width: 1600,
    height: 900,
    pausesGame: false,
    transparent: true,
    // 1.5 adds OSF Animation-aware consented bug reports: the host recognizes
    // this qualified view id, includes OSF Animation.log, and routes the issue
    // to the Animation repository.
    targetVersion: "1.5.0",
    permissions: {
      nativeBridge: true,
    },
  }],
});
