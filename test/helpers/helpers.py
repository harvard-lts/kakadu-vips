# vim: set fileencoding=utf-8 :
# test helpers

import os
import tempfile
import pytest
import pyvips

IMAGES = os.path.join(os.path.dirname(__file__), os.pardir, 'images')
PPM_FILE = os.path.join(IMAGES, "sample_640×426.ppm")
JP2K_FILE = os.path.join(IMAGES, "world.jp2")
JP2K_RESOLUTION_FILE = os.path.join(IMAGES, "church.jp2")
JPEG_FILE = os.path.join(IMAGES, "test_800x600.jpg")

unsigned_formats = ["uchar", "ushort", "uint"]
signed_formats = ["char", "short", "int"]
float_formats = ["float", "double"]
complex_formats = ["complex", "dpcomplex"]
int_formats = unsigned_formats + signed_formats
noncomplex_formats = int_formats + float_formats
all_formats = int_formats + float_formats + complex_formats

colour_colourspaces = ["xyz", "lab", "lch", "cmc", "labs", "scrgb", 
                       "hsv", "srgb", "yxy"]
cmyk_colourspaces = ["cmyk"]
coded_colourspaces = ["labq"]
mono_colourspaces = ["b-w"]
sixteenbit_colourspaces = ["grey16", "rgb16"]
all_colourspaces = colour_colourspaces + mono_colourspaces + \
                   coded_colourspaces + sixteenbit_colourspaces + \
                   cmyk_colourspaces

max_value = {"uchar": 0xff,
             "ushort": 0xffff,
             "uint": 0xffffffff,
             "char": 0x7f,
             "short": 0x7fff,
             "int": 0x7fffffff,
             "float": 1.0,
             "double": 1.0,
             "complex": 1.0,
             "dpcomplex": 1.0}

sizeof_format = {"uchar": 1,
                 "ushort": 2,
                 "uint": 4,
                 "char": 1,
                 "short": 2,
                 "int": 4,
                 "float": 4,
                 "double": 8,
                 "complex": 8,
                 "dpcomplex": 16}

rot45_angles = ["d0", "d45", "d90", "d135", "d180", "d225", "d270", "d315"]
rot45_angle_bonds = ["d0", "d315", "d270", "d225", "d180", "d135", "d90", "d45"]

rot_angles = ["d0", "d90", "d180", "d270"]
rot_angle_bonds = ["d0", "d270", "d180", "d90"]


# an expanding zip ... if either of the args is a scalar or a one-element list,
# duplicate it down the other side
def zip_expand(x, y):
    # handle singleton list case
    if isinstance(x, list) and len(x) == 1:
        x = x[0]
    if isinstance(y, list) and len(y) == 1:
        y = y[0]

    if isinstance(x, list) and isinstance(y, list):
        return list(zip(x, y))
    elif isinstance(x, list):
        return [[i, y] for i in x]
    elif isinstance(y, list):
        return [[x, j] for j in y]
    else:
        return [[x, y]]


# run a 1-ary function on a thing -- loop over elements if the
# thing is a list
def run_fn(fn, x):
    if isinstance(x, list):
        return [fn(i) for i in x]
    else:
        return fn(x)


# make a temp filename with the specified suffix and in the
# specified directory
def temp_filename(directory, suffix):
    temp_name = next(tempfile._get_candidate_names())
    filename = os.path.join(directory, temp_name + suffix)

    return filename


# test for an operator exists
def have(name):
    return pyvips.type_find("VipsOperation", name) != 0


def skip_if_no(operation_name):
    return pytest.mark.skipif(not have(operation_name),
                        reason='no {}, skipping test'.format(operation_name))


