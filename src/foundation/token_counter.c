/*
 * token_counter.c — Simple token count estimator (no external deps)
 *
 * Approximates OpenAI cl100k_base tokenizer.  Accuracy ~85-90%,
 * good enough for prompt-size warnings.
 *
 * Heuristic:
 *   CJK (U+4E00..U+9FFF etc): 2 tokens per character
 *   English word boundary:     1.3 tokens per word
 *   Code / punctuation:        char/4  tokens
 *   Whitespace runs:           1 token
 */

#include "token_counter.h"
#include "os_api.h"

int token_estimate(const char *text)
{
    if (!text || !*text) return 0;
    int tokens = 0;
    const char *p = text;

    while (*p) {
        unsigned char c = (unsigned char)*p;

        /* UTF-8 multi-byte lead */
        if (c >= 0xC0) {
            int nbytes = (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            unsigned int cp = 0;
            if (nbytes >= 2 && nbytes <= 4 && p[1]) {
                cp = (c & (0xFF >> (nbytes + 1))) << ((nbytes - 1) * 6);
                for (int i = 1; i < nbytes && p[i]; i++)
                    cp |= ((unsigned char)p[i] & 0x3F) << ((nbytes - 1 - i) * 6);
            }
            /* CJK wide chars → 2 tokens each */
            if ((cp >= 0x4E00 && cp <= 0x9FFF) ||
                (cp >= 0x3400 && cp <= 0x4DBF) ||
                (cp >= 0xF900 && cp <= 0xFAFF) ||
                (cp >= 0x2E80 && cp <= 0x2EFF) ||
                (cp >= 0x3000 && cp <= 0x303F)) {
                tokens += 2;
            } else {
                tokens += 1;
            }
            p += nbytes;
            continue;
        }

        /* ASCII whitespace */
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            tokens += 1;
            while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                p++;
            continue;
        }

        /* ASCII word (letters + digits) — count as a group */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            int letters = 0;
            while (*p && (((unsigned char)*p >= 'a' && (unsigned char)*p <= 'z') ||
                          ((unsigned char)*p >= 'A' && (unsigned char)*p <= 'Z') ||
                          ((unsigned char)*p >= '0' && (unsigned char)*p <= '9'))) {
                letters++;
                p++;
            }
            /* ~1.3 tokens per 4 chars */
            tokens += (letters + 3) / 4 + 1;
            continue;
        }

        /* Punctuation / symbols → ~1 token per 2 chars */
        tokens += 1;
        p++;
    }
    return tokens;
}
