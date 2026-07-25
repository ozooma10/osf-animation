import { expect, test } from "@playwright/test";

test("migrates a legacy draft and recovers it after reload", async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem("osf-studio.workspace.v1", JSON.stringify([{
      id: "legacy-document",
      filename: "legacy-alpha.osf.json",
      root: {
        schema: 1,
        scenes: [{ id: "legacy.scene", name: "Legacy recovery", clip: "legacy.glb" }],
      },
      dirty: true,
    }]));
  });
  await page.goto("/");
  await expect(page.locator(".file-name", { hasText: "legacy-alpha.osf.json" })).toBeVisible();
  await expect(page.getByRole("heading", { name: "Legacy recovery" })).toBeVisible();
  await page.waitForTimeout(650);
  await page.reload();
  await expect(page.locator(".file-name", { hasText: "legacy-alpha.osf.json" })).toBeVisible();
});

test("JSONC import, structured edit, undo, reload, and export", async ({ page }) => {
  await page.goto("/");
  const picker = page.locator('input[type="file"][accept=".json,.osf.json"]');
  await picker.setInputFiles({
    name: "jsonc-flow.osf.json",
    mimeType: "application/json",
    buffer: Buffer.from(`{
      // comment accepted by OSF
      "schema": 1,
      "futurePackField": { "preserve": true },
      "scenes": [{ "id": "flow.before", "name": "JSONC flow", "clip": "flow.glb", }],
    }`),
  });
  await expect(page.locator(".file-name", { hasText: "jsonc-flow.osf.json" })).toBeVisible();
  const sceneId = page.getByLabel("Scene ID");
  await sceneId.fill("flow.after");
  await page.getByTitle("Undo (Ctrl+Z)").click();
  await expect(sceneId).toHaveValue("flow.before");
  await sceneId.fill("flow.persisted");
  await page.waitForTimeout(650);
  await page.reload();
  await expect(page.getByLabel("Scene ID")).toHaveValue("flow.persisted");
  const download = page.waitForEvent("download");
  await page.getByRole("button", { name: "Export JSON" }).click();
  expect((await download).suggestedFilename()).toContain("jsonc-flow.osf.json");
});

test("viewer lazy-loads, rejects malformed GLB, and disposes between switches", async ({ page }) => {
  await page.goto("/");
  for (let index = 0; index < 4; index += 1) {
    await page.getByRole("button", { name: /Animation viewer/ }).click();
    await expect(page.locator(".viewer-canvas canvas")).toHaveCount(1);
    await page.getByRole("button", { name: /Scene editor/ }).click();
    await expect(page.locator(".viewer-canvas canvas")).toHaveCount(0);
  }
  await page.getByRole("button", { name: /Animation viewer/ }).click();
  const picker = page.locator('input[type="file"][accept*=".glb"]');
  await picker.setInputFiles({
    name: "malformed.glb",
    mimeType: "model/gltf-binary",
    buffer: Buffer.from([0x67, 0x6c, 0x54, 0x46]),
  });
  await expect(page.getByRole("alert")).toContainText(/malformed|invalid|truncated/i);
  await expect(page.locator(".viewer-canvas canvas")).toHaveCount(1);
});

