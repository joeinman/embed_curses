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
#include <array>
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
    KEY_RESIZE    = 0x1120,
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

inline constexpr int KEYMOD_SHIFT = 1 << 16;
inline constexpr int KEYMOD_ALT   = 1 << 17;
inline constexpr int KEYMOD_CTRL  = 1 << 18;

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
template <int MAX_COLS = 240, int MAX_ROWS = 120>
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

        bool resize(int height, int width)
        {
            if (height <= 0 || width <= 0)
                return false;

            const int max_cols = _parent._buf.cols();
            const int max_rows = _parent._buf.rows();
            if (max_cols <= 0 || max_rows <= 0)
                return false;

            int new_width  = width;
            int new_height = height;
            int new_startx = _startx;
            int new_starty = _starty;
            clamp_to_parent_bounds(new_width, new_height, new_startx, new_starty);

            if (new_width == _width && new_height == _height && new_startx == _startx && new_starty == _starty)
                return true;

            const uint16_t attr = current_attr();

            const int old_startx = _startx;
            const int old_starty = _starty;
            clear_region_abs(old_startx, old_starty, _width, _height, attr);

            int dx = new_startx - _startx;
            int dy = new_starty - _starty;

            _startx = new_startx;
            _starty = new_starty;
            _width  = new_width;
            _height = new_height;
            _curx   = std::clamp(_curx, 0, _width - 1);
            _cury   = std::clamp(_cury, 0, _height - 1);

            fill_region_abs(_startx, _starty, _width, _height, ' ', attr);

            bool had_border = _has_border;
            _has_border     = false;
            if (had_border)
                box();

            if (dx != 0 || dy != 0)
                move_children(dx, dy);
            adjust_children_after_parent_change();
            return true;
        }

        bool mvwin(int starty, int startx)
        {
            const int max_cols = _parent._buf.cols();
            const int max_rows = _parent._buf.rows();
            if (max_cols <= 0 || max_rows <= 0)
                return false;

            startx = std::clamp(startx, 0, max_cols - 1);
            starty = std::clamp(starty, 0, max_rows - 1);

            int target_width  = _width;
            int target_height = _height;
            clamp_to_parent_bounds(target_width, target_height, startx, starty);

            if (startx == _startx && starty == _starty)
                return true;

            const uint16_t attr = current_attr();
            clear_region_abs(_startx, _starty, _width, _height, attr);

            int dx = startx - _startx;
            int dy = starty - _starty;

            _startx = startx;
            _starty = starty;
            _width  = target_width;
            _height = target_height;
            _curx   = std::clamp(_curx, 0, _width - 1);
            _cury   = std::clamp(_cury, 0, _height - 1);

            fill_region_abs(_startx, _starty, _width, _height, ' ', attr);

            bool had_border = _has_border;
            _has_border     = false;
            if (had_border)
                box();

            move_children(dx, dy);
            adjust_children_after_parent_change();
            return true;
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

        void touchline(int y, int n, bool changed = true)
        {
            if (n <= 0)
                return;
            const int start = std::clamp(y, 0, _height > 0 ? _height - 1 : 0);
            const int count = std::min(std::max(_height - start, 0), n);
            if (count <= 0)
                return;
            _parent.touchline_internal(_starty + start, count, changed);
        }

        void untouchwin(bool recurse = true)
        {
            _parent.touchline_internal(_starty, _height, false);
            if (recurse)
                for (Window* child : _children)
                    if (child)
                        child->untouchwin(true);
        }

        bool is_linetouched(int y) const { return _parent.is_linetouched(_starty + y); }

        bool is_wintouched() const { return _parent.is_region_touched(_starty, _height); }

        void touchwin(bool recurse = true)
        {
            _parent.mark_region_dirty(_starty, _height);
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
            _parent.mark_region_dirty(_starty, _height);
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
            _parent.mark_region_dirty(_starty + top, bottom_excl - top);
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
            _parent.mark_line_dirty(_starty + y);
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

        void clear_region_abs(int abs_x, int abs_y, int width, int height, uint16_t attr)
        {
            fill_region_abs(abs_x, abs_y, width, height, ' ', attr);
        }

        void fill_region_abs(int abs_x, int abs_y, int width, int height, char ch, uint16_t attr)
        {
            if (width <= 0 || height <= 0)
                return;
            const int cols = _parent._buf.cols();
            const int rows = _parent._buf.rows();
            for (int ry = 0; ry < height; ++ry)
            {
                int ay = abs_y + ry;
                if (ay < 0 || ay >= rows)
                    continue;
                for (int rx = 0; rx < width; ++rx)
                {
                    int ax = abs_x + rx;
                    if (ax < 0 || ax >= cols)
                        continue;
                    _parent._buf.at(ax, ay) = Cell{ch, attr};
                }
                _parent.mark_line_dirty(ay);
            }
        }

        void clamp_to_parent_bounds(int& width, int& height, int& startx, int& starty) const
        {
            const int max_cols = _parent._buf.cols();
            const int max_rows = _parent._buf.rows();

            int min_x = 0;
            int min_y = 0;
            int max_x = max_cols;
            int max_y = max_rows;

            if (_parent_win)
            {
                min_x = _parent_win->_startx;
                min_y = _parent_win->_starty;
                max_x = _parent_win->_startx + _parent_win->_width;
                max_y = _parent_win->_starty + _parent_win->_height;
            }

            startx = std::clamp(startx, min_x, std::max(min_x, max_x - 1));
            starty = std::clamp(starty, min_y, std::max(min_y, max_y - 1));

            width  = std::clamp(width, 1, std::max(1, max_x - startx));
            height = std::clamp(height, 1, std::max(1, max_y - starty));
        }

        void clamp_to_parent_bounds()
        {
            int width  = _width;
            int height = _height;
            int startx = _startx;
            int starty = _starty;
            clamp_to_parent_bounds(width, height, startx, starty);
            _startx = startx;
            _starty = starty;
            _width  = width;
            _height = height;
            _curx   = std::clamp(_curx, 0, _width - 1);
            _cury   = std::clamp(_cury, 0, _height - 1);
        }

        void adjust_children_after_parent_change()
        {
            for (Window* child : _children)
            {
                if (!child)
                    continue;
                child->clamp_to_parent_bounds();
                child->touchwin(true);
            }
        }

        void move_children(int dx, int dy)
        {
            if (dx == 0 && dy == 0)
                return;
            for (Window* child : _children)
            {
                if (!child)
                    continue;
                child->_startx += dx;
                child->_starty += dy;
            }
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
        mark_all_dirty();
        _resize_pending = false;
        _resize_rows    = rows;
        _resize_cols    = cols;
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
        mark_all_dirty();
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
        mark_line_dirty(_cury);
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
        mark_line_dirty(_cury);
    }

    // Input-mode controls
    void nodelay(bool nd) { _nodelay = nd; }
    void timeout(int ms) { _timeout_ms = ms; }
    void echo(bool en) { _echo = en; }
    void cbreak(bool en) { _cbreak = en; }
    void curs_set(bool vis) { _curs_vis = vis; }
    void keypad(bool en) { _keypad = en; }
    void touchwin() { mark_all_dirty(); }

    void touchline(int y, int n) { touchline_internal(y, n, true); }
    void touchline(int y, int n, bool changed) { touchline_internal(y, n, changed); }

    void untouchwin()
    {
        _dirty_all     = false;
        const int rows = _buf.rows();
        for (int i = 0; i < rows && i < static_cast<int>(_dirty_lines.size()); ++i)
            _dirty_lines[i] = false;
    }

    bool is_linetouched(int y) const
    {
        if (y < 0 || y >= _buf.rows())
            return false;
        return _dirty_all || _dirty_lines[y];
    }

    bool is_wintouched() const
    {
        if (_dirty_all)
            return true;
        const int rows = _buf.rows();
        for (int i = 0; i < rows; ++i)
            if (_dirty_lines[i])
                return true;
        return false;
    }

    bool resizeterm(int new_rows, int new_cols)
    {
        new_cols = std::clamp(new_cols, 1, MAX_COLS);
        new_rows = std::clamp(new_rows, 1, MAX_ROWS);
        if (new_cols == _buf.cols() && new_rows == _buf.rows())
            return false;

        _buf.resize(new_cols, new_rows);
        _curx = std::clamp(_curx, 0, new_cols - 1);
        _cury = std::clamp(_cury, 0, new_rows - 1);
        mark_all_dirty();
        for (int i = new_rows; i < static_cast<int>(_dirty_lines.size()); ++i)
            _dirty_lines[i] = false;

        for (auto& win : _windows)
        {
            if (win)
            {
                win->clamp_to_parent_bounds();
                win->touchwin(true);
            }
        }
        _resize_rows = new_rows;
        _resize_cols = new_cols;
        return true;
    }

    bool consume_resize(int& rows, int& cols)
    {
        if (!_resize_pending)
            return false;
        rows            = std::max(1, _resize_rows);
        cols            = std::max(1, _resize_cols);
        _resize_pending = false;
        return true;
    }

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
        _cury = h - 1;
        _curx = 0;
        mark_all_dirty();
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

        auto apply_modifiers = [](int key, int mod_code) {
            if (mod_code <= 1)
                return key;
            int result = key;
            if (mod_code == 2 || mod_code == 4 || mod_code == 6 || mod_code == 8)
                result |= KEYMOD_SHIFT;
            if (mod_code == 3 || mod_code == 4 || mod_code == 7 || mod_code == 8)
                result |= KEYMOD_ALT;
            if (mod_code == 5 || mod_code == 6 || mod_code == 7 || mod_code == 8)
                result |= KEYMOD_CTRL;
            return result;
        };

        int second = read_followup_key(FOLLOWUP_WAIT_MS);
        if (second == KEY_NONE)
            return first;  // bare ESC
        consumed.push_back(second);

        if (second == '[')
        {
            std::vector<int> params;
            int              current    = 0;
            bool             in_number  = false;
            bool             any_param  = false;
            int              final_char = 0;

            while (true)
            {
                int ch = read_followup_key(FOLLOWUP_WAIT_MS);
                if (ch == KEY_NONE)
                {
                    queue_pending(consumed);
                    return first;
                }
                consumed.push_back(ch);

                if (ch >= '0' && ch <= '9')
                {
                    current   = current * 10 + (ch - '0');
                    in_number = true;
                    any_param = true;
                    continue;
                }
                if (ch == ';')
                {
                    params.push_back(in_number ? current : 0);
                    current   = 0;
                    in_number = false;
                    any_param = true;
                    continue;
                }

                final_char = ch;
                if (in_number || any_param)
                    params.push_back(in_number ? current : 0);
                break;
            }

            if (final_char == 0)
            {
                queue_pending(consumed);
                return first;
            }

            if (final_char == 't')
            {
                if (params.size() >= 3 && params[0] == 8)
                {
                    _resize_rows    = params[1];
                    _resize_cols    = params[2];
                    _resize_pending = true;
                    return KEY_RESIZE;
                }
                queue_pending(consumed);
                return first;
            }

            int mod_code = (params.size() >= 2) ? (params.back() == 0 ? 1 : params.back()) : 1;
            int base     = 0;

            switch (final_char)
            {
            case 'A':
                base = KEY_UP;
                break;
            case 'B':
                base = KEY_DOWN;
                break;
            case 'C':
                base = KEY_RIGHT;
                break;
            case 'D':
                base = KEY_LEFT;
                break;
            case 'H':
                base = KEY_HOME;
                break;
            case 'F':
                base = KEY_END;
                break;
            case 'P':
                base = KEY_F1;
                break;
            case 'Q':
                base = KEY_F2;
                break;
            case 'R':
                base = KEY_F3;
                break;
            case 'S':
                base = KEY_F4;
                break;
            case '~':
            {
                if (params.empty())
                {
                    queue_pending(consumed);
                    return first;
                }
                int code = params[0];
                switch (code)
                {
                case 1:
                case 7:
                    base = KEY_HOME;
                    break;
                case 4:
                case 8:
                    base = KEY_END;
                    break;
                case 5:
                    base = KEY_PGUP;
                    break;
                case 6:
                    base = KEY_PGDN;
                    break;
                case 11:
                    base = KEY_F1;
                    break;
                case 12:
                    base = KEY_F2;
                    break;
                case 13:
                    base = KEY_F3;
                    break;
                case 14:
                    base = KEY_F4;
                    break;
                case 15:
                    base = KEY_F5;
                    break;
                case 17:
                    base = KEY_F6;
                    break;
                case 18:
                    base = KEY_F7;
                    break;
                case 19:
                    base = KEY_F8;
                    break;
                case 20:
                    base = KEY_F9;
                    break;
                case 21:
                    base = KEY_F10;
                    break;
                default:
                    queue_pending(consumed);
                    return first;
                }
                break;
            }
            default:
                queue_pending(consumed);
                return first;
            }

            return apply_modifiers(base, mod_code);
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
            if (!_dirty_all && !_dirty_lines[y])
                continue;
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
            _dirty_lines[y] = false;
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
    std::array<bool, MAX_ROWS>           _dirty_lines{};
    bool                                 _resize_pending = false;
    int                                  _resize_rows    = 0;
    int                                  _resize_cols    = 0;

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
            mark_region_dirty(win->_starty, win->_height);
            _windows.erase(it);
        }
    }

    void mark_line_dirty(int y)
    {
        if (y < 0 || y >= _buf.rows())
            return;
        _dirty_lines[y] = true;
    }

    void mark_region_dirty(int start, int count) { touchline_internal(start, count, true); }

    void mark_all_dirty()
    {
        _dirty_all     = true;
        const int rows = _buf.rows();
        for (int i = 0; i < rows && i < static_cast<int>(_dirty_lines.size()); ++i)
            _dirty_lines[i] = true;
    }

    void touchline_internal(int start, int count, bool value)
    {
        if (count <= 0)
            return;
        int rows = _buf.rows();
        if (rows <= 0)
            return;
        if (start < 0)
        {
            count += start;
            start = 0;
        }
        if (count <= 0)
            return;
        const int end = std::min(start + count, rows);
        for (int i = start; i < end; ++i)
            _dirty_lines[i] = value;
        if (!value)
        {
            if (!_dirty_all)
                return;
            for (int i = 0; i < rows; ++i)
            {
                if (_dirty_lines[i])
                    return;
            }
            _dirty_all = false;
        }
    }

    bool is_region_touched(int start, int count) const
    {
        if (_dirty_all)
            return true;
        if (count <= 0)
            return false;
        int rows = _buf.rows();
        if (rows <= 0)
            return false;
        if (start < 0)
        {
            count += start;
            start = 0;
        }
        if (count <= 0)
            return false;
        const int end = std::min(start + count, rows);
        for (int i = start; i < end; ++i)
            if (_dirty_lines[i])
                return true;
        return false;
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
inline bool wresize(WINDOW* win, int nlines, int ncols)
{
    return win ? win->resize(nlines, ncols) : false;
}
inline bool mvwin(WINDOW* win, int begin_y, int begin_x)
{
    return win ? win->mvwin(begin_y, begin_x) : false;
}
inline bool resizeterm(int nlines, int ncols)
{
    return scr().resizeterm(nlines, ncols);
}
inline bool consume_resize(int& rows, int& cols)
{
    return scr().consume_resize(rows, cols);
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

inline void touchwin()
{
    scr().touchwin();
}
inline void touchwin(WINDOW* win)
{
    if (win)
        win->touchwin(true);
    else
        scr().touchwin();
}
inline void untouchwin()
{
    scr().untouchwin();
}
inline void untouchwin(WINDOW* win)
{
    if (win)
        win->untouchwin(true);
    else
        scr().untouchwin();
}
inline void touchline(int y, int n)
{
    scr().touchline(y, n);
}
inline void touchline(WINDOW* win, int y, int n)
{
    if (win)
        win->touchline(y, n, true);
    else
        scr().touchline(y, n);
}
inline void wtouchln(WINDOW* win, int y, int n, int changed)
{
    if (win)
        win->touchline(y, n, changed != 0);
    else
        scr().touchline(y, n, changed != 0);
}
inline bool is_linetouched(int y)
{
    return scr().is_linetouched(y);
}
inline bool is_linetouched(WINDOW* win, int y)
{
    return win ? win->is_linetouched(y) : scr().is_linetouched(y);
}
inline bool is_wintouched()
{
    return scr().is_wintouched();
}
inline bool is_wintouched(WINDOW* win)
{
    return win ? win->is_wintouched() : scr().is_wintouched();
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
