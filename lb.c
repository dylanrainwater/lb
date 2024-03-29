/*** includes ***/

// Feature test macros to make a little more portable
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/
#define LB_VERSION "0.0.1"
#define TAB_LENGTH 4
#define CTRL_KEY(k) ((k) & 0x1f) // 0x1f = 00011111

enum editorKey {
    BACK_SPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY
};

/*** data ***/
typedef struct editor_row {
    int length;
    int render_length;
    char *chars;
    char *render;
} erow;

struct statusbar {
    char *filename;
    char msg[80];
    time_t msg_time;
};

struct editorConfig {
    int cursor_x, cursor_y;
    int render_x;
    int row_offset;
    int screen_rows;
    int col_offset;
    int screen_cols;
    int num_rows;
    erow *rows;
    struct statusbar status;
    struct termios orig_termios;
};


struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);

/*** terminal ***/
void die(const char *s) {
    // Escape command to clear the whole screen
    write(STDOUT_FILENO, "\x1b[2J", 4);
    // Reposition cursor
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("disableRawMode()::tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("enableRawMode()::tcgetattr");
    }

    atexit(disableRawMode);

    struct termios raw = E.orig_termios; 

    // Turn off control characters and carriage return / new line
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    // Turn off output processing (for \n to \r\n translation)
    raw.c_oflag &= ~(OPOST);
    // Sets character size to 8, just in case
    raw.c_cflag |= (CS8);
    // Turn off echoing, canonical mode, SIGINT/SIGTSTP signals, and implementation-defined input processing
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    // Min number of bytes = 0 for timeout
    raw.c_cc[VMIN] = 0;
    // Time to wait for timeout in 1/10 of a second
    raw.c_cc[VTIME] = 10;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("enableRawMode()::tcsetattr");
    }
}

/* Waits for keypress and returns it */
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("editorReadKey()::read");
        }
    }

    // Check for command sequence
    if (c == '\x1b') {
        char seq[3];
        
        // Assume <esc> if nothing after initial seq
        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        
        if (seq[0] == '[') {
            // Check for quick jump commands
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
                    switch(seq[1]) {
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
                // Check for arrow keys
                switch(seq[1]) {
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
    }

    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    // Command to ask for cursor position
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    // Read response from request
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }

        if (buf[i] == 'R') {
            break;
        }

        i++;
    }
    buf[i] = '\0';

    // Check for command sequence
    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }

    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // As fallback if system doesn't support ioctl
        // Move to bottom right and count how far you moved to get there
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;

        return 0;
    }
}

/*** row operations ***/

int cursorXToRenderX(erow *row, int cx) {
    int rx = 0;

    int i;
    for (i = 0; i < cx; i++) {
        if (row->chars[i] == '\t') {
            rx += (TAB_LENGTH - 1) - (rx % TAB_LENGTH);
        }
        rx++;
    }

    return rx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->length; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->length + (tabs * (TAB_LENGTH - 1)) + 1);

    int rlength = 0;
    for (j = 0; j < row->length; j++) {
        // Render tabs as spaces
        if (row->chars[j] == '\t') {
            row->render[rlength++] = ' ';

            while (rlength % TAB_LENGTH != 0) {
                row->render[rlength++] = ' ';
            }
        } else {
            row->render[rlength++] = row->chars[j];
        }
    }

    row->render[rlength] = '\0';
    row->render_length = rlength;
}

void editorAppendRow(char *s, size_t len) {
    E.rows = realloc(E.rows, sizeof(erow) * (E.num_rows + 1));

    int r = E.num_rows;

    E.rows[r].length = len;
    E.rows[r].chars = malloc(len + 1);
    memcpy(E.rows[r].chars, s, len);
    E.rows[r].chars[len - 1] = '\0';

    E.rows[r].render_length = 0;
    E.rows[r].render = NULL;
    editorUpdateRow(&E.rows[r]);

    E.num_rows++;
}

void editorInsertCharAt(erow *row, int at, int c) {
    if (at < 0 || at > row->length) {
        at = row->length;
    }

    row->chars = realloc(row->chars, row->length + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->length - at + 1);
    row->length++;
    row->chars[at] = c;
    editorUpdateRow(row);
}

/*** editor operations ***/

void editorInsertChar(char c) {
    if (E.cursor_y == E.num_rows) {
        editorAppendRow("", 0);        
    }
    editorInsertCharAt(&E.rows[E.cursor_y], E.cursor_x, c);
    E.cursor_x++;
}


/*** file I/O ***/

char *editorRowsToString(int *buffer_length) {
    int total_length = 0;
    
    int j;
    for (j = 0; j < E.num_rows; j++) {
        total_length += E.rows[j].length + 1;
    }
    
    *buffer_length = total_length;

    char *buffer = malloc(total_length);
    char *p = buffer;
    for (j = 0; j < E.num_rows; j++) {
        memcpy(p, E.rows[j].chars, E.rows[j].length);
        p += E.rows[j].length;
        *p = '\n';
        p++;
    } 
    
    return buffer;
}

void openEditor(char *filename) {
    free(E.status.filename);
    E.status.filename = strdup(filename);

    FILE *fp  = fopen(filename, "r");
    if (!fp) {
        die("openEditor::fopen");
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len = 0;
    while((line_len = getline(&line, &line_cap, fp)) != -1) {
        // Strip off newline or carriage returns
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) line_len--;

        editorAppendRow(line, line_len + 1);
    }

    free(line);
    fclose(fp);
}

void saveEditor() {
    if (E.status.filename == NULL) {
        return;
    }

    int len;
    char *buf = editorRowsToString(&len);
    
    int fd = open(E.status.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                editorSetStatusMessage("%d bytes successfully written to disk.", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("ERROR: Can't save! I/O error: %s", strerror(errno));
}


/*** append buffer ***/

// dynamic, append-only string
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT { NULL, 0 }

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

void editorScroll() {
    E.render_x = 0;
    if (E.cursor_y < E.num_rows) {
        E.render_x = cursorXToRenderX(&E.rows[E.cursor_y], E.cursor_x);
    }

    /*** Vertical Scrolling ***/
    // Scroll above window if necessary
    if (E.cursor_y < E.row_offset) {
        E.row_offset = E.cursor_y;
    }

    // Scroll to bottom if necessary
    if (E.cursor_y >= E.row_offset + E.screen_rows) {
        E.row_offset = E.cursor_y - E.screen_rows + 1;
    }

    /*** Horizontal Scrolling ***/
    if (E.render_x < E.col_offset) {
        E.col_offset = E.render_x;
    }

    if (E.render_x >= E.col_offset + E.screen_cols) {
        E.col_offset = E.render_x + E.screen_cols + 1;
    }

}

/*
 * Works by appending message to ab using calls to abAppend();
 */
void editorDrawRows(struct abuf *ab) {
    int y;
    int screen_rows = E.screen_rows;

    for (y = 0; y < screen_rows; y++) {
        int file_row = y + E.row_offset;
        
        if (file_row >= E.num_rows) {
            // Display welcome message
            if (E.num_rows == 0 && y == screen_rows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "lb editor -- v%s", LB_VERSION);
                if (welcomelen > E.screen_cols) {
                    welcomelen = E.screen_cols;
                }
                
                int padding = (E.screen_cols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }

                while (padding--) {
                    abAppend(ab, " ", 1);
                }

                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1); 
            }
        } else {
            int len = E.rows[file_row].render_length - E.col_offset;
            
            if (len < 0) {
                len = 0;
            }

            if (len > E.screen_cols) {
                len = E.screen_cols;
            }
            abAppend(ab, &E.rows[file_row].render[E.col_offset], len);
        }
        // Clear to end of line
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4); // Invert colors
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "# %.20s - %d lines", 
                      E.status.filename ? E.status.filename : "[New File]", E.num_rows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d:%d %d ", E.cursor_y + 1, E.cursor_x + 1, E.num_rows);
    if (len > E.screen_cols) {
        len = E.screen_cols;
    }

    abAppend(ab, status, len);

    while (len < E.screen_cols) {
        if (E.screen_cols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }

    abAppend(ab, "\x1b[m", 3); // Re-invert colors
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessage(struct abuf *ab) {
    int show_length = 5; // seconds

    abAppend(ab, "\x1b[K", 3); // clear message bar

    int msglen = strlen(E.status.msg);
    if (msglen > E.screen_cols) {
        msglen = E.screen_cols;
    }
    if (msglen && time(NULL) - E.status.msg_time < show_length) {
        abAppend(ab, E.status.msg, msglen);
    }
}


void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;
    
    // Hide cursor
    abAppend(&ab, "\x1b[?25l", 6);
    // Reposition cursor
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessage(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursor_y - E.row_offset) + 1, (E.render_x - E.col_offset) + 1);
    abAppend(&ab, buf, strlen(buf));

    // Show cursor
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);

    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    vsnprintf(E.status.msg, sizeof(E.status.msg), fmt, ap);

    va_end(ap);
    E.status.msg_time = time(NULL);
}


/*** input ***/
void editorMoveCursor(int key) {
    erow *row = (E.cursor_y >= E.num_rows) ? NULL : &E.rows[E.cursor_y];

    switch (key) {
        case ARROW_LEFT:
            if (E.cursor_x != 0) {
                E.cursor_x--;
            } else if (E.cursor_y > 0) {
                E.cursor_y--;
                E.cursor_x = E.rows[E.cursor_y].length;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cursor_x < row->length) {
                E.cursor_x++;
            } else if (row && E.cursor_x == row->length) {
                E.cursor_x = 0;
                E.cursor_y++;
            }
            break;
        case ARROW_UP:
            if (E.cursor_y != 0) {
                E.cursor_y--;
            }
            break;
        case ARROW_DOWN:
            if (E.cursor_y < E.num_rows) {
                E.cursor_y++;
            }
            break;
    }

    // Account for row lengths being different
    row = (E.cursor_y >= E.num_rows) ? NULL : &E.rows[E.cursor_y];

    int row_len = row ? row->length : 0;
    if (E.cursor_x > row_len) {
        E.cursor_x = row_len;
    }
}

void editorProcessKeypresses() {
    int c = editorReadKey();

    switch (c) {
        case '\r':
            /* TODO */
            break;
        case CTRL_KEY('q'):
            // Escape command to clear the whole screen
            write(STDOUT_FILENO, "\x1b[2J", 4);
            // Reposition cursor
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            saveEditor();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cursor_y = E.row_offset;
                } else if (c == PAGE_DOWN) {
                    E.cursor_y = E.row_offset + E.screen_rows + 1;

                    if (E.cursor_y > E.num_rows) {
                        E.cursor_y = E.num_rows;
                    }
                }

                int times = E.screen_rows;
                while (times--) {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;

        case HOME_KEY:
            E.cursor_x = 0;
            break;
        case END_KEY:
            if (E.cursor_y < E.num_rows) {
                E.cursor_x = E.rows[E.cursor_y].length;
            }
            break;
        
        case BACK_SPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            /* TODO */
            break;

        case ARROW_UP:
        case ARROW_LEFT:
        case ARROW_DOWN:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;
    
        default:
            editorInsertChar(c);
            break;
    }
}


/*** init ***/
void initEditor() {
    E.cursor_x = 0;
    E.cursor_y = 0;
    E.render_x  = 0;
    E.num_rows = 0;
    E.rows = NULL;
    E.row_offset = 0;
    E.col_offset = 0;
    E.status.filename = NULL;
    E.status.msg[0] = '\0';
    E.status.msg_time = 0;

    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) {
        die("initEditor::getWindowSize");
    }

    E.screen_rows -= 2; // make room for status bar
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();

    if (argc >= 2) {
        openEditor(argv[1]);
    }

    editorSetStatusMessage("lb help: Ctrl-S to save | Ctrl-Q to quit");

    // Input loop
    while (1) {
        editorRefreshScreen();
        editorProcessKeypresses();
    }
    return 0;
}
