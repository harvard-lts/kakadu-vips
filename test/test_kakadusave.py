# vim: set fileencoding=utf-8 :

import sys
import os
import shutil
import tempfile
import pytest

import pyvips
from helpers import *

class TestKakaduSave:
    tempdir = None

    @classmethod
    def setup_class(cls):
        cls.tempdir = tempfile.mkdtemp()
        cls.ppm = pyvips.Image.ppmload(PPM_FILE)

    @classmethod
    def teardown_class(cls):
        shutil.rmtree(cls.tempdir, ignore_errors=True)

    def image_matches_file(self, image, filename, threshold=10):
        image_file = pyvips.Image.new_from_file(filename)
        assert image.width == image_file.width
        assert image.height == image_file.height
        assert image.bands == image_file.bands
        assert image.format == image_file.format
        assert (image - image_file).abs().max() < threshold

    @skip_if_no("kakaduload")
    @skip_if_no("kakadusave")
    @skip_if_no("jp2kload")
    def test_kakadusave_file(self):
        filename = temp_filename(self.tempdir, ".jp2")
        self.ppm.kakadusave(filename)
        self.image_matches_file(self.ppm, filename)

    @skip_if_no("kakadusave")
    @skip_if_no("jp2kload")
    def test_kakadusave_buffer(self):
        buf = self.ppm.kakadusave_buffer()
        image = pyvips.Image.new_from_buffer(buf, "")
        self.image_matches_file(image, PPM_FILE)

    @skip_if_no("kakaduload")
    @skip_if_no("kakadusave")
    @skip_if_no("jp2kload")
    def test_kakadusave_options(self):
        q1 = self.ppm.kakadusave_buffer(options="Qfactor=1")
        q99 = self.ppm.kakadusave_buffer(options="Qfactor=99")
        assert len(q1) < len(q99)

    @skip_if_no("kakaduload")
    def test_kakadusave_resolution(self):
        image = pyvips.Image.kakaduload(JP2K_RESOLUTION_FILE)
        filename = temp_filename(self.tempdir, ".jp2")
        image.kakadusave(filename)
        image = pyvips.Image.kakaduload(filename)
        assert abs(image.xres - 11.8) < 0.1
        assert abs(image.yres - 11.8) < 0.1

    @skip_if_no("kakadusave")
    def test_kakadusave_rate(self):
        data1 = self.ppm.kakadusave_buffer(rate=1)
        data10 = self.ppm.kakadusave_buffer(rate=10)
        assert len(data1) < len(data10)

        image1 = pyvips.Image.kakaduload_buffer(data1)
        self.image_matches_file(image1, PPM_FILE, 100)

        image10 = pyvips.Image.kakaduload_buffer(data10)
        self.image_matches_file(image10, PPM_FILE, 10)

