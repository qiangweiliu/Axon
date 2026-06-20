/*
 * keywords.h — Pure keyword extraction API
 *
 * Zero dependencies. Handles Chinese (CJK) and English.
 */

#ifndef BUSINESS_ARCHIVE_KEYWORDS_H
#define BUSINESS_ARCHIVE_KEYWORDS_H

#include <stddef.h>

/* Extract significant keywords from text.
 * For Chinese: all CJK character sequences.
 * For English: words >= 3 chars, lowercased.
 * Result: space-separated keywords, truncated to result_len. */
void kw_extract(const char *text, char *result, size_t result_len);

/* Compute keyword overlap ratio between two keyword strings (0.0 - 1.0). */
double kw_overlap(const char *kw1, const char *kw2);

#endif /* BUSINESS_ARCHIVE_KEYWORDS_H */
