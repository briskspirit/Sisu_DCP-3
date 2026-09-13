#include "apps/game_common.h"

#include "apps/profiles_app.h"
#include "audio/audio_levels.h"
#include "services/core1_services.h"

uint8_t game_audio_level(void) {
    uint8_t warning = profile_get_tone_setting(profile_active_index(), PROFILE_SETTING_WARNING_GAME_TONES);
    if (warning == 255u) {
        return AUDIO_LEVEL_SILENT;
    }
    return warning > AUDIO_LEVEL_MAX ? AUDIO_LEVEL_MAX : warning;
}

void play_game_system_tone(uint8_t index) {
    uint8_t level = game_audio_level();
    if (level != AUDIO_LEVEL_SILENT) {
        core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE, audio_arg(index, level));
    }
}
