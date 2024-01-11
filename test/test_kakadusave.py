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

    @skip_if_no("kakaduload")
    @skip_if_no("kakadusave")
    @skip_if_no("jp2kload")
    def test_kakadusave_file(self):
        filename = temp_filename(self.tempdir, ".jp2")
        self.ppm.kakadusave(filename)
        self.image_matches_file(self.ppm, filename)

