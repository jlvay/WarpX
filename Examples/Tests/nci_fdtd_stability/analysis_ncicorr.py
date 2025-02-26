#!/usr/bin/env python3

# Copyright 2019 Jean-Luc Vay, Maxence Thevenet, Remi Lehe
# Weiqun Zhang
#
# This file is part of WarpX.
#
# License: BSD-3-Clause-LBNL


import os
import re
import sys

import numpy as np
import scipy.constants as scc
import yt

yt.funcs.mylog.setLevel(0)

sys.path.insert(1, "../../../../warpx/Regression/Checksum/")
import checksumAPI

fn = sys.argv[1]
use_MR = re.search("nci_correctorMR", fn) is not None

if use_MR:
    energy_corrector_off = 5.0e32
    energy_threshold = 1.0e28
else:
    energy_corrector_off = 1.5e26
    energy_threshold = 1.0e24

# Check EB energy after 1000 timesteps
filename = sys.argv[1]

ds = yt.load(filename)
ad0 = ds.covering_grid(
    level=0, left_edge=ds.domain_left_edge, dims=ds.domain_dimensions
)
ex = ad0["boxlib", "Ex"].v
ez = ad0["boxlib", "Ez"].v
by = ad0["boxlib", "By"].v
energy = np.sum(ex**2 + ez**2 + scc.c**2 * by**2)

print("use_MR: %s" % use_MR)
print("energy if corrector off (from benchmark): %s" % energy_corrector_off)
print("energy threshold (from benchmark): %s" % energy_threshold)
print("energy from this run: %s" % energy)

assert energy < energy_threshold

test_name = os.path.split(os.getcwd())[1]
checksumAPI.evaluate_checksum(test_name, filename)
