#pragma once
// Minimal, embedded-friendly ncurses-style header (C++)
// Hardware agnostic via ICursesDisplay / ICursesInput / IFont.
// License: MIT

#include <cstdint>
#include <cstdarg>
#include <cstdio>

namespace jsi::ecurses
{

// ---------- Keys ----------
enum : int
{
    KEY_NONE      = -1,
    KEY_UP        = 0x1100,
    KEY_DOWN      = 0x1101,
    KEY_LEFT      = 0x1102,
    KEY_RIGHT     = 0x1103,
    KEY_ENTER     = 0x1104,
    KEY_BACKSPACE = 0x1105,
    KEY_HOME      = 0x1106,
    KEY_END       = 0x1107,
    KEY_PGUP      = 0x1108,
    KEY_PGDN      = 0x1109,
    KEY_F1        = 0x1111,
    KEY_F2        = 0x1112,
    KEY_F3        = 0x1113,
    KEY_F4        = 0x1114,
    KEY_F5        = 0x1115,
    KEY_F6        = 0x1116,
    KEY_F7        = 0x1117,
    KEY_F8        = 0x1118,
    KEY_F9        = 0x1119,
    KEY_F10       = 0x111A,
};

// ---------- Attr & colors ----------
using attr_t = uint16_t;  // low 8: flags, high 8: color pair index

enum : attr_t
{
    A_NORMAL    = 0,
    A_BOLD      = 1u << 0,
    A_UNDERLINE = 1u << 1,
    A_REVERSE   = 1u << 2,
    A_DIM       = 1u << 3,
};

struct Color
{
    uint8_t r, g, b;
};
struct ColorPair
{
    Color fg, bg;
};

constexpr int MAX_COLOR_PAIRS = 16;

inline constexpr uint16_t COLOR_PAIR(uint8_t idx)
{
    return (uint16_t) (idx) << 8;
}
inline constexpr uint8_t PAIR_NUMBER(uint16_t a)
{
    return (a >> 8) & 0xFF;
}

// ---------- Abstractions ----------
struct IFont
{
    virtual ~IFont() {}
    virtual uint8_t glyph_width() const  = 0;
    virtual uint8_t glyph_height() const = 0;
    // 1bpp glyph, row-major, rows padded to byte boundary
    virtual const uint8_t* glyph_bitmap(uint8_t ch) const = 0;
};

// Backend gets ch, fg/bg, and resolved style bits (bold/reverse) for convenience.
struct ICursesDisplay
{
    virtual ~ICursesDisplay() {}
    virtual int  width_px() const                               = 0;
    virtual int  height_px() const                              = 0;
    virtual void fill_rect(int x, int y, int w, int h, Color c) = 0;

    virtual void draw_glyph(int            x,
                            int            y,
                            uint8_t        ch,
                            const uint8_t* mono_bitmap,
                            int            gw,
                            int            gh,
                            Color          fg,
                            Color          bg,
                            bool           bold,
                            bool           reverse) = 0;

    virtual void invert_rect(int x, int y, int w, int h) = 0;
    virtual void present()                               = 0;
};

struct ICursesInput
{
    virtual ~ICursesInput() {}
    virtual int poll_key() = 0;  // non-blocking; KEY_NONE when no input
};

// ---------- Screen buffer ----------
struct Cell
{
    char     ch;
    uint16_t attr;
};

template <int MAX_COLS, int MAX_ROWS>
class ScreenBuffer
{
public:
    ScreenBuffer() : _cols(0), _rows(0) {}
    void resize(int cols, int rows)
    {
        if (cols > MAX_COLS)
            cols = MAX_COLS;
        if (rows > MAX_ROWS)
            rows = MAX_ROWS;
        _cols = cols;
        _rows = rows;
        for (int y = 0; y < _rows; ++y)
            for (int x = 0; x < _cols; ++x)
                at(x, y) = Cell{' ', 0};
    }
    inline Cell&       at(int x, int y) { return _cells[y * MAX_COLS + x]; }
    inline const Cell& at(int x, int y) const { return _cells[y * MAX_COLS + x]; }
    inline int         cols() const { return _cols; }
    inline int         rows() const { return _rows; }

private:
    int  _cols, _rows;
    Cell _cells[MAX_COLS * MAX_ROWS];
};

// ---------- Core ----------
template <int MAX_COLS = 120, int MAX_ROWS = 60>
class Curses
{
public:
    Curses(ICursesDisplay& disp, ICursesInput& in, IFont& font) :
        _disp(disp),
        _in(in),
        _font(font),
        _echo(false),
        _cbreak(true),
        _nodelay(false),
        _timeout_ms(-1),
        _curs_vis(true),
        _curx(0),
        _cury(0),
        _attr(A_NORMAL),
        _pair(0),
        _dirty_all(true)
    {
        int cols = disp.width_px() / font.glyph_width();
        int rows = disp.height_px() / font.glyph_height();
        _buf.resize(cols, rows);
        _pairs[0] = {Color{255, 255, 255}, Color{0, 0, 0}};  // white on black
    }

    // Init/teardown
    void initscr()
    {
        clear();
        refresh();
    }
    void endwin() {}

    // Drawing
    void clear()
    {
        for (int y = 0; y < _buf.rows(); ++y)
            for (int x = 0; x < _buf.cols(); ++x)
                _buf.at(x, y) = Cell{' ', (uint16_t) (_attr | COLOR_PAIR(_pair))};
        _curx = _cury = 0;
        _dirty_all    = true;
    }
    void erase() { clear(); }

    void move(int y, int x)
    {
        if (x >= 0 && x < _buf.cols())
            _curx = x;
        if (y >= 0 && y < _buf.rows())
            _cury = y;
    }

    void addch(char ch)
    {
        if (ch == '\n')
        {
            _curx = 0;
            if (++_cury >= _buf.rows())
                scroll();
            return;
        }
        if (ch == '\r')
        {
            _curx = 0;
            return;
        }
        if (ch == '\b')
        {
            if (_curx > 0)
                --_curx;
            return;
        }
        if (_curx >= _buf.cols())
        {
            _curx = 0;
            if (++_cury >= _buf.rows())
                scroll();
        }
        _buf.at(_curx, _cury) = Cell{ch, (uint16_t) (_attr | COLOR_PAIR(_pair))};
        ++_curx;
    }
    void addstr(const char* s)
    {
        while (*s)
            addch(*s++);
    }
    void mvaddch(int y, int x, char ch)
    {
        move(y, x);
        addch(ch);
    }
    void mvaddstr(int y, int x, const char* s)
    {
        move(y, x);
        addstr(s);
    }

    void printw(const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        vprintw(fmt, ap);
        va_end(ap);
    }
    void mvprintw(int y, int x, const char* fmt, ...)
    {
        move(y, x);
        va_list ap;
        va_start(ap, fmt);
        vprintw(fmt, ap);
        va_end(ap);
    }

    // Attributes / colors
    void attrset(uint16_t a)
    {
        _attr = a & 0x00FFu;
        _pair = PAIR_NUMBER(a);
    }
    void attron(uint16_t a)
    {
        _attr |= (a & 0x00FFu);
        if (a & 0xFF00u)
            _pair = PAIR_NUMBER(a);
    }
    void attroff(uint16_t a) { _attr &= ~(a & 0x00FFu); }

    void init_pair(uint8_t idx, Color fg, Color bg)
    {
        if (idx < MAX_COLOR_PAIRS)
            _pairs[idx] = {fg, bg};
    }

    void border(char ls = '|',
                char rs = '|',
                char ts = '-',
                char bs = '-',
                char tl = '+',
                char tr = '+',
                char bl = '+',
                char br = '+')
    {
        int w = _buf.cols(), h = _buf.rows();
        for (int x = 1; x < w - 1; ++x)
        {
            mvaddch(0, x, ts);
            mvaddch(h - 1, x, bs);
        }
        for (int y = 1; y < h - 1; ++y)
        {
            mvaddch(y, 0, ls);
            mvaddch(y, w - 1, rs);
        }
        mvaddch(0, 0, tl);
        mvaddch(0, w - 1, tr);
        mvaddch(h - 1, 0, bl);
        mvaddch(h - 1, w - 1, br);
    }

    void clrtoeol()
    {
        for (int x = _curx; x < _buf.cols(); ++x)
            _buf.at(x, _cury) = Cell{' ', (uint16_t) (_attr | COLOR_PAIR(_pair))};
    }

    // Input-mode controls
    void nodelay(bool nd) { _nodelay = nd; }
    void timeout(int ms) { _timeout_ms = ms; }
    void echo(bool en) { _echo = en; }
    void cbreak(bool en) { _cbreak = en; }
    void curs_set(bool vis) { _curs_vis = vis; }

    // Input
    int getch()
    {
        if (_timeout_ms == 0 || _nodelay)
            return _in.poll_key();
        const int step   = 1;
        int       waited = 0;
        while (true)
        {
            int k = _in.poll_key();
            if (k != KEY_NONE)
                return k;
            if (_timeout_ms > 0 && waited >= _timeout_ms)
                return KEY_NONE;
            waited += step;
        }
    }

    // Scrolling
    void scroll()
    {
        int w = _buf.cols(), h = _buf.rows();
        for (int y = 1; y < h; ++y)
            for (int x = 0; x < w; ++x)
                _buf.at(x, y - 1) = _buf.at(x, y);
        for (int x = 0; x < w; ++x)
            _buf.at(x, h - 1) = Cell{' ', (uint16_t) (_attr | COLOR_PAIR(_pair))};
        _cury      = h - 1;
        _curx      = 0;
        _dirty_all = true;
    }

    // Rendering
    void refresh()
    {
        const int gw = _font.glyph_width();
        const int gh = _font.glyph_height();
        for (int y = 0; y < _buf.rows(); ++y)
        {
            for (int x = 0; x < _buf.cols(); ++x)
            {
                const Cell&     c        = _buf.at(x, y);
                const uint8_t   ch       = static_cast<uint8_t>(c.ch);
                const uint8_t*  bm       = _font.glyph_bitmap(ch);
                const uint8_t   pair_idx = PAIR_NUMBER(c.attr);
                const ColorPair cp       = _pairs[pair_idx < MAX_COLOR_PAIRS ? pair_idx : 0];
                const bool      reverse  = (c.attr & A_REVERSE) != 0;
                const bool      bold     = (c.attr & A_BOLD) != 0;
                Color           fg       = reverse ? cp.bg : cp.fg;
                Color           bg       = reverse ? cp.fg : cp.bg;
                _disp.draw_glyph(x * gw, y * gh, ch, bm, gw, gh, fg, bg, bold, reverse);
            }
        }
        if (_curs_vis)
        {
            _disp.invert_rect(_curx * _font.glyph_width(),
                              _cury * _font.glyph_height(),
                              _font.glyph_width(),
                              _font.glyph_height());
        }
        _disp.present();
        _dirty_all = false;
    }

    // Size
    int cols() const { return _buf.cols(); }
    int rows() const { return _buf.rows(); }
    int curx() const { return _curx; }
    int cury() const { return _cury; }

    // Optional helpers (useful in demos)
    uint16_t  cell_attr(int y, int x) const { return _buf.at(x, y).attr; }
    ColorPair get_pair(uint8_t idx) const { return (idx < MAX_COLOR_PAIRS) ? _pairs[idx] : _pairs[0]; }

private:
    void vprintw(const char* fmt, va_list ap)
    {
        char b[256];
        std::vsnprintf(b, sizeof(b), fmt, ap);
        addstr(b);
    }

    ICursesDisplay& _disp;
    ICursesInput&   _in;
    IFont&          _font;

    ScreenBuffer<MAX_COLS, MAX_ROWS> _buf;

    bool _echo, _cbreak, _nodelay;
    int  _timeout_ms;
    bool _curs_vis;

    int      _curx, _cury;
    uint16_t _attr;
    uint8_t  _pair;
    bool     _dirty_all;

    ColorPair _pairs[MAX_COLOR_PAIRS];
};

// ---------- Global wrappers (optional) ----------
inline Curses<>* g_active = nullptr;
inline void      set_active(Curses<>& c)
{
    g_active = &c;
}
inline Curses<>& scr()
{
    return *g_active;
}

inline void initscr()
{
    scr().initscr();
}
inline void endwin()
{
    scr().endwin();
}
inline void clear()
{
    scr().clear();
}
inline void erase()
{
    scr().erase();
}
inline void refresh()
{
    scr().refresh();
}
inline void move(int y, int x)
{
    scr().move(y, x);
}
inline void addch(char ch)
{
    scr().addch(ch);
}
inline void addstr(const char* s)
{
    scr().addstr(s);
}
inline void mvaddch(int y, int x, char ch)
{
    scr().mvaddch(y, x, ch);
}
inline void mvaddstr(int y, int x, const char* s)
{
    scr().mvaddstr(y, x, s);
}
inline void printw(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char b[256];
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    scr().addstr(b);
}
inline void mvprintw(int y, int x, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char b[256];
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    scr().move(y, x);
    scr().addstr(b);
}
inline void attrset(uint16_t a)
{
    scr().attrset(a);
}
inline void attron(uint16_t a)
{
    scr().attron(a);
}
inline void attroff(uint16_t a)
{
    scr().attroff(a);
}
inline void init_pair(uint8_t idx, Color fg, Color bg)
{
    scr().init_pair(idx, fg, bg);
}
inline void border(char ls = '|',
                   char rs = '|',
                   char ts = '-',
                   char bs = '-',
                   char tl = '+',
                   char tr = '+',
                   char bl = '+',
                   char br = '+')
{
    scr().border(ls, rs, ts, bs, tl, tr, bl, br);
}
inline void clrtoeol()
{
    scr().clrtoeol();
}
inline void nodelay(bool nd)
{
    scr().nodelay(nd);
}
inline void timeout(int ms)
{
    scr().timeout(ms);
}
inline void echo(bool en)
{
    scr().echo(en);
}
inline void cbreak(bool en)
{
    scr().cbreak(en);
}
inline void curs_set(bool vis)
{
    scr().curs_set(vis);
}
inline int getch()
{
    return scr().getch();
}
inline int COLS()
{
    return scr().cols();
}
inline int LINES()
{
    return scr().rows();
}
inline void getyx(int& y, int& x)
{
    y = scr().cury();
    x = scr().curx();
}

}  // namespace jsi::ecurses
