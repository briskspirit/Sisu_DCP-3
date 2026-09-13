#ifndef INPUT_KEYS_H
#define INPUT_KEYS_H

#include <stdint.h>

/* Logical input codes shared by the keypad HAL and application layer. Keep
 * comparisons exact: INPUT_KEY_POWER is synthetic and overlaps matrix bits. */
#define INPUT_KEY_1     0x0001u
#define INPUT_KEY_2     0x0002u
#define INPUT_KEY_3     0x0004u
#define INPUT_KEY_NAVI  0x0008u
#define INPUT_KEY_4     0x0010u
#define INPUT_KEY_5     0x0020u
#define INPUT_KEY_6     0x0040u
#define INPUT_KEY_UP    0x0080u
#define INPUT_KEY_7     0x0100u
#define INPUT_KEY_8     0x0200u
#define INPUT_KEY_9     0x0400u
#define INPUT_KEY_DOWN  0x0800u
#define INPUT_KEY_STAR  0x1000u
#define INPUT_KEY_0     0x2000u
#define INPUT_KEY_HASH  0x4000u
#define INPUT_KEY_C     0x8000u
#define INPUT_KEY_POWER 0x000du

/* Short aliases used throughout the app layer. */
#define KEY_1     INPUT_KEY_1
#define KEY_2     INPUT_KEY_2
#define KEY_3     INPUT_KEY_3
#define KEY_NAVI  INPUT_KEY_NAVI
#define KEY_4     INPUT_KEY_4
#define KEY_5     INPUT_KEY_5
#define KEY_6     INPUT_KEY_6
#define KEY_UP    INPUT_KEY_UP
#define KEY_7     INPUT_KEY_7
#define KEY_8     INPUT_KEY_8
#define KEY_9     INPUT_KEY_9
#define KEY_DOWN  INPUT_KEY_DOWN
#define KEY_STAR  INPUT_KEY_STAR
#define KEY_0     INPUT_KEY_0
#define KEY_HASH  INPUT_KEY_HASH
#define KEY_C     INPUT_KEY_C
#define KEY_POWER INPUT_KEY_POWER

#endif
