#pragma once
static const unsigned game_level_choices[] = {50,55,60,65,70,75,80,85,86,90,95,99,100};
static inline int game_level_valid(unsigned value) {
    for (unsigned i = 0; i < sizeof(game_level_choices) / sizeof(*game_level_choices); ++i)
        if (game_level_choices[i] == value) return 1;
    return 0;
}
