import { expect, test } from "@playwright/test";

function minimalRig(): Buffer {
  const buffer = Buffer.alloc(181);
  buffer.writeFloatLE(1 / 64, 48);
  buffer.writeFloatLE(1 / 16384, 52);
  buffer.writeUInt16LE(1, 56);
  buffer.writeUInt16LE(1, 58);
  buffer.writeFloatLE(1, 80);
  buffer.writeBigUInt64LE(176n, 128);
  buffer.writeInt32LE(-1, 136);
  buffer.write("Root", 176, "utf8");
  return buffer;
}

test("creates, keys, persists, and exports a rig animation into the Clip Library", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: /Animate/ }).click();
  await expect(page.getByRole("heading", { name: "Choose a skeleton to begin" })).toBeVisible();

  await page.locator('input[type="file"][accept=".rig"]').setInputFiles({
    name: "test-skeleton.rig",
    mimeType: "application/octet-stream",
    buffer: minimalRig(),
  });
  await expect(page.getByText("New animation project created")).toBeVisible();
  await page.getByLabel("Title").fill("Browser Wave");
  await page.getByRole("button", { name: "Root", exact: true }).click();
  await page.getByRole("button", { name: "＋ Key pose" }).click();
  await expect(page.getByRole("button", { name: "F0" })).toBeVisible();

  const downloadEvent = page.waitForEvent("download");
  await page.getByRole("button", { name: "Export GLB + add to library" }).click();
  const download = await downloadEvent;
  expect(download.suggestedFilename()).toBe("Browser_Wave.glb");
  await expect(page.getByText(/validated it, and added/)).toBeVisible();

  await page.getByRole("button", { name: "Open Clip Library" }).click();
  await expect(page.getByText("Browser Wave", { exact: true }).first()).toBeVisible();

  await page.reload();
  await page.getByRole("button", { name: /Animate/ }).click();
  await expect(page.getByLabel("Title")).toHaveValue("Browser Wave");
  await page.getByRole("button", { name: "Root", exact: true }).click();
  await expect(page.getByRole("button", { name: "F0" })).toBeVisible();
});
