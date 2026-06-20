/*
 * strength.h — Pure memory strength score engine (Core Layer)
 *
 * Zero dependencies on I/O or archive types besides archive_topic_t.
 * Can be unit-tested standalone.
 */

#ifndef BUSINESS_ARCHIVE_STRENGTH_H
#define BUSINESS_ARCHIVE_STRENGTH_H

#include "archive.h"  /* for archive_topic_t */

/* Recalculate score for a single topic.
 * pure function: no side effects, no I/O. */
int strength_calc(const archive_topic_t *t);

/* Run decay for all topics in an array.
 * Returns number of topics whose score actually changed. */
int strength_decay_all(archive_topic_t *topics, int count);

/* Find the index of the best eviction candidate (lowest-scored non-flash).
 * Returns -1 if all topics are flash or count==0. */
int strength_evict_candidate(const archive_topic_t *topics, int count);

#endif /* BUSINESS_ARCHIVE_STRENGTH_H */
