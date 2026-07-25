import { expect, test } from "@playwright/test";

function glb(id: number): Buffer {
  const json = Buffer.from(JSON.stringify({ asset: { version: "2.0" }, scene: 0, scenes: [{ nodes: [] }], nodes: [], animations: [], extras: { id } }));
  const padding = (4 - (json.length % 4)) % 4;
  const payload = Buffer.concat([json, Buffer.alloc(padding, 0x20)]);
  const header = Buffer.alloc(20);
  header.write("glTF", 0, "ascii");
  header.writeUInt32LE(2, 4);
  header.writeUInt32LE(20 + payload.length, 8);
  header.writeUInt32LE(payload.length, 12);
  header.writeUInt32LE(0x4e4f534a, 16);
  return Buffer.concat([header, payload]);
}

test("creates a scene from a Clip Set and round-trips its portable bundle", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: /Clip library/ }).click();
  await page.locator('input[type="file"][accept=".af,.glb,.gltf"]').setInputFiles([
    { name: "speaker.glb", mimeType: "model/gltf-binary", buffer: glb(1) },
    { name: "listener.glb", mimeType: "model/gltf-binary", buffer: glb(2) },
  ]);
  await expect(page.locator(".clip-tile")).toHaveCount(2);

  await page.getByRole("button", { name: /Clip Sets/ }).click();
  await page.locator(".clipset-editor").getByRole("button", { name: "Create Clip Set" }).click();
  await page.getByLabel("Title").fill("Bundle Pair");
  await page.getByRole("button", { name: "Add actor" }).click();
  await page.getByRole("button", { name: "Add actor" }).click();
  const members = page.locator(".clipset-member");
  await members.nth(0).getByLabel("Role").fill("speaker");
  await members.nth(0).getByLabel("Clip").selectOption({ label: "speaker" });
  await members.nth(1).getByLabel("Role").fill("listener");
  await members.nth(1).getByLabel("Clip").selectOption({ label: "listener" });
  await members.nth(1).getByLabel("y").fill("1.25");
  await members.nth(1).getByLabel("heading").fill("180");
  await page.getByRole("button", { name: "Save Clip Set" }).click();
  await expect(page.getByText("Saved Bundle Pair")).toBeVisible();

  await page.getByRole("button", { name: "Use in scene" }).click();
  await expect(page.getByRole("heading", { name: "Bundle Pair" })).toBeVisible();
  await expect(page.getByText("2 library ready")).toBeVisible();
  await expect(page.getByLabel("Library")).toHaveCount(2);

  const downloadEvent = page.waitForEvent("download");
  await page.getByRole("button", { name: "Export bundle" }).click();
  const download = await downloadEvent;
  expect(download.suggestedFilename()).toMatch(/\.osfscene$/);
  const bundlePath = await download.path();
  expect(bundlePath).toBeTruthy();

  await page.locator('input[type="file"][accept=".osfscene,.zip"]').setInputFiles(bundlePath!);
  await expect(page.getByText("Imported scene bundle with 2 clips")).toBeVisible();
});
