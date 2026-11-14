// Compiles the stb single header libraries (public domain, see third_party/).
//
// STBI_NO_STDIO / STBI_WRITE_NO_STDIO keep stb from ever calling fopen: all
// files are read and written through SDL's IO layer (see file_io.h), and stb
// only works on memory buffers.

#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
