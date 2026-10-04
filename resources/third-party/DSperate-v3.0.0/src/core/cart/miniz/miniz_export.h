// Not upstream: miniz generates this from its own CMake build. We link the
// decompression core statically into dsperate_core, so the visibility macros
// are empty -- the same thing upstream's amalgamated single-header does.
#pragma once
#ifndef MINIZ_EXPORT
#define MINIZ_EXPORT
#endif
#ifndef MINIZ_NO_EXPORT
#define MINIZ_NO_EXPORT
#endif
