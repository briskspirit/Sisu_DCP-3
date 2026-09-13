#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Include the real implementation so its private selection/formatting helpers
 * are exercised without exporting UI-only machinery into the app API. */
#include "../src/apps/standby_app.c"

static int s_failures;

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    size_t len = src != NULL ? strlen(src) : 0u;
    if (len >= cap) {
        len = cap - 1u;
    }
    if (len != 0u) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void check_notice(app_t *app, const char *expected,
                         const char *message) {
    char text[48];
    format_message_waiting_notice(app, text, sizeof(text));
    check(strcmp(text, expected) == 0, message);
}

int main(void) {
    strings_set_language(1u);
    app_t app;
    memset(&app, 0, sizeof(app));

    app.message_waiting_notice_mask =
        modem_message_waiting_category_bit(MODEM_MESSAGE_WAITING_EMAIL);
    app.message_waiting_notice_count[MODEM_MESSAGE_WAITING_EMAIL] = 1u;
    check(standby_message_waiting_notice(&app) ==
              MODEM_MESSAGE_WAITING_EMAIL,
          "e-mail is selected when it is the only pending MWI notice");
    check_notice(&app, "New e-mail\nmessage",
                 "singular e-mail notice uses v6.00 SID 0x139");

    app.message_waiting_notice_count[MODEM_MESSAGE_WAITING_EMAIL] = 5u;
    check_notice(&app, "5\nnew e-mail\nmessages",
                 "plural e-mail notice substitutes the v6.00 %S count");

    app.message_waiting_notice_mask |=
        modem_message_waiting_category_bit(MODEM_MESSAGE_WAITING_FAX);
    app.message_waiting_notice_count[MODEM_MESSAGE_WAITING_FAX] = 1u;
    check(standby_message_waiting_notice(&app) == MODEM_MESSAGE_WAITING_FAX,
          "fax format 0x0a outranks e-mail format 0x0b");
    check_notice(&app, "New fax\nmessage",
                 "singular fax notice uses v6.00 SID 0x146");

    app.message_waiting_notice_count[MODEM_MESSAGE_WAITING_FAX] = 3u;
    check_notice(&app, "3\nnew fax\nmessages",
                 "plural fax notice substitutes the v6.00 %S count");

    app.message_waiting_notice_mask = 0u;
    check(standby_message_waiting_notice(&app) == MODEM_MESSAGE_WAITING_ALL,
          "no pending notice returns the event-only sentinel");
    check_notice(&app, "", "no pending notice formats as empty text");

    input_event_t event = {
        .type = EVENT_KEY_HOLD,
        .code = KEY_C,
        .when_ms = 500u,
    };
    app.input_len = 3u;
    check(standby_clear_all_hold_event(&app, &event),
          "held C over standby input requests clear-all and its second click");
    event.type = EVENT_KEY_DOWN;
    check(!standby_clear_all_hold_event(&app, &event),
          "initial C press is distinct from the clear-all hold");
    event.type = EVENT_KEY_HOLD;
    app.input_len = 0u;
    check(!standby_clear_all_hold_event(&app, &event),
          "held C over empty input does not synthesize a second click");
    app.input_len = 3u;
    event.code = KEY_NAVI;
    check(!standby_clear_all_hold_event(&app, &event),
          "other held keys cannot trigger the C clear-all click");

    if (s_failures == 0) {
        puts("test_message_waiting_ui: all passed");
        return 0;
    }
    fprintf(stderr, "test_message_waiting_ui: %d failure(s)\n", s_failures);
    return 1;
}
