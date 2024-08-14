# BPal - Add BPal entries to Blorb files

This program generates Blorb files with BPal chunks (see [Blorb.md](Blorb.md)),
so that the adaptive palettes of Zork Zero and Arthur can be used with Glk-based
interpreters. Generated Blorb files are backward compatible: they contain all
information in the original Blorb plus the BPal chunk.

Because BPal chunks include a lot of new images, the images are compressed with
[oxipng](https://github.com/shssoichiro/oxipng) to reduce file sizes. By
default, a Rust-based oxipng library is linked, which requires a Rust compiler.
Alternatively, an oxipng binary can be used. The library is significantly
faster.

Requirements:

* A C++23 compiler and library
* Qt6
* Rust (if a library-based oxipng is used), or
* Boost + oxipng (if an external oxipng tool is used)

To build, use GNU make. By default, the Rust-based oxipng is used:

    make

To use the oxipng binary (assumed to be /usr/bin/oxipng):

    make NO_LIBOXI=1

To run:

    ./bpal /path/to/blorb.blb

This will process the Blorb file and generate a file called `out.blb`.

You can also pass a Z-machine story file to bundle into the Blorb as an Exec
resource:

    ./bpal /path/to/blorb.blb /path/to/story.z6
