/* Lines as Pixelado Party speaks them (PXROOM/1, docs: common/README.md): `VERB key=value ...`, one per '\n', values
 * with their bytes under 0x21, '%', '=' and 0x7F written as %XX. Shared by the channel with Party (partyctl) and the
 * room's socket (roomctl). */
#ifndef PIXELADO_LINE_H
#define PIXELADO_LINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PX_LINE_MAX 1024   /* a line's limit, '\n' included */
#define PX_LINE_KEYS 24

typedef struct px_line {
    char verb[32];
    int nkv;
    char key[PX_LINE_KEYS][24];
    char val[PX_LINE_KEYS][256];
} px_line;

/* s into a line (s is one line, without its '\n'); unknown keys kept, a key too long dropped */
void px_line_parse(px_line *l, const char *s);
/* a key's value (unescaped), or def */
const char *px_line_str(const px_line *l, const char *key, const char *def);
long px_line_num(const px_line *l, const char *key, long def);
/* the value escaped into out (always terminated); the length written */
size_t px_line_esc(char *out, size_t cap, const char *s);

/* bytes arriving on a stream into whole lines: px_rx_feed appends, px_rx_next takes the next line out (1) or says
 * there is none yet (0). A line over PX_LINE_MAX is dropped. */
typedef struct px_rx {
    char buf[4096];
    size_t len;
} px_rx;
void px_rx_feed(px_rx *r, const char *b, size_t n);
int px_rx_next(px_rx *r, char *line, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
