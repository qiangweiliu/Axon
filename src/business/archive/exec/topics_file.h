/*
 * topics_file.h — L1 topics.md file I/O (Execution Layer)
 *
 * Only responsibility: read/write topics.md on disk.
 * No business logic. Calls strength.h for score recalculation on load.
 */

#ifndef BUSINESS_ARCHIVE_TOPICS_FILE_H
#define BUSINESS_ARCHIVE_TOPICS_FILE_H

#include "archive.h"  /* archive_topic_t, constants */

/* Load topics from ARC_TOPICS_PATH.
 * topics: caller-owned array of ARC_TOPICS_MAX entries.
 * count_out: receives loaded count.
 * Returns 0 on success, -1 on error. */
int topics_file_load(archive_topic_t *topics, int *count_out);

/* Save topics to ARC_TOPICS_PATH (overwrite).
 * Returns 0 on success, -1 on error. */
int topics_file_save(const archive_topic_t *topics, int count);

#endif /* BUSINESS_ARCHIVE_TOPICS_FILE_H */
