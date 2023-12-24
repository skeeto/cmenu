#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>


#define MEMORY_MAX    (1<<28)  /* 256 MiB */
#define PATTERN_MAX   (1<<12)  /* 4KiB */

#define KEY_ESCAPE0   0x1B
#define KEY_ESCAPE1   0x5B
#define KEY_DOWN      0x42
#define KEY_UP        0x41
#define KEY_ENTER     0x0D
#define KEY_CTRLC     0x03
#define KEY_BACKSPACE 0x7F

#define NEW(arena, type, count) \
    (type*)alloc(arena, sizeof(type), count)

#define ERROR_EXIT(msg) \
    do { writestr(2, msg "\n"); exit(EXIT_FAILURE); } while (0)


typedef struct {
    char* beg;
    char* end;
} Arena;


typedef struct {
    int    tty;
    int    height;
    struct termios original;
} Terminal;


typedef struct {
    char** entries;
    int*   matches;
    int    entries_len;
    int    matches_len;
    int    selected;
} Entries;


int fullread(int fd, void* buf, ptrdiff_t len) {
    ptrdiff_t r, off;

    for (off = 0; off < len;) {
        r = read(fd, (char*)buf+off, len-off);
        if (r < 1) {
            break;
        }
        off += r;
    }
    return off;
}


int fullwrite(int fd, void* buf, ptrdiff_t len) {
    ptrdiff_t r, off;

    for (off = 0; off < len;) {
        r = write(fd, (char*)buf+off, len-off);
        if (r < 1) {
            return -1;
        }
        off += r;
    }
    return 0;
}


int writestr(int fd, char* str) {
    return fullwrite(fd, str, strlen(str));
}


/* Given a non-zero count, the first item will be zeroed. */
void* alloc(Arena* arena, ptrdiff_t size, ptrdiff_t count) {
    void*     ptr;
    ptrdiff_t available;

    available = arena->end - arena->beg;
    if (count > available/size) {
        ERROR_EXIT("out of memory");
    }
    ptr = arena->end -= size * count;
    return count ? memset(ptr, 0, size) : ptr;
}


void clear_screen(Terminal* term) {
    writestr(term->tty, "\x1b[H\x1b[2J\x1b[3J");
}


char xtolower(char c) {
    return ((unsigned)c-'A' < 26) ? c+'a'-'A' : c;
}


bool is_match(char* pattern, char* str) {
    if (*pattern == '\0')
        return true;

    while (*(pattern++) == xtolower(*(str++))) {
        if (*pattern == '\0')
            return true;
        else if (*str == '\0')
            return false;
    }

    return false;
}


void restore_terminal_mode(Terminal* term) {
    tcsetattr(term->tty, TCSANOW, &term->original);
}


void set_terminal_mode(Terminal* term) {
    struct termios termios_new;

    tcgetattr(term->tty, &term->original);
    memcpy(&termios_new, &term->original, sizeof termios_new);

    cfmakeraw(&termios_new);
    tcsetattr(term->tty, TCSANOW, &termios_new);
}


Entries* read_entries(Arena* arena) {
    char*     input;
    ptrdiff_t input_len, input_off, entry_len, i;
    Entries*  e;

    e = NEW(arena, Entries, 1);

    input = arena->beg;
    input_len = arena->end - arena->beg;
    input_len = fullread(0, arena->beg, input_len-1);
    input[input_len++] = '\n';
    arena->beg += input_len;

    /* Count non-empty lines */
    entry_len = 0;
    for (input_off = 0; input_off < input_len; input_off++) {
        switch (input[input_off]) {
        case '\0':
        case '\n':
        case '\r':
            e->entries_len += entry_len > 0;
            entry_len = 0;
            break;
        default:
            entry_len++;
        }
    }

    e->entries = NEW(arena, char*, e->entries_len);
    e->matches = NEW(arena, int, e->entries_len);

    /* Chop lines up into entries */
    i = 0;
    entry_len = 0;
    for (input_off = 0; input_off < input_len; input_off++) {
        switch (input[input_off]) {
        case '\0':
        case '\n':
        case '\r':
            input[input_off] = 0;
            if (entry_len > 0) {
                e->entries[i++] = input + input_off - entry_len;
            }
            entry_len = 0;
            break;
        default:
            entry_len++;
        }
    }

    return e;
}


void set_selected_clamped(Entries* e, int n) {
    e->selected = ((n < 0) ? 0 : n >= e->matches_len ? e->matches_len - 1 : n);
}


void update_matches(Entries* e, char* pattern, Arena scratch) {
    char* copy;
    int   i, len;

    len = strlen(pattern) + 1;
    copy = NEW(&scratch, char, len);
    for (i = 0; i < len; i++) {
        copy[i] = xtolower(pattern[i]);
    }

    e->matches_len = 0;
    for (i = 0; i < e->entries_len; ++i) {
        if (is_match(pattern, e->entries[i]))
            e->matches[e->matches_len++] = i;
    }
}


int getch(Terminal* term) {
    char r;

    if (read(term->tty, &r, 1) == -1) {
        ERROR_EXIT("tty input error");
    }
    return r & 255;
}


void draw(Terminal* term, Entries* e, char* pattern, Arena scratch) {
    int i, j;

    update_matches(e, pattern, scratch);
    set_selected_clamped(e, e->selected);
    clear_screen(term);

    writestr(term->tty, ">");
    writestr(term->tty, pattern);
    writestr(term->tty, "\n");

    if (e->selected > term->height - 3) {
        i = e->selected - (term->height - 3);
    } else {
        i = 0;
    }

    for (j = 0; i < e->matches_len; ++i) {
        if (++j == term->height - 1)
            break;

        writestr(term->tty, e->entries[e->matches[i]]);
        writestr(term->tty, i == e->selected ? " (*)" : "");
        writestr(term->tty, "\n");
    }
}


bool ch_isvalid(char ch) {
    return (ch >= ' ' && ch <= '~');
}


void select_next(Entries* e) {
    set_selected_clamped(e, --e->selected);
}


void select_prev(Entries* e) {
    set_selected_clamped(e, ++e->selected);
}


Terminal* get_terminal(Arena* arena) {
    Terminal* term;
    struct winsize w;

    term = NEW(arena, Terminal, 1);

    term->tty = open("/dev/tty", O_RDWR);
    if (term->tty == -1) {
        ERROR_EXIT("could not open /dev/tty");
    }

    ioctl(term->tty, TIOCGWINSZ, &w);
    term->height = w.ws_row;

    return term;
}


int main(void) {
    char*     pattern;
    char*     selected_entry;
    int       i, ch, pattern_len;
    Arena     arena[1];
    Entries*  entries;
    Terminal* term;

    arena->beg = malloc(MEMORY_MAX);
    if (!arena->beg) {
        arena->end = arena->beg = (void*)16;  /* zero-sized, but non-null */
    } else {
        arena->end = arena->beg + MEMORY_MAX;
    }

    entries = read_entries(arena);
    term = get_terminal(arena);
    draw(term, entries, "", *arena);
    set_terminal_mode(term);

    pattern = NEW(arena, char, PATTERN_MAX);

    pattern_len = 0;
    selected_entry = NULL;
    while (1) {
        ch = getch(term);
        if (KEY_ENTER == ch) {
            if (entries->matches_len > 0) {
                i = entries->matches[entries->selected];
                selected_entry = entries->entries[i];
            }
            goto end;
        } else if (KEY_CTRLC == ch) {
            goto end;
        } else if (KEY_BACKSPACE == ch) {
            if ((pattern_len - 1) > -1)
                --pattern_len;
            pattern[pattern_len] = '\0';
        } else if (KEY_ESCAPE0 == ch) {
            ch = getch(term);
            if (KEY_ESCAPE1 == ch) {
                switch (getch(term)) {
                case KEY_UP:   select_next(entries); break;
                case KEY_DOWN: select_prev(entries); break;
                }
            }
        } else {
            if (ch_isvalid(ch)) {
                pattern[pattern_len++] = ch;
                if (pattern_len >= PATTERN_MAX - 1)
                    ERROR_EXIT("pattern too long");
                pattern[pattern_len] = '\0';
            } else {
                continue;
            }
        }

        restore_terminal_mode(term);
        draw(term, entries, pattern_len == 0 ? "" : pattern, *arena);
        set_terminal_mode(term);
    }

end:
    restore_terminal_mode(term);

    if (selected_entry) {
        writestr(1, selected_entry);
    }

    return EXIT_SUCCESS;
}
