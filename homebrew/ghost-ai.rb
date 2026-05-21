class GhostAi < Formula
  desc "Git hook for origin source tracking — AI code attribution for git repos"
  homepage "https://github.com/farhankhan197/ghost"
  url "https://github.com/farhankhan197/ghost/releases/download/v0.1.0/ghost-macos-x86_64.tar.gz"
  sha256 "PLACEHOLDER"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "ninja" => :build

  def install
    if Hardware::CPU.arm?
      # Build from source on Apple Silicon
      system "mkdir", "-p", "build"
      cd "build" do
        system "cmake", "..", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"
        system "ninja"
        bin.install "ghost" => "ghost"
        bin.install "ghost-checkpoint" => "ghost-checkpoint"
      end
    else
      # Use pre-built binary on Intel
      bin.install "ghost" => "ghost"
      bin.install "ghost-checkpoint" => "ghost-checkpoint"
    end
  end

  test do
    system "#{bin}/ghost", "version"
  end
end
