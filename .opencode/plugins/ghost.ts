function extractPath(input, output) {
  const sources = [output?.args, input?.args, output, input]
  for (const source of sources) {
    if (!source) continue
    for (const key of ["path", "file", "filePath", "file_path"]) {
      if (source[key] && typeof source[key] === "string") return source[key]
    }
    if (source.files && Array.isArray(source.files) && source.files.length > 0) {
      return source.files[0]
    }
  }
  return ""
}

function isTrackedTool(input, output) {
  const tool = input?.tool || output?.tool || input?.name || output?.name || ""
  return tool === "edit" || tool === "write" || tool === "apply_patch"
}

function normalizeModel(value) {
  if (!value) return ""
  if (typeof value === "string") {
    const parts = value.split("/")
    return parts[parts.length - 1] || value
  }
  if (typeof value === "object") {
    return normalizeModel(value.modelID || value.modelId || value.id || value.name || value.model)
  }
  return ""
}

function extractModelFromEvent(event) {
  if (!event) return ""
  const info = event.properties?.info || event.info || {}
  return normalizeModel(event.model) ||
    normalizeModel(event.properties?.model) ||
    normalizeModel(info.model) ||
    normalizeModel(info.modelID) ||
    normalizeModel(info.modelId)
}

function extractModelFromTool(input, output) {
  const sources = [output?.args, input?.args, output, input]
  for (const source of sources) {
    const model = normalizeModel(source?.model || source?.modelID || source?.modelId)
    if (model) return model
  }
  return ""
}

function detectModel() {
  const home = process.env.USERPROFILE || process.env.HOME || ""
  const modelPath = home + "/.ghost/.current_model"
  try {
    const fs = require("fs")
    if (fs.existsSync(modelPath)) {
      return fs.readFileSync(modelPath, "utf8").trim()
    }
  } catch (e) {}
  return ""
}

function resolveFilePath(filePath, directory, worktree) {
  if (!filePath) return ""
  try {
    const path = require("path")
    if (path.isAbsolute(filePath)) return filePath
    const base = (typeof directory === "string" && directory) ||
      (typeof worktree === "string" && worktree) ||
      process.cwd()
    return path.resolve(base, filePath)
  } catch (e) {
    return filePath
  }
}

function writeModelFile(model) {
  const home = process.env.USERPROFILE || process.env.HOME || ""
  const ghostDir = home + "/.ghost"
  const modelPath = home + "/.ghost/.current_model"
  try {
    const fs = require("fs")
    fs.mkdirSync(ghostDir, { recursive: true })
    if (model) {
      fs.writeFileSync(modelPath, model)
    } else {
      try { fs.unlinkSync(modelPath) } catch (e) {}
    }
  } catch (e) {}
}

export const GhostPlugin = async ({ $, directory, worktree }) => {
  let currentModel = detectModel() || "opencode"
  writeModelFile(currentModel)

  function getBinDir() {
    if (process.env.GHOST_BIN) return process.env.GHOST_BIN
    const home = process.env.USERPROFILE || process.env.HOME || ""
    return home + "/.ghost/bin"
  }

  function getCheckpointPath() {
    const bin = getBinDir()
    if (process.platform === "win32") {
      return bin.replace(/\//g, "\\") + "\\ghost-checkpoint.exe"
    }
    return bin + "/ghost-checkpoint"
  }

  return {
    event: async ({ event }) => {
      const model = extractModelFromEvent(event)
      if (model) currentModel = model
      else if (!currentModel) currentModel = detectModel() || "opencode"
      writeModelFile(currentModel)
    },
    "tool.execute.before": async (input, output) => {
      if (isTrackedTool(input, output)) {
        currentModel = extractModelFromTool(input, output) || currentModel || detectModel() || "opencode"
        writeModelFile(currentModel)
        const cp = getCheckpointPath()
        const filePath = resolveFilePath(extractPath(input, output), directory, worktree)
        if (filePath) {
          await $`${cp} pre --agent opencode --file ${filePath}`.quiet().catch(() => {})
        } else {
          await $`${cp} pre --agent opencode`.quiet().catch(() => {})
        }
      }
    },
    "tool.execute.after": async (input, output) => {
      if (isTrackedTool(input, output)) {
        currentModel = extractModelFromTool(input, output) || currentModel || detectModel() || "opencode"
        writeModelFile(currentModel)
        const cp = getCheckpointPath()
        const filePath = resolveFilePath(extractPath(input, output), directory, worktree)
        if (filePath) {
          await $`${cp} post --agent opencode --model ${currentModel} --file ${filePath}`.quiet().catch(() => {})
        } else {
          await $`${cp} post --agent opencode --model ${currentModel}`.quiet().catch(() => {})
        }
      }
    },
  }
}
