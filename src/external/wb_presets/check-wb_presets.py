#!/usr/bin/env python3

# MIT License
#
# Copyright (c) 2016 Roman Lebedev
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import sys
import os.path
import json


def usage():
    sys.stderr.write("Usage: check-wb_presets.py wb_presets.json\n")
    sys.stderr.write("Note: the JSON should conform to wb_presets.schema !\n")
    sys.exit(1)


def check_wb_preset_model_preset_tuning(pos, tuning, known_tunings):
    assert type(tuning) is dict
    assert "tuning" in tuning
    assert type(tuning["tuning"]) is int
    assert "coeffients" in tuning
    assert type(tuning["coeffients"]) is list
    assert len(tuning["coeffients"]) == 4
    assert tuning["coeffients"][0] > 0.0
    assert tuning["coeffients"][1] > 0.0
    assert tuning["coeffients"][2] > 0.0
    assert tuning["coeffients"][3] == 0.0

    cur_tuning = tuning["tuning"]

    pos += ', tuning "%i"' % cur_tuning

    if len(known_tunings):
        assert cur_tuning > known_tunings[
            -1], pos + ' is not monotonically increasing'

    assert cur_tuning not in known_tunings, pos + ' is duplicated'

    known_tunings.append(cur_tuning)

    return known_tunings


def check_wb_preset_model_preset(pos, preset, known_presets):
    assert type(preset) is dict
    assert ((("name" in preset) and (type(preset["name"]) is str)) ^ (
        ("temperature" in preset) and (type(preset["temperature"]) is int)))
    assert "tunings" in preset
    assert type(preset["tunings"]) is list
    assert len(preset["tunings"]) >= 1

    if "name" in preset:
        name = preset["name"]
    else:
        name = "%iK" % preset["temperature"]

    pos += ', preset "%s"' % name
    assert name not in known_presets, pos + ' is duplicated'
    known_presets.append(name)

    tunings = []
    for tuning in preset["tunings"]:
        tunings = check_wb_preset_model_preset_tuning(pos, tuning, tunings)

    if len(tunings) == 1:
        assert tunings[0] == 0, pos + " has only one tuning, which is non-zero"

    return known_presets


def check_wb_preset_model(pos, model, known_models):
    assert type(model) is dict
    assert "model" in model
    assert type(model["model"]) is str
    assert "presets" in model
    assert type(model["presets"]) is list
    assert len(model["presets"]) >= 1

    cur_model = model["model"]

    pos += ', model "%s"' % cur_model
    assert cur_model not in known_models, pos + ' is duplicated'
    known_models.append(cur_model)

    presets = []
    for preset in model["presets"]:
        presets = check_wb_preset_model_preset(pos, preset, presets)

    return known_models


def check_wb_preset(wb_preset, known_makers):
    assert type(wb_preset) is dict
    assert "maker" in wb_preset
    assert type(wb_preset["maker"]) is str
    assert "models" in wb_preset
    assert type(wb_preset["models"]) is list
    assert len(wb_preset["models"]) >= 1

    maker = wb_preset["maker"]

    pos = 'Maker "%s"' % maker
    assert maker not in known_makers, pos + ' is duplicated'
    known_makers.append(maker)

    models = []
    for model in wb_preset["models"]:
        models = check_wb_preset_model(pos, model, models)

    return known_makers


def check_wb_presets(wb_presets):
    assert wb_presets
    assert type(wb_presets) is list
    assert len(wb_presets) >= 1

    makers = []
    for wb_preset in wb_presets:
        makers = check_wb_preset(wb_preset, makers)


def check_root():
    data = json.load(open(sys.argv[1]))

    assert data
    assert type(data) is dict

    assert "version" in data
    assert type(data["version"]) is int
    assert 0 == data["version"], 'Version %i is unknown' % data["version"]

    assert "wb_presets" in data
    assert data["wb_presets"]

    check_wb_presets(data["wb_presets"])

    # print(json.dumps(data, indent=4))

if len(sys.argv) != 2 or not os.path.exists(sys.argv[1]):
    usage()
else:
    check_root()

# vim: tabstop=8 expandtab shiftwidth=4 softtabstop=4
# kate: tab-width: 8; replace-tabs on; indent-width 4; tab-indents: off;
# kate: indent-mode python; remove-trailing-spaces modified;
