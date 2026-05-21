#ifndef GHOST_OUTPUT_STYLE_HPP
#define GHOST_OUTPUT_STYLE_HPP

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace ghost {
namespace output {

class AnimatedSpinner {
public:
    AnimatedSpinner() = default;
    explicit AnimatedSpinner(const std::string& message) { start(message); }
    void start(const std::string& message);
    void update(const std::string& message);
    void stop();
    ~AnimatedSpinner();

private:
    void render();

    std::atomic<bool> m_running{false};
    std::atomic<int> m_frame{0};
    std::string m_message;
    std::thread m_thread;
    std::mutex m_mutex;
};

class Style {
public:
    static bool useColor();

    static std::string glow(const std::string& s);
    static std::string purple(const std::string& s);
    static std::string violet(const std::string& s);
    static std::string blue(const std::string& s);
    static std::string muted(const std::string& s);
    static std::string dim(const std::string& s);
    static std::string bold(const std::string& s);

    static std::string success(const std::string& s);
    static std::string warning(const std::string& s);
    static std::string error(const std::string& s);

    static std::string header(const std::string& s);
    static std::string subHeader(const std::string& s);
    static std::string label(const std::string& s);

    static std::string horizontalRule();
    static std::string progressBar(int current, int total, int width = 20);
    static std::string animatedProgressBar(int current, int total, int width = 20, int steps = 10);

    static std::string ghostLogo();
    static std::vector<std::string> mascot();
    static std::string spinner(int frame);
};

}
}

#endif
