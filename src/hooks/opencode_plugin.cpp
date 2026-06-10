#include "opencode_plugin.hpp"

namespace ghost {
namespace hooks {
static const char* OPENCODE_PLUGIN_CONTENT = R"(function extractPath(input, output) {
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
  try {
    const fs = require("fs")
    for (const home of homeCandidates()) {
      const modelPath = home + "/.ghost/.current_model"
      if (fs.existsSync(modelPath)) {
        const model = fs.readFileSync(modelPath, "utf8").trim()
        if (model) return model
      }
    }
  } catch (e) {}
  return ""
}

function homeCandidates() {
  const values = []
  const add = (value) => {
    if (value && typeof value === "string" && !values.includes(value)) values.push(value)
  }
  add(process.env.HOME)
  add(process.env.USERPROFILE)
  return values
}

function pathExists(filePath) {
  try {
    return require("fs").existsSync(filePath)
  } catch (e) {
    return false
  }
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

export const GhostPlugin = async ({ $, directory, worktree }) => {
  let currentModel = detectModel() || "opencode"
  writeModelFile(currentModel)

  function getCheckpointPath() {
    const bins = []
    const addBin = (bin) => {
      if (bin && typeof bin === "string" && !bins.includes(bin)) bins.push(bin)
    }
    addBin(process.env.GHOST_BIN)
    for (const home of homeCandidates()) addBin(home + "/.ghost/bin")

    for (const bin of bins) {
      const unixPath = bin + "/ghost-checkpoint"
      const exePath = process.platform === "win32"
        ? bin.replace(/\//g, "\\") + "\\ghost-checkpoint.exe"
        : bin + "/ghost-checkpoint.exe"
      if (pathExists(unixPath)) return unixPath
      if (pathExists(exePath)) return exePath
    }

    const fallback = bins[0] || ((process.env.HOME || process.env.USERPROFILE || "") + "/.ghost/bin")
    return process.platform === "win32"
      ? fallback.replace(/\//g, "\\") + "\\ghost-checkpoint.exe"
      : fallback + "/ghost-checkpoint"
  }

  function writeModelFile(model) {
    const home = homeCandidates()[0] || ""
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
)";
const char* openCodePluginContent() {
    return OPENCODE_PLUGIN_CONTENT;
}

}
}