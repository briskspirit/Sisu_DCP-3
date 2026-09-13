#ifndef T9_SERVICE_H
#define T9_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#define T9_WORD_MAX 32u
#define T9_SEQUENCE_MAX 32u
#define T9_CANDIDATE_LIMIT 16u

typedef struct {
    const char *tag;
    const char *label;
} t9_dictionary_info_t;

typedef struct {
    char words[T9_CANDIDATE_LIMIT][T9_WORD_MAX + 1u];
    uint8_t count;
} t9_candidate_list_t;

uint8_t t9_dictionary_count(void);
const t9_dictionary_info_t *t9_dictionary_info(uint8_t index);
uint8_t t9_dictionary_index_for_tag(const char *tag);
/* Stable strings-registry language id of a dictionary (persist THIS, never a
 * registry index: the compiled set and its order are a build choice). */
uint8_t t9_dictionary_lang_id(uint8_t index);
uint8_t t9_dictionary_index_for_lang_id(uint8_t lang_id); /* absent -> 0 */

bool t9_candidates_for_sequence(const char *sequence,
                                uint8_t dictionary_index,
                                t9_candidate_list_t *out);
void t9_fallback_word(const char *sequence, char *dst, uint8_t cap);
void t9_display_word(const char *word, uint8_t mode, bool sentence_start, char *dst, uint8_t cap);
bool t9_signature(const char *word, char *dst, uint8_t cap);

#endif
