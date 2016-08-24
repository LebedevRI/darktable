#!/usr/bin/env python3

import sys
import os.path
import re
import numpy
import matplotlib.pyplot as plt
from scipy.signal import argrelextrema

# TUNING OPTIONS

# log2 of the white level that was set in rawprepare module
# when generating this PFM
bitdepth = 16

# all the pixels that are no more than `max_delta_stops' stops
# darker then the brightest pixel are assumed to be properly overexposed,
# and are considered to be the actually usable, active pixels on sensor
max_delta_stops = 0.3

# max_delta, multiplier to the maximal value in the array
# NOTE: 0.5 (1.0 stop) is just a guess
#       assuming properly clipped image, it should probably be
#       closer to 0.75, but probably less than 0.9
max_delta = numpy.exp2(-(max_delta_stops))

# sanity check values
# which percent of the actually usable, active pixels on sensor can there be?
threshold_low = 0.95
threshold_high = 0.999

# END OF TUNING OPTIONS


def usage():
    sys.stderr.write("Usage: %s image.pfm\n" % sys.argv[0])
    sys.exit(2)

filename = ""


if len(sys.argv) != 2:
    usage()
elif len(sys.argv) == 2:
    if os.path.isfile(sys.argv[1]):
        filename = sys.argv[1]
    else:
        sys.stderr.write("Error: %s - file not found\n" % sys.argv[1])
        usage()


'''
Load a PFM file into a Numpy array.
NOTE: it will have a shape of H x W, not W x H.
NOTE: PFM has rows in reverse order.
Returns a tuple containing the loaded image and the scale factor from the file.
'''


def load_pfm(file):
    color = None
    width = None
    height = None
    scale = None
    endian = None

    header = file.readline().rstrip()
    if header == b'PF':
        color = True
    elif header == b'Pf':
        color = False
    else:
        raise Exception('Not a PFM file.')

    dim_match = re.match(br'^(\d+)\s(\d+)\s$', file.readline())
    if dim_match:
        width, height = map(int, dim_match.groups())
    else:
        raise Exception('Malformed PFM header.')

    scale = float(file.readline().rstrip())
    if scale < 0:  # little-endian
        endian = '<'
        scale = -scale
    else:
        endian = '>'  # big-endian

    data = numpy.fromfile(file, endian + 'f')
    shape = (height, width, 3) if color else (height, width)

    return numpy.reshape(data, shape), scale


'''
convert loaded PFM into format best suited for further computations
i.e. W x H x 1, rows in proper order
Returns prepared data array
'''


def prepare_pfm(data):
    # if color image, "convert" to grayscale
    if len(data.shape) == 3 and data.shape[2] == 3:  # color image
        # RGB -> gray, by dropping all but one channel
        data = data.transpose(2, 0, 1)
        data = data[0]

    # make image to have W x H shape
    data = numpy.moveaxis(data, 1, 0)

    # PFM has rows in reverse order
    data = numpy.fliplr(data)

    if data.shape[0] < data.shape[1]:
        raise Exception('Wrong image orientation?.')

    return data


'''
do a data sanity check - does the percentage of "clipped" pixels make sense?
Returns our initial guess about the count of usable pixels on given sensor.
'''


def check_values(pixel_threshold, num, p):
    # now, check if the image is sufficiently clipped
    if p < threshold_low or p > threshold_high:
        print(
            "With threshold {}, {:.3f}% pixels are clipped, wanted in [{:.3f}%, {:.3f}%] range"
            .format(pixel_threshold, 100.0 * p, 100.0 * threshold_low, 100.0 * threshold_high))
        # raise Exception('Sanity (clip) check failed.')

    return num, p


def sanity_check_histogram(data, pixel_threshold):
    # want 2 bins
    bins = [0, pixel_threshold, data.max()]

    hist = numpy.histogram(data, bins=bins)

    # print('hist', hist)

    assert hist[1][2] == data.max()
    assert hist[0].sum() == data.size

    num = hist[0][1]
    # which percent of pixels is in last bin?
    p = num / data.size
    # ip = 1.0 - p # the rest are not in the last bin

    return check_values(pixel_threshold, num, p)


def sanity_check_count(data, pixel_threshold):
    data_flat = data.flatten()

    # want all the pixels that are not less than threshold
    condition = data_flat >= pixel_threshold

    clipped_pixels = numpy.where(condition)[0]

    num = clipped_pixels.size
    # which percent of pixels is in last bin?
    p = num / data.size
    # ip = 1.0 - p # the rest are not in the last bin

    return check_values(pixel_threshold, num, p)


def sanity_check(data):
    # all the pixels that are brighter than this value are assumed to be
    # be the actually usable, active pixels on sensor
    pixel_threshold = max_delta * data.max()
    pixel_threshold = numpy.floor(pixel_threshold)

    val_hist = sanity_check_histogram(data, pixel_threshold)
    val_count = sanity_check_count(data, pixel_threshold)

    assert val_hist == val_count

    pixels_usable, pixels_usable_p = val_hist

    return pixels_usable, pixels_usable_p, pixel_threshold


def find_all_extremes(y):
    # we want extremes of the first difference !
    dy = numpy.diff(y)

    extreme_1 = argrelextrema(dy, numpy.less)[0]
    extreme_2 = argrelextrema(dy, numpy.greater)[0]

    return numpy.append(extreme_1, extreme_2)


def do_plot(y, extremes):
    plt.figure()
    plt.plot(y)
    # plt.step(x, y, where='post') # step is confusing
    # plt.xlim(xmin, xmax)
    # plt.ylim(ymin*min(y[xmin:xmax]), ymax*max(y[xmin:xmax]))
    plt.plot(extremes, y[extremes], "o", label="extreme")
    plt.legend()
    # print('11 -> ',x[extremes], y[extremes])


def print_x(selected, sum0, sum0_diff):
    sel_x = numpy.argwhere(sum0_diff == selected)

    assert selected == sum0_diff[sel_x]

    print('\tx ', sel_x)
    print('\ty ', sum0[sel_x])
    print('\tdy', sum0_diff[sel_x])

    return sel_x


def do_stuff(data, axis, axis_name):
    assert len(data.shape) == 2
    assert axis <= 1

    print('\n\t\t\t{} axis analysis'.format(axis_name))

    print('\nOriginal values: ')

    waveform = data.sum(axis=axis)
    waveform /= waveform.max()
    assert waveform.size == data.shape[1 - axis]

    print('\t W ({0}): '.format(waveform.size))
    print(waveform)

    waveform_diff = numpy.diff(waveform)
    waveform_diff = numpy.append(waveform_diff, 0.0)
    waveform_diff /= numpy.absolute(waveform_diff).max()
    assert waveform_diff.shape == waveform.shape

    print('\tdW ({0}): '.format(waveform_diff.size))
    print(waveform_diff)

    # x coordinates of extreme values of waveform
    extremes_x = find_all_extremes(waveform)  # local min+max
    assert numpy.max(waveform_diff) == numpy.max(waveform_diff[extremes_x])

    print('\textremes ({0}): '.format(extremes_x.size))
    print(extremes_x, '\n')

    if 0:
        # we expect to have no more than two global extremes, check that

        condition = numpy.absolute(
            waveform_diff[extremes_x]) >= 0.6 * numpy.absolute(waveform_diff).max()
        global_max_extremes_dy = numpy.extract(
            condition, waveform_diff[extremes_x])
        print('global extremes (dy)', global_max_extremes_dy, '\n')
        assert len(global_max_extremes_dy) <= 2

        condition = numpy.in1d(waveform_diff, global_max_extremes_dy)
        print(condition)
        print(waveform_diff[condition])
        print(waveform[condition])
        global_max_extremes_x = numpy.extract(
            condition, waveform_diff[extremes_x])
        print('global extremes (dy)', global_max_extremes_dy, '\n')
        assert len(global_max_extremes_dy) <= 2

    # return

    if 1:
        xmin = 45
        xmax = 55

        ymin = 0.5
        ymax = 1.5

        do_plot(waveform, extremes_x)
        plt.ylabel('value')
        plt.axhline(y=1, color='r')

        do_plot(numpy.absolute(waveform_diff), extremes_x)
        plt.ylabel('1-th differences')
        plt.axhline(y=0, color='r')

        # do_plot(waveform_diff, waveform_diff2)
        # plt.axhline(y=0, color='r')

        plt.xlabel('H')

        plt.show()

    # return

    # maximal value of first derivative of waveform
    print('max first derivative')
    sel_max = numpy.max(waveform_diff)
    sel_arg = numpy.argmax(waveform_diff)
    sel_x = print_x(sel_max, waveform, waveform_diff)

    # minimal value of first derivative of waveform
    print('min first derivative')
    sel_min = numpy.min(waveform_diff)
    sel_arg_2 = numpy.argmin(waveform_diff)
    sel_x_2 = print_x(sel_min, waveform, waveform_diff)

    # exit()

    print('\n')

    # and now, let's select next extrenum (with bigger x coordinate)
    print('next extrenum')

    extr_id = numpy.argwhere(extremes_x == sel_x)[0][1]
    print(extr_id)
    print(extremes_x[extr_id])
    print(extremes_x[extr_id + 1])

    sel_next = numpy.argmax(waveform_diff[extremes_x])
    sel_max = waveform_diff[extremes_x][sel_next + 1]
    next_x = print_x(sel_max, waveform, waveform_diff)

    crop = roundup(next_x, 2)

    print('next_x', next_x, 'but in reality', crop)  # works !

    return crop


def roundup(x, mul):
    return int(numpy.ceil(x / mul)) * mul


data, scale = load_pfm(open(filename, 'rb'))

data = prepare_pfm(data)

assert data.min() >= 0.0

print('Input image dimensions: ', data.shape)

pixel_count = data.size

print('Pixels, total:\t', pixel_count)

# want to operate on double-precision fp
data = data.astype(numpy.float64)

# normalize
# data /= data.max()

# want the max value to be some big integer, for histograms to make sense
data *= 2**bitdepth - 1

# assert 2**bitdepth - 1 == data.max()

print('\tmedian:\t', numpy.median(data))
print('\taverage:', numpy.average(data))
print('\tmean:\t', numpy.mean(data))
print('\tstd:\t', numpy.std(data))
print('\tvar:\t', numpy.var(data))
print('\tmin:\t', numpy.min(data))
print('\tmax:\t', numpy.max(data))
print('\tsum:\t', numpy.sum(data))

# exit()

# now do a sanity check

pixels_usable, pixels_usable_p, pixel_threshold = sanity_check(data)
print('Pixels, usable: {0} ({1:f}%) (threshold = {2})'
      .format(pixels_usable, 100.0 * pixels_usable_p, pixel_threshold))

# show histogram of whole image with all the bins
if 0:
    all_bins = numpy.bincount(data.flatten().astype(numpy.int64))
    print('bincount', all_bins)
    print('bincount', all_bins.size)

    all_bins_diff = numpy.diff(all_bins)
    all_bins_diff = numpy.append(all_bins_diff, 0.0)
    assert all_bins_diff.shape == all_bins.shape

    plt.figure()
    plt.plot(numpy.array(range(0, all_bins.size)), all_bins)
    plt.show()
    exit()

# okay, now the fun begins

crop_y = do_stuff(data, 0, "X")
crop_x = do_stuff(data, 1, "Y")

print('\n\n\n')
print('<Crop x="{0}" y="{1}" width="{2}" height="{3}"/>'
      .format(crop_x, crop_y, '???', '???'))

# vim: tabstop=8 expandtab shiftwidth=4 softtabstop=4
# kate: tab-width: 8; replace-tabs on; indent-width 4; tab-indents: off;
# kate: indent-mode python; remove-trailing-spaces modified;
