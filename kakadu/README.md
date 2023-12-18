# Build and install kakadu


## Download

Download the sources and unzip here to make eg.:

```
$ ls
README.md  v8_3-02172N  v8_3-02172N.zip
```

There's a gitignore rule to stop this licenced software being uploaded to
github.

You probably won't get exactly the same version. The makefile for
`kakadu-vips` will adjust, so it doesn't matter.

See `v8_3-02172N/Compiling_Instructions.txt` for detailed notes.

## Enable high throughput jp2k

You may have HTJP2K enabled already, if not, edit
`v8_3-02172N/coresys/fast_coding/fbc_common.h` and uncomment `FBC_ENABLED`.
You can avoid editing the source by using `make CXXFLAGS=-DFBC_ENABLED ...`
when building coresys, of course.

```
$ cd v8_3-02172N
$ mv srclib_ht srclib_ht_noopt
$ cp -r altlib_ht_opt srclib_ht
```

## Prerequisites

You might not have `libnuma`. The apps can also use libtiff, but it's pain
to set up and we don't need it.

```
$ sudo apt install libnuma-dev
```

## `coresys`

Build the core library with:

```
$ cd v8_3-02172N/coresys/make
$ make -f Makefile-Linux-x86-64-gcc
```

And the aux library with:

```
$ cd v8_3-02172N/managed/make
$ make -f Makefile-Linux-x86-64-gcc all_but_jni
```

Makes:

```
$ ls v8_3-02172N/lib/Linux-x86-64-gcc/
libkdu.a  libkdu_a83R.so  libkdu_aux.a  libkdu_v83R.so
$ ls v8_3-02172N/coresys/common/*.h
v8_3-02172N/coresys/common/kdu_arch.h
v8_3-02172N/coresys/common/kdu_block_coding.h
v8_3-02172N/coresys/common/kdu_compressed.h
v8_3-02172N/coresys/common/kdu_elementary.h
v8_3-02172N/coresys/common/kdu_kernels.h
v8_3-02172N/coresys/common/kdu_messaging.h
v8_3-02172N/coresys/common/kdu_params.h
v8_3-02172N/coresys/common/kdu_roi_processing.h
v8_3-02172N/coresys/common/kdu_sample_processing.h
v8_3-02172N/coresys/common/kdu_threads.h
v8_3-02172N/coresys/common/kdu_ubiquitous.h
v8_3-02172N/coresys/common/kdu_utils.h
```

`libkdu_v83R.so` means version 8.3, release mode.

## Debugging

You have to edit the makefiles for a debug build -- just change `-O2` to `-g`,
then `make clean` and `make` again.

### TODO

- `Compiling_Instructions.txt` talks about needing to define 
  `_FILE_OFFSET_BITS` and `_LARGEFILE64_SOURCE`  to get large file support,
  check this

## `apps`

Build command-line apps and utilities with:


```
$ cd v8_3-02172N/apps/make
$ make -f Makefile-Linux-x86-64-gcc
```

To make:

```
$ ls v8_3-02172N/bin/Linux-x86-64-gcc/
kdu_buffered_compress  kdu_makeppm       kdu_stream_expand   kdu_vex_fast
kdu_buffered_expand    kdu_maketlm       kdu_stream_send     kdu_v_expand
kdu_compress           kdu_merge         kdu_text_extractor  simple_example_c
kdu_expand             kdu_render        kdu_transcode       simple_example_d
kdu_hyperdoc           kdu_server        kdu_vcom_fast
kdu_jp2info            kdu_server_admin  kdu_v_compress
```

This also builds the documentation from source code comments using 
`kdu_hyperdoc`:

```
$ firefox v8_3-02172N/documentation/index.html 
```

## Install

Append something like this to your `.bashrc`:

```
export KAKADUHOME=/home/john/GIT/kakadu-vips/kakadu/v8_3-02172N
export PATH="$KAKADUHOME/bin/Linux-x86-64-gcc:$PATH"
export LD_LIBRARY_PATH="$KAKADUHOME/lib/Linux-x86-64-gcc:$LD_LIBRARY_PATH"
```

## Test

The TIFF loader built into kakadu only supports uncompressed tiff. Try
something like:

```
$ vips copy k2.jpg x.tif
$ kdu_compress -i x.tif -o x.jp2
```

The result is pretty big, you'll need to set some options for a better
filesize.
