export const GhostPlugin = async ({ $, directory, worktree }) => {
  let currentModel = "unknown"

  function getBinDir() {
    if (process.env.GHOST_BIN) return process.env.GHOST_BIN
    const home = process.env.USERPROFILE || process.env.HOME || ""
    return home + "/.ghost/bin"
  }

  function getCheckpointPath() {
    const bin = getBinDir()
    return process.platform === "win32"
      ? bin.replace(/\//g, "\\") + "\\ghost-checkpoint.exe"
      : bin + "/ghost-checkpoint"
  }

  function readModelFromConfig() {
    try {
      const configPath = worktree ? worktree + "/opencode.json" : directory + "/opencode.json"
      const content = Bun.file(configPath).text()
      if (!content) return "unknown"
      const json = JSON.parse(content)
      if (json.model && typeof json.model === "string") {
        const parts = json.model.split("/")
        return parts.length > 1 ? parts[1] : json.model
      }
    } catch {}
    return "unknown"
  }

  return {
    "session.updated": async ({ event }) => {
      if (event?.model) {
        const parts = event.model.split("/")
        currentModel = parts.length > 1 ? parts[1] : event.model
      }
    },
    "tool.execute.before": async (input, output) => {
      if (input.tool === "edit" || input.tool === "write" || input.tool === "apply_patch") {
        const cp = getCheckpointPath()
        await $`${cp} pre --agent opencode`.quiet().catch(() => {})
      }
    },
    "tool.execute.after": async (input, output) => {
      if (input.tool === "edit" || input.tool === "write" || input.tool === "apply_patch") {
        currentModel = readModelFromConfig()
        const cp = getCheckpointPath()
        await $`${cp} post --agent opencode --model ${currentModel}`.quiet().catch(() => {})
      }
    },
  }
}
