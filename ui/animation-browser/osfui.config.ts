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
    targetVersion: "1.3.0",
    permissions: {
      nativeBridge: true,
    },
  }],
});
