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

test("imports clips, persists metadata, builds a Clip Set, and protects dependencies", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: /Clip library/ }).click();

  const picker = page.locator('input[type="file"][accept=".af,.glb,.gltf"]');
  await picker.setInputFiles([
    { name: "lead_idle.glb", mimeType: "model/gltf-binary", buffer: glb(1) },
    { name: "partner_idle.glb", mimeType: "model/gltf-binary", buffer: glb(2) },
  ]);
  await expect(page.locator(".clip-tile", { hasText: "lead idle" })).toBeVisible();
  await expect(page.locator(".clip-tile", { hasText: "partner idle" })).toBeVisible();
  await expect(page.locator(".clip-tile img")).toHaveCount(2);

  await page.getByRole("button", { name: /lead idle/i }).click();
  await page.getByLabel("Title").fill("Lead Idle");
  await page.getByLabel("Creator").fill("Alpha Author");
  await page.getByLabel("Game-relative file path").fill("OSF/Community/lead_idle.glb");
  await page.getByLabel(/Tags/).fill("idle, conversation");
  await page.getByRole("button", { name: "Save metadata" }).click();

  await page.getByRole("button", { name: /Clip Sets/ }).click();
  await page.locator(".clipset-editor").getByRole("button", { name: "Create Clip Set" }).click();
  await page.getByLabel("Title").fill("Two Actor Conversation");
  await page.getByRole("button", { name: "Add actor" }).click();
  await page.getByRole("button", { name: "Add actor" }).click();
  const members = page.locator(".clipset-member");
  await members.nth(0).getByLabel("Role").fill("speaker");
  await members.nth(0).getByLabel("Clip").selectOption({ label: "Lead Idle" });
  await members.nth(1).getByLabel("Role").fill("listener");
  await members.nth(1).getByLabel("Clip").selectOption({ label: "partner idle" });
  await page.getByRole("button", { name: "Save Clip Set" }).click();
  await expect(page.getByText("Saved Two Actor Conversation")).toBeVisible();

  await page.reload();
  await page.getByRole("button", { name: /Clip library/ }).click();
  await page.getByRole("button", { name: /Clip Sets/ }).click();
  await expect(page.getByRole("heading", { name: "Two Actor Conversation" })).toBeVisible();
  await expect(page.getByText("Dependencies ready")).toBeVisible();

  await page.getByRole("button", { name: /^Clips/ }).click();
  await page.getByRole("button", { name: /Lead Idle/ }).click();
  page.once("dialog", (dialog) => dialog.accept());
  await page.getByRole("button", { name: "Remove clip" }).click();
  await expect(page.getByText(/Remove this clip from Two Actor Conversation/)).toBeVisible();
  await expect(page.locator(".clip-tile", { hasText: "Lead Idle" })).toBeVisible();
});
