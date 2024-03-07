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

    @skip_if_no("jp2kload")
    def test_kakadusave_file(self):
        filename = temp_filename(self.tempdir, ".jp2")
        self.ppm.kakadusave(filename)
        self.image_matches_file(self.ppm, filename)

    def test_kakadusave_buffer(self):
        buf = self.ppm.kakadusave_buffer()
        image = pyvips.Image.new_from_buffer(buf, "")
        self.image_matches_file(image, PPM_FILE)

    def test_kakadusave_options(self):
        q1 = self.ppm.kakadusave_buffer(options="Qfactor=1")
        q99 = self.ppm.kakadusave_buffer(options="Qfactor=99")
        assert len(q1) < len(q99)

    def test_kakadusave_resolution(self):
        image = pyvips.Image.kakaduload(JP2K_RESOLUTION_FILE)
        filename = temp_filename(self.tempdir, ".jp2")
        image.kakadusave(filename)
        image = pyvips.Image.kakaduload(filename)
        assert abs(image.xres - 11.8) < 0.1
        assert abs(image.yres - 11.8) < 0.1

    def test_kakadusave_htj2k(self):
        data1 = self.ppm.kakadusave_buffer()
        data2 = self.ppm.kakadusave_buffer(htj2k=True)

        # ie. the option has had an effect
        assert len(data1) != len(data2)

    def test_kakadusave_tlm(self):
        # tlm needs working rewrite in target
        data = self.ppm.kakadusave_buffer(options="Qfactor=90 Cmodes=HT ORGgen_plt=yes Creversible=no Cblk={64,64} ORGtparts=R ORGgen_tlm=9")
        assert len(data) > 100
        image = pyvips.Image.kakaduload_buffer(data)
        self.image_matches_file(image, PPM_FILE, 15)

    def test_kakadusave_rate(self):
        data1 = self.ppm.kakadusave_buffer(rate=1)
        data10 = self.ppm.kakadusave_buffer(rate=10)
        assert len(data1) < len(data10)

        image1 = pyvips.Image.kakaduload_buffer(data1)
        self.image_matches_file(image1, PPM_FILE, 100)

        image10 = pyvips.Image.kakaduload_buffer(data10)
        self.image_matches_file(image10, PPM_FILE, 10)

    def test_kakadusave_profile(self):
        data = self.ppm.kakadusave_buffer(profile="srgb")
        image = pyvips.Image.kakaduload_buffer(data)
        assert len(image.get("icc-profile-data")) == 480
