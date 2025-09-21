/*
 * Copyright (c) 2025, Joe Inman
 *
 * Licensed under the MIT License.
 * You may obtain a copy of the License at:
 *     https://opensource.org/licenses/MIT
 *
 * This file is part of the EmbedCurses Library.
 */

#pragma once

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

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

inline constexpr char ACS_ULCORNER = '+';
inline constexpr char ACS_URCORNER = '+';
inline constexpr char ACS_LLCORNER = '+';
inline constexpr char ACS_LRCORNER = '+';
inline constexpr char ACS_HLINE    = '-';
inline constexpr char ACS_VLINE    = '|';
inline constexpr char ACS_PLUS     = '+';

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
    class Window
    {
    public:
        friend class Curses;
        Window(Curses& parent, Window* parent_win, int height, int width, int starty, int startx) :
            _parent(parent),
            _parent_win(parent_win),
            _height(0),
            _width(0),
            _starty(0),
            _startx(0),
            _curx(0),
            _cury(0),
            _attr(parent._attr),
            _pair(parent._pair),
            _has_border(false)
        {
            const int max_cols = parent._buf.cols();
            const int max_rows = parent._buf.rows();
            if (max_cols > 0 && max_rows > 0)
            {
                _startx = std::clamp(startx, 0, max_cols - 1);
                _starty = std::clamp(starty, 0, max_rows - 1);
                _width  = std::clamp(width, 1, max_cols - _startx);
                _height = std::clamp(height, 1, max_rows - _starty);
            }
        }

        void move(int y, int x)
        {
            if (!has_area())
                return;
            _cury = std::clamp(y, 0, _height - 1);
            _curx = std::clamp(x, 0, _width - 1);
        }

        void addch(char ch)
        {
            if (!has_area())
                return;

            const int left        = inner_left();
            const int top         = inner_top();
            const int right_excl  = inner_right_exclusive();
            const int bottom_excl = inner_bottom_exclusive();
            if (right_excl <= left || bottom_excl <= top)
                return;

            _curx = std::clamp(_curx, left, right_excl - 1);
            _cury = std::clamp(_cury, top, bottom_excl - 1);

            if (ch == '\n')
            {
                _curx = left;
                if (++_cury >= bottom_excl)
                    scroll();
                return;
            }
            if (ch == '\r')
            {
                _curx = left;
                return;
            }
            if (ch == '\b')
            {
                if (_curx > left)
                    --_curx;
                return;
            }
            if (_curx >= right_excl)
            {
                _curx = left;
                if (++_cury >= bottom_excl)
                    scroll();
            }
            write_cell(_curx, _cury, ch, current_attr());
            ++_curx;
        }

        void addstr(const char* s)
        {
            if (!s)
                return;
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

        Window* derwin(int height, int width, int starty, int startx)
        {
            const int rel_x = std::clamp(startx, 0, _width > 0 ? _width - 1 : 0);
            const int rel_y = std::clamp(starty, 0, _height > 0 ? _height - 1 : 0);
            const int abs_x = _startx + rel_x;
            const int abs_y = _starty + rel_y;
            const int max_w = std::max(1, _width - rel_x);
            const int max_h = std::max(1, _height - rel_y);
            Window*   child =
                _parent.newsubwin(this, std::clamp(height, 1, max_h), std::clamp(width, 1, max_w), abs_y, abs_x);
            if (child)
            {
                child->_attr = _attr;
                child->_pair = _pair;
            }
            return child;
        }

        Window* subwin(int height, int width, int starty, int startx)
        {
            Window* child = _parent.newsubwin(this, height, width, starty, startx);
            if (child)
            {
                child->_attr = _attr;
                child->_pair = _pair;
            }
            return child;
        }

        void printw(const char* fmt, ...)
        {
            if (!fmt)
                return;
            va_list ap;
            va_start(ap, fmt);
            vprintw(fmt, ap);
            va_end(ap);
        }

        void mvprintw(int y, int x, const char* fmt, ...)
        {
            move(y, x);
            if (!fmt)
                return;
            va_list ap;
            va_start(ap, fmt);
            vprintw(fmt, ap);
            va_end(ap);
        }

        void hline(char ch, int n)
        {
            if (!has_area() || n <= 0)
                return;
            const int left        = inner_left();
            const int right_excl  = inner_right_exclusive();
            const int top         = inner_top();
            const int bottom_excl = inner_bottom_exclusive();
            if (right_excl <= left || bottom_excl <= top)
                return;

            const int      row   = std::clamp(_cury, top, bottom_excl - 1);
            int            col   = std::clamp(_curx, left, right_excl - 1);
            const int      limit = std::min(std::max(right_excl - col, 0), n);
            const uint16_t attr  = current_attr();
            for (int i = 0; i < limit; ++i)
                write_cell(col + i, row, ch, attr);
            if (limit > 0)
                _curx = col + limit - 1;
            _cury = row;
        }

        void vline(char ch, int n)
        {
            if (!has_area() || n <= 0)
                return;
            const int left        = inner_left();
            const int right_excl  = inner_right_exclusive();
            const int top         = inner_top();
            const int bottom_excl = inner_bottom_exclusive();
            if (right_excl <= left || bottom_excl <= top)
                return;

            const int      col   = std::clamp(_curx, left, right_excl - 1);
            int            row   = std::clamp(_cury, top, bottom_excl - 1);
            const int      limit = std::min(std::max(bottom_excl - row, 0), n);
            const uint16_t attr  = current_attr();
            for (int i = 0; i < limit; ++i)
                write_cell(col, row + i, ch, attr);
            if (limit > 0)
                _cury = row + limit - 1;
            _curx = col;
        }

        void box(char vert = ACS_VLINE, char horiz = ACS_HLINE)
        {
            border(vert, vert, horiz, horiz, ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
        }

        void touchwin(bool recurse = true)
        {
            _parent._dirty_all = true;
            if (recurse)
                for (Window* child : _children)
                    if (child)
                        child->touchwin(true);
        }

        void clear()
        {
            if (!has_area())
                return;
            const uint16_t attr = current_attr();
            for (int y = 0; y < _height; ++y)
                fill_row(y, 0, attr);
            _curx       = 0;
            _cury       = 0;
            _has_border = false;
        }

        void erase() { clear(); }

        void border(char ls = '|',
                    char rs = '|',
                    char ts = '-',
                    char bs = '-',
                    char tl = '+',
                    char tr = '+',
                    char bl = '+',
                    char br = '+')
        {
            if (_width <= 1 || _height <= 1)
            {
                _has_border = false;
                return;
            }

            const uint16_t attr     = current_attr();
            const int      last_col = _width - 1;
            const int      last_row = _height - 1;

            write_cell(0, 0, tl, attr);
            write_cell(last_col, 0, tr, attr);
            write_cell(0, last_row, bl, attr);
            write_cell(last_col, last_row, br, attr);

            for (int x = 1; x < last_col; ++x)
            {
                write_cell(x, 0, ts, attr);
                write_cell(x, last_row, bs, attr);
            }
            for (int y = 1; y < last_row; ++y)
            {
                write_cell(0, y, ls, attr);
                write_cell(last_col, y, rs, attr);
            }

            _has_border    = true;
            const int left = inner_left();
            const int top  = inner_top();
            if (inner_right_exclusive() > left && inner_bottom_exclusive() > top)
            {
                _curx = left;
                _cury = top;
            }
        }

        void clrtoeol()
        {
            if (!has_area())
                return;
            const int left        = inner_left();
            const int right_excl  = inner_right_exclusive();
            const int top         = inner_top();
            const int bottom_excl = inner_bottom_exclusive();
            if (right_excl <= left || bottom_excl <= top)
                return;
            if (_cury < top || _cury >= bottom_excl)
                return;

            const uint16_t attr  = current_attr();
            const int      start = std::clamp(_curx, left, right_excl - 1);
            for (int x = start; x < right_excl; ++x)
                write_cell(x, _cury, ' ', attr);
        }

        void refresh()
        {
            if (!has_area())
            {
                _parent.refresh();
                return;
            }
            const int abs_x = _startx + _curx;
            const int abs_y = _starty + _cury;
            _parent.refresh_with_cursor(abs_y, abs_x);
        }

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

        void getyx(int& y, int& x) const
        {
            y = _cury;
            x = _curx;
        }

        void getmaxyx(int& y, int& x) const
        {
            y = _height;
            x = _width;
        }

        void getbegyx(int& y, int& x) const
        {
            y = _starty;
            x = _startx;
        }

        int getch() { return _parent.getch(); }

        void keypad(bool en) { _parent.keypad(en); }

    private:
        bool has_area() const { return _width > 0 && _height > 0; }

        int inner_left() const { return _has_border ? 1 : 0; }
        int inner_top() const { return _has_border ? 1 : 0; }
        int inner_right_exclusive() const { return _has_border ? std::max(_width - 1, inner_left()) : _width; }
        int inner_bottom_exclusive() const { return _has_border ? std::max(_height - 1, inner_top()) : _height; }

        void scroll()
        {
            if (!has_area())
                return;
            const int left        = inner_left();
            const int right_excl  = inner_right_exclusive();
            const int top         = inner_top();
            const int bottom_excl = inner_bottom_exclusive();
            if (right_excl <= left || bottom_excl <= top)
                return;

            for (int y = top + 1; y < bottom_excl; ++y)
                for (int x = left; x < right_excl; ++x)
                    cell_at(x, y - 1) = cell_at(x, y);
            const uint16_t attr = current_attr();
            for (int x = left; x < right_excl; ++x)
                write_cell(x, bottom_excl - 1, ' ', attr);
            _cury = bottom_excl - 1;
            _curx = left;
        }

        void vprintw(const char* fmt, va_list ap)
        {
            char b[256];
            std::vsnprintf(b, sizeof(b), fmt, ap);
            addstr(b);
        }

        Cell& cell_at(int x, int y) { return _parent._buf.at(_startx + x, _starty + y); }

        const Cell& cell_at(int x, int y) const { return _parent._buf.at(_startx + x, _starty + y); }

        void write_cell(int x, int y, char ch, uint16_t attr)
        {
            if (x < 0 || x >= _width || y < 0 || y >= _height)
                return;
            cell_at(x, y) = Cell{ch, attr};
        }

        void fill_row(int y, int from_x, uint16_t attr)
        {
            for (int x = from_x; x < _width; ++x)
                write_cell(x, y, ' ', attr);
        }

        uint16_t current_attr() const { return static_cast<uint16_t>(_attr | COLOR_PAIR(_pair)); }

        Curses&              _parent;
        Window*              _parent_win;
        int                  _height, _width;
        int                  _starty, _startx;
        int                  _curx, _cury;
        uint16_t             _attr;
        uint8_t              _pair;
        bool                 _has_border;
        std::vector<Window*> _children;

        void detach_child(Window* child)
        {
            auto it = std::remove(_children.begin(), _children.end(), child);
            if (it != _children.end())
                _children.erase(it, _children.end());
        }
    };

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
        _keypad(false),
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

    Window* newwin(int height, int width, int starty, int startx)
    {
        return create_window(nullptr, height, width, starty, startx);
    }

    Window* subwin(int height, int width, int starty, int startx)
    {
        return create_window(nullptr, height, width, starty, startx);
    }

    void delwin(Window* win)
    {
        if (!win)
            return;
        destroy_window(win);
    }

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
        while (s && *s)
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
        if (!fmt)
            return;
        va_list ap;
        va_start(ap, fmt);
        vprintw(fmt, ap);
        va_end(ap);
    }
    void mvprintw(int y, int x, const char* fmt, ...)
    {
        move(y, x);
        if (!fmt)
            return;
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
    void keypad(bool en) { _keypad = en; }
    void touchwin() { _dirty_all = true; }

    // Input
    int getch()
    {
        if (!_pending_keys.empty())
        {
            int pending = _pending_keys.front();
            _pending_keys.pop_front();
            return pending;
        }

        int key = read_primary_key();
        if (key == KEY_NONE)
            return KEY_NONE;
        return translate_key(key);
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
    void refresh() { render(false, 0, 0); }

    void refresh_with_cursor(int y, int x) { render(true, y, x); }

    // Size
    int cols() const { return _buf.cols(); }
    int rows() const { return _buf.rows(); }
    int curx() const { return _curx; }
    int cury() const { return _cury; }

    // Optional helpers (useful in demos)
    uint16_t  cell_attr(int y, int x) const { return _buf.at(x, y).attr; }
    ColorPair get_pair(uint8_t idx) const { return (idx < MAX_COLOR_PAIRS) ? _pairs[idx] : _pairs[0]; }

private:
    int read_primary_key()
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

    int read_followup_key(int wait_ms)
    {
        if (_nodelay || wait_ms <= 0)
            return _in.poll_key();
        const int step   = 1;
        int       waited = 0;
        while (waited <= wait_ms)
        {
            int k = _in.poll_key();
            if (k != KEY_NONE)
                return k;
            waited += step;
        }
        return KEY_NONE;
    }

    void queue_pending(const std::vector<int>& seq)
    {
        for (int v : seq)
            _pending_keys.push_back(v);
    }

    int translate_key(int first)
    {
        if (!_keypad || first != 27)
            return first;

        constexpr int    FOLLOWUP_WAIT_MS = 10;
        std::vector<int> consumed;

        int second = read_followup_key(FOLLOWUP_WAIT_MS);
        if (second == KEY_NONE)
            return first;  // bare ESC
        consumed.push_back(second);

        if (second == '[')
        {
            int third = read_followup_key(FOLLOWUP_WAIT_MS);
            if (third == KEY_NONE)
            {
                queue_pending(consumed);
                return first;
            }
            consumed.push_back(third);

            switch (third)
            {
            case 'A':
                return KEY_UP;
            case 'B':
                return KEY_DOWN;
            case 'C':
                return KEY_RIGHT;
            case 'D':
                return KEY_LEFT;
            case 'H':
                return KEY_HOME;
            case 'F':
                return KEY_END;
            default:
                break;
            }

            if (third >= '0' && third <= '9')
            {
                int code = third - '0';
                while (true)
                {
                    int next = read_followup_key(FOLLOWUP_WAIT_MS);
                    if (next == KEY_NONE)
                    {
                        queue_pending(consumed);
                        return first;
                    }
                    consumed.push_back(next);
                    if (next >= '0' && next <= '9')
                    {
                        code = code * 10 + (next - '0');
                        continue;
                    }
                    if (next == '~')
                    {
                        switch (code)
                        {
                        case 1:
                        case 7:
                            return KEY_HOME;
                        case 4:
                        case 8:
                            return KEY_END;
                        case 5:
                            return KEY_PGUP;
                        case 6:
                            return KEY_PGDN;
                        case 15:
                            return KEY_F5;
                        case 17:
                            return KEY_F6;
                        case 18:
                            return KEY_F7;
                        case 19:
                            return KEY_F8;
                        case 20:
                            return KEY_F9;
                        case 21:
                            return KEY_F10;
                        default:
                            queue_pending(consumed);
                            return first;
                        }
                    }

                    queue_pending(consumed);
                    return first;
                }
            }

            queue_pending(consumed);
            return first;
        }
        else if (second == 'O')
        {
            int third = read_followup_key(FOLLOWUP_WAIT_MS);
            if (third == KEY_NONE)
            {
                queue_pending(consumed);
                return first;
            }
            consumed.push_back(third);
            switch (third)
            {
            case 'P':
                return KEY_F1;
            case 'Q':
                return KEY_F2;
            case 'R':
                return KEY_F3;
            case 'S':
                return KEY_F4;
            default:
                queue_pending(consumed);
                return first;
            }
        }
        else
        {
            queue_pending(consumed);
            return first;
        }
    }

    void render(bool override_cursor, int cursor_y, int cursor_x)
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
            const int draw_x = override_cursor ? cursor_x : _curx;
            const int draw_y = override_cursor ? cursor_y : _cury;
            if (draw_x >= 0 && draw_x < _buf.cols() && draw_y >= 0 && draw_y < _buf.rows())
            {
                _disp.invert_rect(draw_x * gw, draw_y * gh, gw, gh);
            }
        }
        _disp.present();
        _dirty_all = false;
    }

    void vprintw(const char* fmt, va_list ap)
    {
        char b[256];
        std::vsnprintf(b, sizeof(b), fmt, ap);
        addstr(b);
    }

    friend class Window;

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
    bool     _keypad;
    bool     _dirty_all;

    ColorPair                            _pairs[MAX_COLOR_PAIRS];
    std::deque<int>                      _pending_keys;
    std::vector<std::unique_ptr<Window>> _windows;

    Window* create_window(Window* parent_win, int height, int width, int starty, int startx)
    {
        auto    win = std::make_unique<Window>(*this, parent_win, height, width, starty, startx);
        Window* ptr = win.get();
        _windows.push_back(std::move(win));
        if (parent_win)
            parent_win->_children.push_back(ptr);
        return ptr;
    }

    Window* newsubwin(Window* parent_win, int height, int width, int starty, int startx)
    {
        return create_window(parent_win, height, width, starty, startx);
    }

    void destroy_window(Window* win)
    {
        if (!win)
            return;

        auto children = win->_children;
        for (Window* child : children)
            destroy_window(child);
        win->_children.clear();

        if (win->_parent_win)
            win->_parent_win->detach_child(win);

        auto it = std::find_if(_windows.begin(), _windows.end(), [win](const auto& candidate) {
            return candidate.get() == win;
        });
        if (it != _windows.end())
        {
            win->_parent._dirty_all = true;
            _windows.erase(it);
        }
    }
};

// ---------- Global wrappers (optional) ----------
inline Curses<>* g_active = nullptr;
using WINDOW              = Curses<>::Window;

inline void set_active(Curses<>& c)
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

inline WINDOW* newwin(int nlines, int ncols, int begin_y, int begin_x)
{
    return scr().newwin(nlines, ncols, begin_y, begin_x);
}
inline void delwin(WINDOW* win)
{
    scr().delwin(win);
}
inline WINDOW* subwin(int nlines, int ncols, int begin_y, int begin_x)
{
    return scr().subwin(nlines, ncols, begin_y, begin_x);
}
inline WINDOW* subwin(WINDOW* win, int nlines, int ncols, int begin_y, int begin_x)
{
    return win ? win->subwin(nlines, ncols, begin_y, begin_x) : nullptr;
}
inline WINDOW* derwin(WINDOW* win, int nlines, int ncols, int begin_y, int begin_x)
{
    return win ? win->derwin(nlines, ncols, begin_y, begin_x) : nullptr;
}
inline void wrefresh(WINDOW* win)
{
    if (win)
        win->refresh();
}
inline void wclear(WINDOW* win)
{
    if (win)
        win->clear();
}
inline void werase(WINDOW* win)
{
    if (win)
        win->erase();
}
inline void wborder(WINDOW* win,
                    char    ls = '|',
                    char    rs = '|',
                    char    ts = '-',
                    char    bs = '-',
                    char    tl = '+',
                    char    tr = '+',
                    char    bl = '+',
                    char    br = '+')
{
    if (win)
        win->border(ls, rs, ts, bs, tl, tr, bl, br);
}
inline void wmove(WINDOW* win, int y, int x)
{
    if (win)
        win->move(y, x);
}
inline void waddch(WINDOW* win, char ch)
{
    if (win)
        win->addch(ch);
}
inline void waddstr(WINDOW* win, const char* s)
{
    if (win)
        win->addstr(s);
}
inline void mvwaddch(WINDOW* win, int y, int x, char ch)
{
    if (win)
        win->mvaddch(y, x, ch);
}
inline void mvwaddstr(WINDOW* win, int y, int x, const char* s)
{
    if (win)
        win->mvaddstr(y, x, s);
}
inline void wprintw(WINDOW* win, const char* fmt, ...)
{
    if (!win || !fmt)
        return;
    va_list ap;
    va_start(ap, fmt);
    char b[256];
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    win->addstr(b);
}
inline void mvwprintw(WINDOW* win, int y, int x, const char* fmt, ...)
{
    if (!win || !fmt)
        return;
    va_list ap;
    va_start(ap, fmt);
    char b[256];
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (win)
    {
        win->move(y, x);
        win->addstr(b);
    }
}
inline void wattrset(WINDOW* win, uint16_t a)
{
    if (win)
        win->attrset(a);
}
inline void wattron(WINDOW* win, uint16_t a)
{
    if (win)
        win->attron(a);
}
inline void wattroff(WINDOW* win, uint16_t a)
{
    if (win)
        win->attroff(a);
}
inline void wclrtoeol(WINDOW* win)
{
    if (win)
        win->clrtoeol();
}
inline int wgetch(WINDOW* win)
{
    return win ? win->getch() : KEY_NONE;
}
inline void keypad(bool en)
{
    scr().keypad(en);
}
inline void keypad(WINDOW* win, bool en)
{
    if (win)
        win->keypad(en);
    else
        scr().keypad(en);
}
inline void getyx(WINDOW* win, int& y, int& x)
{
    if (win)
    {
        win->getyx(y, x);
    }
    else
    {
        y = 0;
        x = 0;
    }
}
inline void getbegyx(WINDOW* win, int& y, int& x)
{
    if (win)
    {
        win->getbegyx(y, x);
    }
    else
    {
        y = 0;
        x = 0;
    }
}
inline void getmaxyx(WINDOW* win, int& y, int& x)
{
    if (win)
    {
        win->getmaxyx(y, x);
    }
    else
    {
        y = 0;
        x = 0;
    }
}

inline void touchwin(WINDOW* win)
{
    if (win)
        win->touchwin(true);
    else
        scr().touchwin();
}
inline void whline(WINDOW* win, char ch, int n)
{
    if (win)
        win->hline(ch, n);
}
inline void wvline(WINDOW* win, char ch, int n)
{
    if (win)
        win->vline(ch, n);
}
inline void mvwhline(WINDOW* win, int y, int x, char ch, int n)
{
    if (win)
    {
        win->move(y, x);
        win->hline(ch, n);
    }
}
inline void mvwvline(WINDOW* win, int y, int x, char ch, int n)
{
    if (win)
    {
        win->move(y, x);
        win->vline(ch, n);
    }
}
inline void box(WINDOW* win, char vert = ACS_VLINE, char horiz = ACS_HLINE)
{
    if (win)
        win->box(vert, horiz);
}

}  // namespace jsi::ecurses
