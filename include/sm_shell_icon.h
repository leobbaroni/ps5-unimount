#ifndef SM_SHELL_ICON_H
#define SM_SHELL_ICON_H

// Remove the verified legacy web-shortcut tiles of ShadowMountPlus
// (FAKE10101) and Kraken Mount (KRKN00001), then install the UniMount Shell UI
// launcher (UNMT00001) once. An existing UniMount tile is untouched.
void sm_shell_icon_install_if_missing(void);

#endif
