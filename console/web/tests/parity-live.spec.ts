import { test, expect } from "@playwright/test";
import { execFileSync, spawn, type ChildProcess } from "node:child_process";
import { resolve } from "node:path";
test("parity console through real Linux API, Docker PTY, logs and link telemetry", async ({
  page,
}) => {
  test.skip(
    !process.env.GRAPHLAB_PARITY_LIVE,
    "Requires explicitly prepared isolated Linux parity services",
  );
  test.setTimeout(120000);
  page.setDefaultTimeout(15000);
  const config =
    process.env.GRAPHLAB_SSH_CONFIG ??
    `${process.env.HOME}/.lima/graphlab/ssh.config`;
  const fixture =
    process.env.GRAPHLAB_PARITY_ROOT ?? "/var/tmp/gl6-console-parity-20260923";
  const password = execFileSync(
    "ssh",
    ["-F", config, "lima-graphlab", `cat ${fixture}/password`],
    { encoding: "utf8" },
  ).trim();
  let tunnel: ChildProcess | undefined;
  try {
    tunnel = spawn(
      "ssh",
      [
        "-F",
        config,
        "-N",
        "-o",
        "ExitOnForwardFailure=yes",
        "-L",
        "127.0.0.1:18091:127.0.0.1:18091",
        "lima-graphlab",
      ],
      { stdio: "ignore" },
    );
    for (let i = 0; i < 60; i++) {
      try {
        await fetch("http://127.0.0.1:18091/");
        break;
      } catch {
        await new Promise((r) => setTimeout(r, 50));
      }
    }
    await page.goto("http://127.0.0.1:18091/");
    await page.getByLabel("Operator credential").fill(password);
    await page.getByRole("button", { name: "Sign in", exact: true }).click();
    await page.getByLabel("I accept running without capture coverage.").check();
    await page.getByRole("button", { name: "Start selected topology" }).click();
    await expect(
      page.getByRole("status").filter({ hasText: "start: succeeded" }),
    ).toBeVisible({ timeout: 60000 });
    await page
      .getByText("Accessible inventory · all nodes and data edges")
      .click();
    await page.getByRole("button", { name: /^a · docker · runtime/ }).click();
    await expect(page.getByLabel("Workload", { exact: true })).toHaveValue("a");
    await page.getByRole("button", { name: "Logs", exact: true }).click();
    await expect(page.getByRole("region", { name: "Node logs" })).toContainText(
      "Container stdout/stderr",
    );
    await expect(
      page.getByRole("region", { name: "Node logs" }).getByRole("alert"),
    ).toHaveCount(0);
    await page
      .getByRole("button", { name: "Container shell", exact: true })
      .click();
    await expect(page.locator(".xterm-screen")).toBeVisible();
    await page
      .getByRole("button", { name: "Acquire writer", exact: true })
      .click();
    await expect(page.getByText(/Writer lease active/)).toBeVisible();
    await page.locator(".xterm-helper-textarea").focus();
    await page.keyboard.type("echo PARITY_LIVE_CONSOLE");
    await page.keyboard.press("Enter");
    await expect(page.locator(".xterm-rows")).toContainText(
      "PARITY_LIVE_CONSOLE",
    );
    await page
      .getByRole("button", { name: "Release keyboard", exact: true })
      .click();
    await expect(page.getByText(/Read-only viewer/)).toBeVisible();
    await page.getByRole("button", { name: "Reconnect and replay" }).click();
    await expect(page.locator(".xterm-rows")).toContainText(
      "PARITY_LIVE_CONSOLE",
    );
    const metrics = page.getByRole("region", { name: "Telemetry and faults" });
    await metrics.getByRole("button", { name: "a-s", exact: true }).click();
    await expect(
      page.getByRole("region", { name: "Link performance" }),
    ).toContainText("rtnetlink/iproute2-stats64");
    await expect(page.locator(".edge-rate").first()).not.toHaveText("stale");
    await page
      .getByRole("button", { name: "Close shell", exact: true })
      .click();
    const download = page.waitForEvent("download");
    await page
      .getByRole("button", { name: /Download recording/ })
      .first()
      .click();
    expect((await download).suggestedFilename()).toMatch(/\.glterm$/);
    await page.screenshot({
      path: resolve("..", "..", "docs/validation/web-console-parity-live.png"),
      fullPage: true,
    });
  } finally {
    try {
      if (
        await page
          .getByRole("button", { name: "Destroy run", exact: true })
          .isVisible()
      ) {
        await page
          .getByRole("button", { name: "Destroy run", exact: true })
          .click();
        await expect(
          page.getByRole("status").filter({ hasText: "destroy: succeeded" }),
        ).toBeVisible({ timeout: 30000 });
      }
    } finally {
      tunnel?.kill();
    }
  }
});
