/*
 * keywords.c — Pure keyword extraction for topic shift detection
 *
 * Zero dependencies on archive module. Can be tested standalone.
 * Handles Chinese (CJK) and English text.
 */

#include "keywords.h"
#include <string.h>
#include <ctype.h>

void kw_extract(const char *text, char *result, size_t result_len)
{
    if (!text || !result || result_len < 2) return;
    result[0] = '\0';

    char buf[512];
    size_t blen = strlen(text);
    if (blen >= sizeof(buf)) blen = sizeof(buf) - 1;
    memcpy(buf, text, blen);
    buf[blen] = '\0';

    size_t pos = 0;
    char *save;
    const char *delim = " ,.!?;:\"'、。！？，；：""（）()[]【】\t\n\r";
    char *tok = strtok_r(buf, delim, &save);

    while (tok && pos < result_len - 20) {
        size_t tlen = strlen(tok);
        int is_cjk = 0;
        for (size_t i = 0; i < tlen; i++)
            if ((unsigned char)tok[i] >= 0x80) { is_cjk = 1; break; }

        if (is_cjk || tlen >= 3) {
            char lower[64];
            size_t llen = tlen < sizeof(lower) - 1 ? tlen : sizeof(lower) - 1;
            for (size_t i = 0; i < llen; i++)
                lower[i] = (tok[i] >= 'A' && tok[i] <= 'Z') ? tok[i] + 32 : tok[i];
            lower[llen] = '\0';

            if (pos > 0 && result[pos - 1] != ' ') result[pos++] = ' ';
            size_t clen = strlen(lower);
            if (pos + clen + 1 < result_len) {
                memcpy(result + pos, lower, clen);
                pos += clen;
                result[pos++] = ' ';
            }
        }
        tok = strtok_r(NULL, delim, &save);
    }
    if (pos > 0) pos--;
    result[pos] = '\0';
}

double kw_overlap(const char *kw1, const char *kw2)
{
    if (!kw1 || !kw2 || !*kw1 || !*kw2) return 0.0;
    int common = 0, total = 0;
    char buf[256];
    strncpy(buf, kw1, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save, *tok = strtok_r(buf, " ", &save);
    while (tok) {
        total++;
        if (strstr(kw2, tok)) common++;
        tok = strtok_r(NULL, " ", &save);
    }
    return total > 0 ? (double)common / (double)total : 0.0;
}
