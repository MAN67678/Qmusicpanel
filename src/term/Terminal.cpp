#include "Terminal.hpp"
#include "../ui/Text.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace qp {

Terminal& term = Terminal::get();

namespace {
    void sigwinch_handler(int) { Terminal::get().markResized(); }
}

void Terminal::refreshSize() { onResize(); }

bool Terminal::start() {
    if (!isatty(STDIN_FILENO)) return false;
    tcgetattr(STDIN_FILENO, &saved_);
    termios raw = saved_;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    raw_ = true;

    signal(SIGWINCH, sigwinch_handler);
    onResize();

    std::string s;
    s += "\e[?1049h";                    // alt screen
    s += "\e[?1000h\e[?1002h\e[?1006h";  // SGR extended mouse (press/motion/release)
    s += "\e[?25l";                      // hide cursor
    writeStr(s);
    clear();
    flush();
    return true;
}

void Terminal::stop() {
    std::string s;
    s += "\e[?25h";
    s += "\e[?1006l\e[?1002l\e[?1000l";
    s += "\e[?1049l";
    writeStr(s);
    if (raw_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    raw_ = false;
}

void Terminal::onResize() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return;
    int w = ws.ws_col > 0 ? int(ws.ws_col) : 80;
    int h = ws.ws_row > 0 ? int(ws.ws_row) : 24;
    if (w == w_ && h == h_ && cur_.size() == size_t(w * h)) { resized_ = true; return; }
    w_ = w; h_ = h;
    cur_.assign(size_t(w_) * h_, Cell{});
    last_.assign(size_t(w_) * h_, Cell{});
    clearPending_ = true;      // wipe the physical screen before the repaint
    resized_ = true;
}

// A cell grid where one glyph may span two columns. The lead cell holds the
// codepoint; the trailing cell is marked cont with ch==0 and is skipped by the
// flush. Overwriting either half detaches its partner so no orphan half-glyph
// is left behind (that is what shifts a whole row by one column).
void Terminal::detachWide(std::vector<Cell>& grid, int w, size_t i) {
    Cell& c = grid[i];
    if (c.cont) {
        if (i > 0) grid[i - 1] = Cell{};
    } else if (c.ch && cpWidth(c.ch) == 2 && i + 1 < grid.size() && (int)((i + 1) % size_t(w)) != 0) {
        grid[i + 1] = Cell{};
    }
    c = Cell{};
}

void Terminal::setCell(int x, int y, char32_t ch, Color fg, Color bg) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    if (ch == 0) {                                  // explicit continuation marker
        size_t i = size_t(y) * w_ + x;
        Cell& c = cur_[i];
        if (!c.cont && c.ch && cpWidth(c.ch) == 2 && x + 1 < w_) cur_[i + 1] = Cell{};
        c.ch = 0; c.fg = fg; c.bg = bg; c.cont = true;
        return;
    }
    if (ch < 0x20 || ch == 0x7F) ch = U' ';        // control bytes never reach the stream
    size_t i = size_t(y) * w_ + x;
    detachWide(cur_, w_, i);
    Cell& c = cur_[i];
    c.ch = ch; c.fg = fg; c.bg = bg; c.cont = false;
    if (cpWidth(ch) == 2) {
        if (x + 1 >= w_) { c.ch = U' '; return; }   // no room: degrade to space
        size_t j = i + 1;
        detachWide(cur_, w_, j);
        Cell& n = cur_[j];
        n.ch = 0; n.fg = fg; n.bg = bg; n.cont = true;
    }
}

void Terminal::blendCell(int x, int y, char32_t ch, Color fg, float a) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    if (ch < 0x20 || ch == 0x7F) ch = U' ';
    size_t i = size_t(y) * w_ + x;
    Color base = (cur_[i].bg == NONE) ? NONE : cur_[i].bg;
    Color keepFg = cur_[i].fg;
    detachWide(cur_, w_, i);
    Cell& c = cur_[i];
    c.ch = ch;
    c.fg = (fg == NONE) ? keepFg : mix(base, fg, a);
    c.cont = false;
    if (cpWidth(ch) == 2 && x + 1 < w_) {
        size_t j = i + 1;
        detachWide(cur_, w_, j);
        Cell& n = cur_[j];
        n.ch = 0; n.fg = c.fg; n.bg = c.bg; n.cont = true;
    }
}

void Terminal::fillRect(const Rect& r, char32_t ch, Color fg, Color bg) {
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++)
            setCell(x, y, ch, fg, bg);
}

void Terminal::clear() {
    std::fill(cur_.begin(), cur_.end(), Cell{});
    out_ += "\e[2J";
}

void Terminal::writeStr(const std::string& s) {
    ::write(STDOUT_FILENO, s.data(), s.size());
}

static void appendUtf8(std::string& s, char32_t cp) {
    if (cp <= 0x7F) s += char(cp);
    else if (cp <= 0x7FF)   { s += char(0xC0 | (cp >> 6));        s += char(0x80 | (cp & 0x3F)); }
    else if (cp <= 0xFFFF)  { s += char(0xE0 | (cp >> 12));       s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    else                    { s += char(0xF0 | (cp >> 18));      s += char(0x80 | ((cp >> 12) & 0x3F)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
}

// diff-flush: only touch changed cells; track cursor/SGR state to minimize bytes
void Terminal::flush() {
    out_.clear();
    if (clearPending_) { out_ += "\e[2J"; clearPending_ = false; }
    int cx = -2, cy = -2;                 // "virtual cursor" position assumed
    Color lastFg = Color(0xFFFFFFFE), lastBg = Color(0xFFFFFFFE);

    auto cup = [&](int x, int y) {
        char b[32]; snprintf(b, sizeof b, "\e[%d;%dH", y + 1, x + 1);
        out_ += b; cx = x; cy = y;
    };
    // emit one cell and advance the virtual cursor by the glyph's column span
    auto emit = [&](const Cell& c, int& x, int& y) {
        if (c.fg != lastFg) {
            if (c.fg == NONE) out_ += "\e[39m";
            else { char b[32]; snprintf(b, sizeof b, "\e[38;2;%d;%d;%dm", int((c.fg >> 16) & 255), int((c.fg >> 8) & 255), int(c.fg & 255)); out_ += b; }
            lastFg = c.fg;
        }
        if (c.bg != lastBg) {
            if (c.bg == NONE) out_ += "\e[49m";   // transparency-friendly
            else { char b[32]; snprintf(b, sizeof b, "\e[48;2;%d;%d;%dm", int((c.bg >> 16) & 255), int((c.bg >> 8) & 255), int(c.bg & 255)); out_ += b; }
            lastBg = c.bg;
        }
        int adv = cpWidth(c.ch);
        if (adv < 1) adv = 1;
        char32_t ch = c.ch ? c.ch : U' ';
        // a wide glyph in the last column would wrap: degrade to a space there
        if (adv == 2 && x + 1 >= w_) { adv = 1; ch = U' '; }
        appendUtf8(out_, ch);
        if (x + adv >= w_) { cx = -2; cy = -2; }   // EOL: distrust the wrap position
        else { cx = x + adv; cy = y; }
        x += adv;
    };

    for (int y = 0; y < h_; y++) {
        int x = 0;
        while (x < w_) {
            const Cell& a = cur_[size_t(y) * w_ + x];
            const Cell& b = last_[size_t(y) * w_ + x];
            if (a == b && !a.cont) { ++x; continue; }
            bool force = false;
            if (a.cont && x > 0) { --x; force = true; }   // re-emit the wide lead
            if (y != cy || x != cx) cup(x, y);
            while (x < w_) {
                size_t j = size_t(y) * w_ + x;
                const Cell& ca = cur_[j];
                if (ca.cont) { ++x; continue; }           // covered by its lead
                if (!force && last_[j] == ca) break;      // run ended
                force = false;
                emit(ca, x, y);
                if (x >= w_) break;
            }
        }
    }
    out_ += "\e[49m";
    if (!out_.empty()) writeStr(out_);

    // swap: 'last_' holds the just-emitted frame; cur_ becomes a blank canvas
    cur_.swap(last_);
    std::fill(cur_.begin(), cur_.end(), Cell{});
}

// ---- input parsing ----
bool Terminal::readOne(uint8_t& b) { return ::read(STDIN_FILENO, &b, 1) == 1; }

bool Terminal::poll(Input& out) {
    parseStream();
    if (pending_.empty()) return false;
    out = pending_.front();
    pending_.erase(pending_.begin());
    return true;
}

int Terminal::decodeUtf8Push(size_t at) {
    auto& v = inBuf_;
    uint8_t c0 = v[at];
    int need = c0 < 0x80 ? 1 : (c0 >> 5) == 0x6 ? 2 : (c0 >> 4) == 0xE ? 3 : (c0 >> 3) == 0x1E ? 4 : 1;
    if (need > int(v.size() - at)) return 0; // wait for more bytes
    char32_t cp = c0 & (need == 1 ? 0x7F : need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
    for (int k = 1; k < need; k++) {
        if ((v[at + k] & 0xC0) != 0x80) return need; // malformed: skip head byte
        cp = (cp << 6) | (v[at + k] & 0x3F);
    }
    Input ev; ev.kind = Input::Key; ev.key = int(cp);
    pending_.push_back(ev);
    return need;
}

void Terminal::decodeCsi(const std::string& params, uint8_t final) {
    if (final == 'u') {
        // kitty keyboard protocol: CSI codepoint;modifier u. Ctrl+9 / Ctrl+0
        // are the reload bindings (mod 5 = ctrl, 6 = ctrl+shift); Ctrl+R was
        // dropped because it collides with the terminal's own shortcuts.
        int cp = 0, mod = 0;
        if (std::sscanf(params.c_str(), "%d;%d", &cp, &mod) == 2 && mod >= 5 &&
            (cp == '9' || cp == '0')) {
            Input ev; ev.kind = Input::Key; ev.key = VK_RELOAD;
            pending_.push_back(ev);
        }
        return;
    }
    Input ev;
    bool isMouse = !params.empty() && params[0] == '<';
    if (isMouse && (final == 'M' || final == 'm')) {
        int btn = 0, mx = 1, my = 1;
        std::sscanf(params.c_str(), "<%d;%d;%d", &btn, &mx, &my);
        --mx; --my;
        ev.kind = Input::MouseEv;
        if (btn >= 64 && btn <= 67) {
            // SGR/xterm: 64 = wheel UP, 65 = wheel DOWN, 66/67 = horizontal.
            // (This used to map 64 AND 65 to WheelUp, so scrolling down went up.)
            if (btn == 66 || btn == 67) return;
            ev.m.type = (btn == 64) ? Mouse::WheelUp : Mouse::WheelDown;
        }
        else if (final == 'm') ev.m.type = Mouse::Release;
        else if (btn >= 32 && btn <= 34) ev.m.type = Mouse::Motion;
        else ev.m.type = Mouse::Press;
        ev.m.x = mx; ev.m.y = my;
        pending_.push_back(ev);
        return;
    }
    switch (final) {
        case 'A': ev.key = VK_UP; break;      // virtual key codes (above unicode BMP range)
        case 'B': ev.key = VK_DOWN; break;
        case 'C': ev.key = VK_RIGHT; break;
        case 'D': ev.key = VK_LEFT; break;
        case 'H': ev.key = VK_HOME; break;
        case 'F': ev.key = VK_END; break;
        case '~': {
            int n = std::atoi(params.c_str());
            if (n == 3) ev.key = VK_DEL; else return;
            break;
        }
        default: return;
    }
    ev.kind = Input::Key;
    pending_.push_back(ev);
}

void Terminal::parseStream() {
    uint8_t b;
    const size_t CAP = 4096;
    while (readOne(b)) {
        if (b == 0x03) { Input ev; ev.kind = Input::Quit; pending_.push_back(ev); continue; }
        if (b == 0x1B) { parseState_ = 1; continue; }
        if (parseState_ == 0) {
            inBuf_.push_back(b);
            if (inBuf_.size() > CAP) { Input ev; ev.kind = Input::Key; ev.key = int(b); pending_.push_back(ev); inBuf_.clear(); continue; }
            if (b < 0x80) { // fast path: single byte printable/control
                Input ev; ev.kind = Input::Key; ev.key = b;
                if (b == '\r' || b == '\n') ev.key = '\n';
                // Ctrl+9 / Ctrl+0: terminals without the kitty protocol send
                // the bare digit (0x39 / 0x30), so the digits double as the
                // config-reload key. Nothing else binds 9 or 0.
                else if (b == '9' || b == '0') ev.key = VK_RELOAD;
                pending_.push_back(ev);
                inBuf_.clear();
                continue;
            }
            int used = decodeUtf8Push(inBuf_.size() - 1);
            if (used > 0) inBuf_.clear();
            else if (inBuf_.size() >= 6) inBuf_.clear(); // malformed multi-byte, drop
            continue;
        } else if (parseState_ == 1) {
            if (b == '[') { parseState_ = 2; csi_.clear(); continue; }
            if (b == 'O') { continue; } // SS3
            parseState_ = 0;
            continue;
        } else { // CSI
            if ((b >= '0' && b <= '9') || b == ';' || b == '<' || b == '?') { csi_ += char(b); continue; }
            if (b >= 0x40 && b <= 0x7E) { decodeCsi(csi_, b); parseState_ = 0; continue; }
            parseState_ = 0;
        }
    }
    if (pending_.size() > CAP) pending_.clear(); // flood guard
}

} // namespace qp
