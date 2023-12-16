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

```shell
vips kakaduload ~/pics/k2.jp2 x.jpg
```

to load a jpeg2000  image and save as a regular jpeg. It should also run from 
python etc.

## TODO

- threaded load (it's all single-threaded for now)

- implement shrink-on-load via the page parameter

- 16 bit and float images should work, but need testing

- alpha images should work, but need testing

- cmyk, lab and greyscale should work, but need testing

- palettised images should work, but need testing

- chroma subsampling should work, but needs testing

- multispectral images should work, but need testing

- implement `kakadusave`
