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

## Threading

This branch uses the kakadiu threading, but fails because it requires you to
kake all start/process/finish calls from a single thread, which libvips can't
do.

It typically fails with:

```
$ vips kakaduload ~/pics/k2.jp2 x.v
vips: ../threads/kdu_threads.cpp:4744: virtual void kdu_core::kdu_thread_context::enter_group(kdu_core::kdu_thread_entity*): Assertion `(group == NULL) && caller->exists()' failed.
Aborted (core dumped)
```

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
