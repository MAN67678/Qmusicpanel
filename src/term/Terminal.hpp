// QPanel — terminal engine: raw mode, 24-bit double-buffered canvas,
// SGR 1006 extended mouse, UTF-8 in/out, SIGWINCH resize.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <termios.h>

#include "../ui/Palette.hpp"

namespace qp {

struct Mouse {
    enum Type { Press, Release, Motion, WheelUp, WheelDown };
    Type type = Press;
    int x = -1, y = -1;
};

struct Input {
    enum Kind { None, Key, MouseEv, Quit };
    Kind kind = None;
    int key = 0;          // unicode codepoint when kind==Key
    Mouse m{};
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

class Terminal {
public:
    static Terminal& get() { static Terminal t; return t; }

    bool start();
    void stop();

    void setCell(int x, int y, char32_t ch, Color fg, Color bg);
    void blendCell(int x, int y, char32_t ch, Color fg, float alpha);
    void fillRect(const Rect& r, char32_t ch, Color fg, Color bg);
    void clear();

    void flush();                 // diff & emit

    bool poll(Input& out);        // non-blocking

    int width()  const { return w_; }
    int height() const { return h_; }
    // read the current frame's cell (used to avoid painting over text)
    char32_t peek(int x, int y) const {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return U' ';
        return cur_[size_t(y) * w_ + x].ch;
    }
    bool resized() { return resized_.exchange(false); }
    void markResized() { resized_ = true; }
    void refreshSize();

    void writeStr(const std::string& s);

private:
    struct Cell {
        char32_t ch = U' '; Color fg = NONE; Color bg = NONE;
        bool cont = false;      // right half of a 2-column glyph (ch==0)
        bool operator==(const Cell& o) const { return ch == o.ch && fg == o.fg && bg == o.bg && cont == o.cont; }
        bool operator!=(const Cell& o) const { return !(*this == o); }
    };

    static void detachWide(std::vector<Cell>& g, int w, size_t i);
    void onResize();   // private impl
    bool readOne(uint8_t& b);
    void parseStream();
    int  decodeUtf8Push(size_t at);
    void decodeCsi(const std::string& params, uint8_t final);


    int w_ = 0, h_ = 0;
    termios saved_{};
    bool raw_ = false;

    std::vector<Cell> cur_, last_;
    std::string out_;

    std::vector<uint8_t> inBuf_;
    std::vector<Input> pending_;
    std::atomic<bool> resized_{false};
    bool clearPending_ = false;   // resize needs a physical clear: the diff
                                  // only emits changed cells, so stale cells
                                  // from the previous size would survive

    int parseState_ = 0;
    std::string csi_;
};

extern Terminal& term;

} // namespace qp
