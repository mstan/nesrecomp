import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    testTimeout: 30_000,
    reporters: ["verbose"],
    pool: "threads",
    fileParallelism: false,
    maxWorkers: 1,
  },
});
