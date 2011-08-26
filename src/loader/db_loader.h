#ifndef __DB_LOADER_H__
#define __DB_LOADER_H__

#include "db_image.h"
#include "db_system.h"

int InitPort(char *port);
int LoadImage(System *sys, BoardConfig *config, char *port, char *path);
int WriteHubLoaderToEEPROM(System *sys, BoardConfig *config, char *port, char *path);
int WriteFlashLoaderToEEPROM(System *sys, BoardConfig *config, char *port);
int RunLoadedProgram(int step);

#endif
