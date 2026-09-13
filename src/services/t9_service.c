#include "services/t9_service.h"

#include <stddef.h>
#include <string.h>

#include "generated/t9_ldb.h"

#define T9_LDB_BASE 0x0010u
#define T9_MAX_TRANSITIONS 1024u

typedef struct {
    const char *tag;
    const char *label;
    const uint8_t *data;
    uint32_t data_len;
    uint16_t dictionary_id;
    uint16_t char_table_offset;
    uint16_t trie_root_offset;
    uint16_t short_pointer_offsets[4];
    uint16_t block_pointer_base_offset;
    uint16_t payload_base_offset;
    uint16_t block_pointer_thresholds[6];
} t9_dictionary_record_t;

typedef struct {
    const t9_dictionary_record_t *record;
    uint32_t nibble_pos;
} t9_reader_t;

typedef struct {
    uint16_t rank;
    uint8_t slot;
    bool terminal;
} t9_transition_t;

static uint8_t native_byte_at(const t9_dictionary_record_t *record, uint32_t file_offset);
static uint8_t native_nibble(const t9_dictionary_record_t *record, uint32_t native_nibble_offset);
static uint16_t native_u16_le(const t9_dictionary_record_t *record, uint32_t native_offset);
static void reader_seek_native_byte(t9_reader_t *reader, uint32_t offset);
static uint8_t reader_read_nibble(t9_reader_t *reader);
static char slot_char(const t9_dictionary_record_t *record, uint8_t klass, uint8_t slot);
static t9_transition_t decode_transition(t9_reader_t *reader);
static void skip_pointer_code(t9_reader_t *reader);
static uint32_t decode_pointer_code(t9_reader_t *reader);
static bool select_branch(t9_reader_t *reader, uint8_t klass);
static bool is_t9_sequence(const char *sequence);
static void clean_native_word(char *word);
static char digit_for_char(char ch);
static char ascii_lower(char ch);
static char ascii_upper(char ch);
static char latin_unit_to_ascii(uint16_t unit);
static void copy_text(char *dst, uint8_t cap, const char *src);

/* Dictionaries come from the generated registry (g_t9_ldbs): the build
 * carries exactly the dictionaries it was generated with, English always
 * first. Records are copied once on first use; every layout parameter was
 * parsed from the LDB's own header at generation time. */
#define T9_DICTIONARY_CAP 12u
static t9_dictionary_record_t s_dictionaries[T9_DICTIONARY_CAP];
static uint8_t s_dictionary_built_count;

static const t9_dictionary_record_t *dictionaries(void) {
    if (s_dictionary_built_count == 0u) {
        uint8_t count = g_t9_ldb_count;
        if (count > T9_DICTIONARY_CAP) {
            count = T9_DICTIONARY_CAP;
        }
        for (uint8_t i = 0u; i < count; i++) {
            const t9_ldb_desc_t *desc = &g_t9_ldbs[i];
            t9_dictionary_record_t *record = &s_dictionaries[i];
            record->tag = desc->tag;
            record->label = desc->label;
            record->data = desc->data;
            record->data_len = desc->data_len;
            record->dictionary_id = desc->dictionary_id;
            record->char_table_offset = desc->char_table_offset;
            record->trie_root_offset = desc->trie_root_offset;
            for (uint8_t j = 0u; j < 4u; j++) {
                record->short_pointer_offsets[j] = desc->short_pointer_offsets[j];
            }
            record->block_pointer_base_offset = desc->block_pointer_base_offset;
            record->payload_base_offset = desc->payload_base_offset;
            for (uint8_t j = 0u; j < 6u; j++) {
                record->block_pointer_thresholds[j] = desc->block_pointer_thresholds[j];
            }
        }
        s_dictionary_built_count = count;
    }
    return s_dictionaries;
}

static uint16_t s_ranks[T9_MAX_TRANSITIONS];
static uint8_t s_slots[T9_MAX_TRANSITIONS];
static bool s_terminals[T9_MAX_TRANSITIONS];
static uint16_t s_level_starts[T9_SEQUENCE_MAX];

uint8_t t9_dictionary_count(void) {
    (void)dictionaries();
    return s_dictionary_built_count;
}

const t9_dictionary_info_t *t9_dictionary_info(uint8_t index) {
    static t9_dictionary_info_t info;
    const t9_dictionary_record_t *records = dictionaries();
    if (index >= t9_dictionary_count()) {
        index = 0u;
    }
    info.tag = records[index].tag;
    info.label = records[index].label;
    return &info;
}

uint8_t t9_dictionary_index_for_tag(const char *tag) {
    const t9_dictionary_record_t *records = dictionaries();
    if (tag == 0) {
        return 0u;
    }
    for (uint8_t i = 0; i < t9_dictionary_count(); i++) {
        if (strcmp(tag, records[i].tag) == 0) {
            return i;
        }
    }
    return 0u;
}

uint8_t t9_dictionary_lang_id(uint8_t index) {
    (void)dictionaries();
    if (index >= s_dictionary_built_count) {
        index = 0u;
    }
    return g_t9_ldbs[index].lang_id;
}

uint8_t t9_dictionary_index_for_lang_id(uint8_t lang_id) {
    (void)dictionaries();
    for (uint8_t i = 0u; i < s_dictionary_built_count; i++) {
        if (g_t9_ldbs[i].lang_id == lang_id) {
            return i;
        }
    }
    return 0u;
}

bool t9_candidates_for_sequence(const char *sequence,
                                uint8_t dictionary_index,
                                t9_candidate_list_t *out) {
    if (out == 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!is_t9_sequence(sequence)) {
        return false;
    }
    if (dictionary_index >= t9_dictionary_count()) {
        dictionary_index = 0u;
    }

    const t9_dictionary_record_t *record = &dictionaries()[dictionary_index];
    uint8_t classes[T9_SEQUENCE_MAX];
    uint8_t sequence_len = 0u;
    while (sequence[sequence_len] != '\0' && sequence_len < T9_SEQUENCE_MAX) {
        classes[sequence_len] = (uint8_t)(sequence[sequence_len] - '1');
        sequence_len++;
    }
    if (sequence[sequence_len] != '\0') {
        return false;
    }

    t9_reader_t reader;
    reader.record = record;
    reader_seek_native_byte(&reader, record->trie_root_offset);

    uint16_t previous_count = 0x20u;
    uint16_t total_transitions = 0u;
    for (uint8_t level = 0u; level < sequence_len; level++) {
        uint8_t klass = classes[level];
        s_level_starts[level] = total_transitions;
        if (!select_branch(&reader, klass)) {
            return false;
        }
        uint16_t valid_count = 0u;
        while (true) {
            if (total_transitions >= T9_MAX_TRANSITIONS) {
                return false;
            }
            t9_transition_t transition = decode_transition(&reader);
            if (transition.rank < previous_count) {
                valid_count++;
            }
            s_ranks[total_transitions] = transition.rank;
            s_slots[total_transitions] = transition.slot;
            s_terminals[total_transitions] = transition.terminal;
            total_transitions++;
            if (transition.terminal) {
                break;
            }
        }
        if (level + 1u >= sequence_len) {
            break;
        }
        reader_seek_native_byte(&reader, decode_pointer_code(&reader));
        previous_count = valid_count;
    }

    for (uint8_t candidate = 1u; candidate <= T9_CANDIDATE_LIMIT; candidate++) {
        uint16_t rank = (uint16_t)(candidate - 1u);
        uint16_t upper_bound = total_transitions;
        uint8_t reversed_slots[T9_SEQUENCE_MAX];
        bool ok = true;
        for (int level = (int)sequence_len - 1; level >= 0; level--) {
            uint16_t start = s_level_starts[level];
            if (upper_bound <= rank + start) {
                ok = false;
                break;
            }
            uint16_t index = (uint16_t)(start + rank);
            upper_bound = start;
            reversed_slots[(uint8_t)((sequence_len - 1u) - (uint8_t)level)] = s_slots[index];
            rank = s_ranks[index];
        }
        if (!ok) {
            break;
        }

        char word[T9_WORD_MAX + 1u];
        uint8_t pos = 0u;
        for (uint8_t level = 0u; level < sequence_len && pos + 1u < sizeof(word); level++) {
            uint8_t reversed_index = (uint8_t)((sequence_len - 1u) - level);
            char ch = slot_char(record, classes[level], reversed_slots[reversed_index]);
            if (ch != '\0') {
                word[pos++] = ch;
            }
        }
        word[pos] = '\0';
        clean_native_word(word);
        if (word[0] != '\0') {
            bool duplicate = false;
            for (uint8_t i = 0; i < out->count; i++) {
                if (strcmp(out->words[i], word) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                copy_text(out->words[out->count], (uint8_t)sizeof(out->words[out->count]), word);
                out->count++;
                if (out->count >= T9_CANDIDATE_LIMIT) {
                    break;
                }
            }
        }
    }
    return out->count > 0u;
}

void t9_fallback_word(const char *sequence, char *dst, uint8_t cap) {
    if (dst == 0 || cap == 0u) {
        return;
    }
    uint8_t pos = 0u;
    for (uint8_t i = 0u; sequence != 0 && sequence[i] != '\0' && pos + 1u < cap; i++) {
        switch (sequence[i]) {
        case '2': dst[pos++] = 'a'; break;
        case '3': dst[pos++] = 'e'; break;
        case '4': dst[pos++] = 'i'; break;
        case '5': dst[pos++] = 'l'; break;
        case '6': dst[pos++] = 'n'; break;
        case '7': dst[pos++] = 's'; break;
        case '8': dst[pos++] = 't'; break;
        case '9': dst[pos++] = 'y'; break;
        default: break;
        }
    }
    dst[pos] = '\0';
}

void t9_display_word(const char *word, uint8_t mode, bool sentence_start, char *dst, uint8_t cap) {
    if (dst == 0 || cap == 0u) {
        return;
    }
    dst[0] = '\0';
    uint8_t pos = 0u;
    for (uint8_t i = 0u; word != 0 && word[i] != '\0' && pos + 1u < cap; i++) {
        char ch = word[i];
        if (mode == 2u) {
            ch = ascii_upper(ch);
        } else if (mode == 0u && i == 0u && sentence_start) {
            /* S6.3: sentence mode capitalizes only at a sentence start, not
             * the first letter of every candidate. */
            ch = ascii_upper(ch);
        } else {
            ch = ascii_lower(ch);
        }
        dst[pos++] = ch;
    }
    dst[pos] = '\0';
}

bool t9_signature(const char *word, char *dst, uint8_t cap) {
    if (dst == 0 || cap == 0u) {
        return false;
    }
    uint8_t pos = 0u;
    for (uint8_t i = 0u; word != 0 && word[i] != '\0'; i++) {
        char digit = digit_for_char(ascii_lower(word[i]));
        if (digit == '\0') {
            continue;
        }
        if (pos + 1u >= cap) {
            break;
        }
        dst[pos++] = digit;
    }
    dst[pos] = '\0';
    return pos > 0u;
}

static uint8_t native_byte_at(const t9_dictionary_record_t *record, uint32_t file_offset) {
    if (record == 0 || record->data == 0 || file_offset >= record->data_len) {
        return 0u;
    }
    return record->data[file_offset];
}

static uint8_t native_nibble(const t9_dictionary_record_t *record, uint32_t native_nibble_offset) {
    uint32_t file_offset = T9_LDB_BASE + native_nibble_offset / 2u;
    uint8_t byte = native_byte_at(record, file_offset);
    return (native_nibble_offset & 1u) != 0u ? (uint8_t)(byte & 0x0fu) : (uint8_t)(byte >> 4);
}

static uint16_t native_u16_le(const t9_dictionary_record_t *record, uint32_t native_offset) {
    uint32_t file_offset = T9_LDB_BASE + native_offset;
    return (uint16_t)(native_byte_at(record, file_offset) |
                      ((uint16_t)native_byte_at(record, file_offset + 1u) << 8));
}

static void reader_seek_native_byte(t9_reader_t *reader, uint32_t offset) {
    reader->nibble_pos = offset * 2u;
}

static uint8_t reader_read_nibble(t9_reader_t *reader) {
    uint8_t value = native_nibble(reader->record, reader->nibble_pos);
    reader->nibble_pos++;
    return value;
}

static char slot_char(const t9_dictionary_record_t *record, uint8_t klass, uint8_t slot) {
    uint32_t offset = record->char_table_offset + (uint32_t)klass * 0x20u + (uint32_t)slot * 2u;
    return latin_unit_to_ascii(native_u16_le(record, offset));
}

static t9_transition_t decode_transition(t9_reader_t *reader) {
    uint8_t first = reader_read_nibble(reader);
    uint16_t value = (uint16_t)(first & 0x07u);
    if ((first & 0x08u) != 0u) {
        uint8_t second = reader_read_nibble(reader);
        value = (uint16_t)((value << 3) | (second & 0x07u));
        if ((second & 0x08u) != 0u) {
            uint8_t third = reader_read_nibble(reader);
            value = (uint16_t)((value << 4) | third);
            return (t9_transition_t){(uint16_t)(value >> 5), (uint8_t)((value >> 1) & 0x0fu), (value & 1u) != 0u};
        }
        value = (uint16_t)(value + 8u);
    }
    bool terminal = (value & 1u) != 0u;
    if (value < 6u) {
        return (t9_transition_t){(uint16_t)(value >> 1), 0u, terminal};
    }
    if (value > 7u) {
        if (value < 0x1cu) {
            return (t9_transition_t){(uint16_t)(((value - 8u) >> 1) + 3u), 0u, terminal};
        }
        if (value > 0x31u) {
            return (t9_transition_t){(uint16_t)((value - 0x32u) >> 1), 2u, terminal};
        }
        return (t9_transition_t){(uint16_t)(((value - 0x1cu) >> 1) + 1u), 1u, terminal};
    }
    return (t9_transition_t){0u, 1u, terminal};
}

static void skip_pointer_code(t9_reader_t *reader) {
    uint8_t first = reader_read_nibble(reader);
    uint8_t pointer_type = (uint8_t)(first >> 2);
    if (pointer_type == 1u) {
        (void)reader_read_nibble(reader);
    } else if (pointer_type != 0u) {
        for (uint8_t i = 0u; i < pointer_type + 1u; i++) {
            (void)reader_read_nibble(reader);
        }
    }
}

static uint32_t decode_pointer_code(t9_reader_t *reader) {
    uint8_t first = reader_read_nibble(reader);
    uint8_t pointer_type = (uint8_t)(first >> 2);
    uint32_t low = (uint32_t)(first & 0x03u);
    if (pointer_type == 0u) {
        return low < 4u ? reader->record->short_pointer_offsets[low] : 0u;
    }
    if (pointer_type == 1u) {
        uint32_t value = (low << 4) | reader_read_nibble(reader);
        uint32_t total = 0u;
        uint8_t index = 0u;
        uint32_t threshold = reader->record->block_pointer_thresholds[index];
        while (threshold < value && index + 1u < 6u) {
            total += value;
            value -= threshold;
            index++;
            threshold = reader->record->block_pointer_thresholds[index];
        }
        return reader->record->block_pointer_base_offset + (value + total) * 0x20u;
    }
    if (pointer_type == 2u || pointer_type == 3u) {
        uint32_t value = low;
        for (uint8_t i = 0u; i < pointer_type + 1u; i++) {
            value = (value << 4) | reader_read_nibble(reader);
        }
        return reader->record->payload_base_offset + value;
    }
    return 0u;
}

static bool select_branch(t9_reader_t *reader, uint8_t klass) {
    uint16_t mask = (uint16_t)((reader_read_nibble(reader) << 8) |
                               (reader_read_nibble(reader) << 4) |
                               reader_read_nibble(reader));
    if ((mask & (uint16_t)(1u << klass)) == 0u) {
        return false;
    }
    for (uint8_t prior = 0u; prior < klass; prior++) {
        if ((mask & (uint16_t)(1u << prior)) != 0u) {
            while (true) {
                t9_transition_t transition = decode_transition(reader);
                if (transition.terminal) {
                    break;
                }
            }
            skip_pointer_code(reader);
        }
    }
    return true;
}

static bool is_t9_sequence(const char *sequence) {
    if (sequence == 0 || sequence[0] == '\0') {
        return false;
    }
    for (uint8_t i = 0u; sequence[i] != '\0'; i++) {
        if (sequence[i] < '2' || sequence[i] > '9' || i >= T9_SEQUENCE_MAX) {
            return false;
        }
    }
    return true;
}

static void clean_native_word(char *word) {
    if (word == 0) {
        return;
    }
    uint8_t out = 0u;
    for (uint8_t i = 0u; word[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)word[i];
        if (ch >= 0x20u && ch != 0x7fu) {
            word[out++] = (char)ch;
        }
    }
    word[out] = '\0';
}

static char digit_for_char(char ch) {
    if (ch >= 'a' && ch <= 'c') {
        return '2';
    }
    if (ch >= 'd' && ch <= 'f') {
        return '3';
    }
    if (ch >= 'g' && ch <= 'i') {
        return '4';
    }
    if (ch >= 'j' && ch <= 'l') {
        return '5';
    }
    if (ch >= 'm' && ch <= 'o') {
        return '6';
    }
    if (ch >= 'p' && ch <= 's') {
        return '7';
    }
    if (ch >= 't' && ch <= 'v') {
        return '8';
    }
    if (ch >= 'w' && ch <= 'z') {
        return '9';
    }
    return '\0';
}

static char ascii_lower(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : ch;
}

static char ascii_upper(char ch) {
    return (ch >= 'a' && ch <= 'z') ? (char)(ch - ('a' - 'A')) : ch;
}

static char latin_unit_to_ascii(uint16_t unit) {
    if (unit == 0u) {
        return '\0';
    }
    if (unit >= 0x20u && unit <= 0x7eu) {
        return (char)unit;
    }
    switch (unit) {
    case 0x00c0u: case 0x00c1u: case 0x00c2u: case 0x00c3u:
    case 0x00c4u: case 0x00c5u: case 0x00c6u: case 0x00e0u:
    case 0x00e1u: case 0x00e2u: case 0x00e3u: case 0x00e4u:
    case 0x00e5u: case 0x00e6u:
        return 'a';
    case 0x00c7u: case 0x00e7u:
        return 'c';
    case 0x00c8u: case 0x00c9u: case 0x00cau: case 0x00cbu:
    case 0x00e8u: case 0x00e9u: case 0x00eau: case 0x00ebu:
        return 'e';
    case 0x00ccu: case 0x00cdu: case 0x00ceu: case 0x00cfu:
    case 0x00ecu: case 0x00edu: case 0x00eeu: case 0x00efu:
        return 'i';
    case 0x00d1u: case 0x00f1u:
        return 'n';
    case 0x00d2u: case 0x00d3u: case 0x00d4u: case 0x00d5u:
    case 0x00d6u: case 0x00d8u: case 0x00f2u: case 0x00f3u:
    case 0x00f4u: case 0x00f5u: case 0x00f6u: case 0x00f8u:
        return 'o';
    case 0x00d9u: case 0x00dau: case 0x00dbu: case 0x00dcu:
    case 0x00f9u: case 0x00fau: case 0x00fbu: case 0x00fcu:
        return 'u';
    case 0x00ddu: case 0x0178u: case 0x00fdu: case 0x00ffu:
        return 'y';
    case 0x00dfu:
        return 's';
    case 0x00a1u: /* inverted exclamation (Spanish key-1 class) */
        return '!';
    case 0x00bfu: /* inverted question mark (Spanish key-1 class) */
        return '?';
    case 0x011eu: case 0x011fu: /* G-breve (Turkish) */
        return 'g';
    case 0x0130u: case 0x0131u: /* dotted capital I / dotless i (Turkish) */
        return 'i';
    case 0x015eu: case 0x015fu: /* S-cedilla (Turkish) */
        return 's';
    case 0x008au: case 0x009au: /* S-caron, CP1252-coded in the 6210 tables */
        return 's';
    case 0x009fu: /* Y-diaeresis, CP1252-coded in the 6210 tables */
        return 'y';
    default:
        return '\0';
    }
}

static void copy_text(char *dst, uint8_t cap, const char *src) {
    if (dst == 0 || cap == 0u) {
        return;
    }
    if (src == 0) {
        src = "";
    }
    strncpy(dst, src, cap - 1u);
    dst[cap - 1u] = '\0';
}
