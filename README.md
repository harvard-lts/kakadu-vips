# Load/save of jp2k for libvips using kakadu

This plugin adds load and save of jpeg2000 images using kakadu, a fast
commercial implementation of the jpeg2000 standard.

## Install

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

Test the plugin with:

```shell
$ pytest
============================= test session starts ==============================
platform linux -- Python 3.11.6, pytest-7.4.0, pluggy-1.2.0
rootdir: /home/john/GIT/kakadu-vips
collected 4 items

test/test_kakaduload.py ...                                              [ 75%]
test/test_kakadusave.py .                                                [100%]

============================== 4 passed in 0.54s ===============================
```

## Known cavets & limitations

- Gamma ad clipping issues with float images - see
  https://github.com/harvard-lts/kakadu-vips/issues/12

- Alpha channel should work, but needs testing

- CMYK, lab and greyscale should work, but need testing

- Palettised images should work, but need testing

- Chroma subsampling should work, but needs testing

- Multispectral images should work, but need testing
