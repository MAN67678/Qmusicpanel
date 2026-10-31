// Single TU that instantiates all vendored header-only libraries.
#define MINIAUDIO_IMPLEMENTATION
#define MINIAUDIO_IMPLEMENTATION_STATIC
#include <miniaudio.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include <stb_image_resize2.h>
