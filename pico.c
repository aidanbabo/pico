/*** includes ***/

// feature test macro ??
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>  // Input/Output ConTroL
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

// Cool ascii thing, clearing bits 5 and 6 gets the control sequence associated with that character
// Also bit five toggles lowercase and uppercase
#define CTRL_KEY(k) ((k) & 0x1f)

#define PICO_VERSION "0.0.1"

#define PICO_TAB_STOP 8

enum EditorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

/*** data ***/

typedef struct {
    int size;
    int rsize;  // The size of the render of the row (tabs are more than one character)
    char *chars;
    char *render;
} Row;

struct EditorConfig {
    int cx, cy;
    int rx;     // index into the render field
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    Row *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};

struct EditorConfig E;

/*** terminal ***/

void die(const char *s) {
    // Clear screen
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // Most libc functions that fail set the global errno variable to indicate what the error was.
    // `perror` looks at errno and prints a descriptive error message for it.
    // It also prints the given string.
    perror(s);
    // Exit with status of 1, indicating error, like an non-zero exit code.
    exit(1);
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    atexit(disable_raw_mode);

    struct termios raw = E.orig_termios;

    // Flags documented with * are usually already off and/or don't apply, but good practices

    // IXON: When on enables C-s and C-q, which involve "software control flow".
    //      stands for input XON because C-s and C-q are XON and XOFF.
    // ICRNL: When on enables coercing a \r without a \n to a \n (C-m is \r and C-j is \n).
    //      stands for input carriage return new line.
    // *BRKINT: When on a "break condition" will cause a SIGINT signal to be sent.
    // *INPCK: When on enables parity checking, which doesn't seem to apply to modern terminal emulators.
    // *ISTRIP: When on enables causes the high bit to be 0 for every input byte.
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);

    // OPOST: When on enables output processing, in reality the only feature would be coercing \n into \r\n.
    //      This means whenever we want a new line we have to do \r\n, otherwise the cursor will not
    //      go to the start of the line and will just move down.
    raw.c_oflag &= ~(OPOST);

    // *CS8: not a flag, a mask. Sets the character size to 8=bits per bytes, lmao, imagine needing this.
    raw.c_cflag |= (CS8);

    // ECHO: When on enables echoing back of input.
    // ICANON: When on enables reading line by line (we want it byte by byte).
    // ISIG: When on enables SIGINT and SIGTSTP from C-c and C-z, respectively.
    //      these are signal interrupt and signal temporary stop.
    // IEXTEN: When on enables C-v to send another character "literally" (e.g. C-v and C-c would only send a literal 3 and not SIGINT).
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    // cc stands for control characters
    // Both measured in tenths of a second.

    // Minimum amount of time before read returns.
    raw.c_cc[VMIN] = 0;
    // Maximum amount of time to wait before read returns.
    //      On time out read() returns 0.
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editor_read_key() {
    // read in character
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    // Process character if escape character
    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {

                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';

                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';

    } else {
        return c;
    }
}

int get_cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) -1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }

    // printf expected a null byte at the end
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

int get_window_size(int *rows, int *cols) {
    struct winsize ws;

    // Terminal IOCtl Get Window Size
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) 
            return -1;
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

int editor_row_cx_to_rx(Row *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (PICO_TAB_STOP - 1) - (rx % PICO_TAB_STOP);
        rx++;
    }
    return rx;
}

void editor_update_row(Row *row) {
    // used for iteration twice
    int j;

    int tabs = 0;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') 
            tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (PICO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            // get us to the next tab spot
            while (idx % PICO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editor_append_row(char *s, size_t len) {
    // Allocate space for the new row
    E.row = realloc(E.row, sizeof(Row) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editor_update_row(&E.row[at]);

    E.numrows++;
}

/*** file i/o ***/

void editor_open(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;     // This type is used for sizes of objects
    ssize_t linelen;        // This type is used for indicating errors (and also success ig)

    // Read line from file into `line` into buffer of current size `linecap` from `fp`.
    while ((linelen = getline(&line, &linecap, fp)) != -1) {

        // strip \r and \n
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
            linelen--;

        editor_append_row(line, linelen);
    }

    free(line);
    fclose(fp);

}

/*** append buffer ***/

// Avoid many small calls to write
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void ab_append(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
        return;

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** input ***/

void editor_move_cursor(int key) {
    Row *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
    }

    // Correct row if we moved down/up to a row that was shorter
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editor_process_keypress() {
    int c = editor_read_key();
    switch (c) {
        case CTRL_KEY('q'):
            // Clear screen
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            // exit
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            // Create code block so that we can delcare a variable
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

                int times = E.screenrows;
                while (times--)
                    editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_LEFT:
        case ARROW_RIGHT:
        case ARROW_UP:
        case ARROW_DOWN:
            editor_move_cursor(c);
            break;
    }
}

/*** output ***/

void editor_scroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editor_row_cx_to_rx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editor_draw_rows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        // I think the tutorial as `>=` here but it drew an extra tilde for me and now it just
        // has a blank line for appending a line to the file which is much better.
        // Is this undefined behavior or correct code ?!?!
        // It was UB...     the bug is still here... Only crashes with no file input
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && filerow == E.screenrows / 3) {
                // Create welcome message
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Pico editor -- version %s", PICO_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;

                // Add padding
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    ab_append(ab, " ", 1);

                ab_append(ab, welcome, welcomelen);

            // THIS SEEMED TO FIX IT!!!!!
            // used to just be else
            } else if (E.numrows == 0 || filerow == E.screenrows) {
                ab_append(ab, "~", 1);
            }
        } else {
            // Otherwise print the text at the yth row
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            ab_append(ab, &E.row[filerow].render[E.coloff], len);
        }

        ab_append(ab, "\x1b[K", 3);
        ab_append(ab, "\r\n", 2);
    }
}

void editor_draw_status_bar(struct abuf *ab) {
    // inverted colors
    ab_append(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines", E.filename ? E.filename : "[No Name]", E.numrows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);

    if (len > E.screencols) len = E.screencols;

    ab_append(ab, status, len);

    // Fill with spaces until right aligned line numbers
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            ab_append(ab, rstatus, rlen);
            break;
        } else {
            ab_append(ab, " ", 1);
            len++;
        }
    }

    // normal text formatting
    ab_append(ab, "\x1b[m", 3);
    ab_append(ab, "\r\n", 2);
}

void editor_draw_messageBar(struct abuf *ab) {
    ab_append(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        ab_append(ab, E.statusmsg, msglen);
}

void editor_refresh_screen() {
    editor_scroll();

    struct abuf ab = ABUF_INIT;

    // Hide the cursor
    ab_append(&ab, "\x1b[?25l", 6);

    // 0x1b is 27 in hexidecimal which is the escape character
    // the other three bytes are "[2j"
    // Escape sequences start with an escape character (\x1b)
    // and then '['. They instruct the terminal to do various things.
    // 'J' is erase and '2' means to erase the entire screen (0 is default argument).
    // `ab_append(&ab, "\x1b[2J", 4);`

    // The 3 means write four bytes
    // 'H' is the cursor position command.
    // takes 2 arguments we could use "\x1b[12;40H" to move the curosr to the 12th row and 40th col
    // multiple arguments are separated by a ';'. Default arguments are 1 and 1.
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_messageBar(&ab);

    // move cursor
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    ab_append(&ab, buf, strlen(buf));

    // Show the cursor
    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// This is how you do variable arguments in C, this is now a "variadic function"
// All the cool va shit comes from <stdarg.h>
// printf comes from <stdio.h> and time, obviously <time.h>
void editor_set_status_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** init ***/

void editor_init() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        die("get_window_size");

    E.screenrows -= 2;
}

int main(int arc, char *argv[]) {
    enable_raw_mode();
    editor_init();
    if (arc >= 2) {
        editor_open(argv[1]);
    }

    editor_set_status_message("HELP: Ctrl-Q to quit");

    for (;;) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
