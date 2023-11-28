# Load/save of jp2k for libvips using kakadu

This plugin adds load and save of jpeg2000 images using kakadu, a fast
commercial implementation of the jpeg2000 standard.

See `kakadu/README.md` for notes on downloading, configuring and building the
kakadu library.

You'll need libvips and the development headers.

Build and install this plugin with:

```bash
cd src
make
make install
```

This will copy `vips-kakadu.so` to your libvips module directory.

Run with eg.:

```
$ vips kakaduload
load JPEG2000 image
usage:
   kakaduload filename out [--option-name option-value ...]
where:
   filename     - Filename to load from, input gchararray
   out          - Output image, output VipsImage
optional arguments:
   page         - Load this page from the image, input gint
			default: 0
			min: 0, max: 100000
   flags        - Flags for this file, output VipsForeignFlags
			default flags: 
			allowed flags: none, partial, bigendian, sequential, all
   memory       - Force open via memory, input gboolean
			default: false
   access       - Required access pattern for this file, input VipsAccess
			default enum: random
			allowed enums: random, sequential, sequential-unbuffered
   fail-on      - Error level to fail on, input VipsFailOn
			default enum: none
			allowed enums: none, truncated, error, warning
   revalidate   - Don't use a cached result for this operation, input gboolean
			default: false
operation flags: untrusted 
$ vips kakaduload k2.jp2 x.v
vips_foreign_load_kakadu_build:
vips_foreign_load_kakadu_header:
vips_foreign_load_kakadu_set_header:
vips_foreign_load_kakadu_load:
vips_foreign_load_kakadu_set_header:
vips_foreign_load_kakadu_dispose:
vips_image_generate: demand hint not set
memory: high-water mark 216 bytes
error buffer: vips_image_generate: demand hint not set
```
