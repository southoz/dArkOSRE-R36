Vendored from https://github.com/richgel999/miniz -- miniz 3.1.2, commit
77d0dce8627735138c51770d1799a1ef48f2117d.

Two DEFLATE cores are compiled: miniz_tinfl.c (decompression, for the ZIP
reader) and miniz_tdef.c (compression, only for its PNG writer
tdefl_write_image_to_png_file_in_memory_ex, which the frontend's auto-save
thumbnail uses), plus miniz.c without its zlib-style API for the CRC-32,
Adler-32 and mz_free those need. The other files here are headers they need, because miniz.h
unconditionally includes the whole family; miniz_zip.c (miniz's own ZIP
reader) is not vendored and never linked, so the archive code in ../zip.cpp
is ours and bounded to what a DS ROM archive needs.

miniz_export.h is not upstream -- upstream generates it from its own CMake
build. See the note in it.

Built with MINIZ_NO_ARCHIVE_APIS, MINIZ_NO_STDIO and MINIZ_NO_TIME, plus
MINIZ_NO_DEFLATE_APIS for miniz_tinfl.c, MINIZ_NO_INFLATE_APIS for
miniz_tdef.c and MINIZ_NO_ZLIB_APIS for miniz.c (set in ../../CMakeLists.txt).

MIT licensed (see LICENSE), compatible with this project's GPL-3.0-or-later.
Do not hand-edit: re-copy from a newer upstream tag and update the commit
recorded above.
