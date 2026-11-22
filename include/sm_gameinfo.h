#ifndef SM_GAMEINFO_H
#define SM_GAMEINFO_H

#include <stdbool.h>
#include <sys/stat.h>

struct json_object;

// Copy a title from localizedParameters using the console language.
bool sm_gameinfo_console_title(struct json_object *localized, char *out_name);
// Read title ID and title name from a mounted game directory.
bool get_game_info(const char *base_path, const struct stat *param_st,
                   char *out_id, char *out_name);
// Accept four uppercase ASCII letters and five digits, excluding NPXS system apps.
bool is_supported_game_title_id(const char *title_id);
// Read the display name of an installed title from
// /user/appmeta/<TITLE_ID>/param.json; needs no initialised service.
bool sm_gameinfo_appmeta_title(const char *title_id, char *out_name);
// Check whether a directory contains sce_sys/param.json or param.sfo and stat it.
bool directory_has_game_param(const char *dir_path, struct stat *param_st_out);

#endif
