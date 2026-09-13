#ifndef SISU_BUILD_CONFIG_H
#define SISU_BUILD_CONFIG_H

/* Host tests and standalone diagnostic targets retain the service-firmware
 * behavior unless their composition root explicitly selects the release
 * profile. The production CMake target always defines this to 0 or 1. */
#ifndef SISU_RELEASE_BUILD
#define SISU_RELEASE_BUILD 0
#endif

#if SISU_RELEASE_BUILD != 0 && SISU_RELEASE_BUILD != 1
#error "SISU_RELEASE_BUILD must be 0 or 1"
#endif

#endif
