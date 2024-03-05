# vim: set fileencoding=utf-8 :

import sys
import os
import shutil
import tempfile
import pytest

import pyvips
from helpers import *

class TestKakaduLoad:
    tempdir = None

    @classmethod
    def setup_class(cls):
        cls.tempdir = tempfile.mkdtemp()
        cls.ppm = pyvips.Image.ppmload(PPM_FILE)

    @classmethod
    def teardown_class(cls):
        shutil.rmtree(cls.tempdir, ignore_errors=True)

    def image_matches_file(self, image, filename):
        image_file = pyvips.Image.new_from_file(filename)
        assert image.width == image_file.width
        assert image.height == image_file.height
        assert image.bands == image_file.bands
        assert image.format == image_file.format
        assert (image - image_file).abs().max() < 10

    @skip_if_no("jp2kload")
    def test_kakaduload_file(self):
        image = pyvips.Image.kakaduload(JP2K_FILE)
        self.image_matches_file(image, JP2K_FILE) 

    @skip_if_no("jp2kload")
    def test_kakaduload_buffer(self):
        with open(JP2K_FILE, 'rb') as f:
            buf = f.read()
        image = pyvips.Image.kakaduload_buffer(buf)
        self.image_matches_file(image, JP2K_FILE) 

    @skip_if_no("jp2kload")
    def test_kakaduload_source_memory(self):
        with open(JP2K_FILE, 'rb') as f:
            buf = f.read()
        source = pyvips.Source.new_from_memory(buf)
        image = pyvips.Image.kakaduload_source(source)
        self.image_matches_file(image, JP2K_FILE) 

    def test_kakaduload_subsample(self):
        image = pyvips.Image.kakaduload(JP2K_FILE)
        big_average = image.avg()
        big_width = image.width
        big_height = image.height

        image = pyvips.Image.kakaduload(JP2K_FILE, page=1)
        assert image.width == big_width // 2
        assert image.height == big_height // 2
        assert abs(big_average - image.avg()) < 1

    def test_kakaduload_resolution(self):
        image = pyvips.Image.kakaduload(JP2K_RESOLUTION_FILE)
        assert abs(image.xres - 11.8) < 0.1
        assert abs(image.yres - 11.8) < 0.1
