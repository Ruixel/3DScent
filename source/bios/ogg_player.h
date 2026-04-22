#ifndef OGG_PLAYER_H
#define OGG_PLAYER_H

#include <stdbool.h>
#include "digi.h"

void initOggPlayer(void);
void shutdownOggPlayer(void);
bool load_ogg_from_music_library(struct MusicLibrary *library, char *fileName);

#endif
