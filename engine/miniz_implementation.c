// miniz (zip archives for saved maps), compiled once. Third party code: its
// warnings are switched off in the Makefile. File access goes through SDL
// (see file_io.h), so miniz' own stdio functions are left out.

#define MINIZ_NO_STDIO
#include "miniz.c"
