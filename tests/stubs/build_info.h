#ifndef STUB_BUILD_INFO_H
#define STUB_BUILD_INFO_H

/* Host stand-in for the build-directory build_info.h that
 * cmake/gen_build_info.cmake generates per configure. Only the layering
 * guard's host preprocessing of src/apps/service_codes_app.c uses it; the
 * firmware build sees the generated one. */
#define SISU_BUILD_HASH "host"
#define SISU_BUILD_DATE "0000-00-00"

#endif
