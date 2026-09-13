#include "services/key_utils.h"

#include "services/input_keys.h"

char key_digit(uint16_t key) {
    switch (key) {
    case KEY_1: return '1';
    case KEY_2: return '2';
    case KEY_3: return '3';
    case KEY_4: return '4';
    case KEY_5: return '5';
    case KEY_6: return '6';
    case KEY_7: return '7';
    case KEY_8: return '8';
    case KEY_9: return '9';
    case KEY_0: return '0';
    case KEY_STAR: return '*';
    case KEY_HASH: return '#';
    default: return 0;
    }
}
