#!/usr/bin/env ruby

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

require 'nokogiri'

ADOBE_COEFFS = File.expand_path("../src/external/adobe_coeff.c", File.dirname(__FILE__))

IGNORED_DUPLICATE_ENTRIES=[
  "Sony NEX-3", "Sony NEX-5" # have both DCJ and Adobe entries, DCJ-one is used
]

def parse_line(hash, line)
  if line[0..6] != "    { \""
    return
  end

  cameramodel = line.split('"')[1]

  if hash.key?(cameramodel) and not IGNORED_DUPLICATE_ENTRIES.include? cameramodel
    puts "Camera #{cameramodel} has multiple entries!"
    return
  end

  p = line.split('{')[2].split('}')[0].chomp().split(',').map(&:strip)

  # p = p.map(&:to_i)
  p = p.map(&:to_f).map {|x| x / 10000.0 }

  if p.size != 9 and  p.size != 12
    puts "Camera #{cameramodel}, strange matrix? #{p}"
    exit
  end

  hash[cameramodel] = p
end

entries = {}
File.open(ADOBE_COEFFS) do |f|
  f.each do |line|
    parse_line(entries, line)
  end
end

rev = Hash.new{ |h,k| h[k]=[] }
entries.each{ |k,v| rev[v] << k }

rev.delete_if {|key, value| value.size == 1 }

list_cams_with_dup_matrixes = []
rev.each{ |k,v| v.each { |v| list_cams_with_dup_matrixes << v } }

rev.each do |mat, cameras|
  puts "Matrix #{mat} is listed for several cameras: #{cameras}"
end

DCP_XMPs = File.expand_path("../../_CAMERA_SUPPORT/xml/", File.dirname(__FILE__))

def parse_dcp_xml(hash, filename)
  File.open(filename) do |f|
    xml_doc  = Nokogiri::XML(f)
    xml_doc.css("dcpData").each do |d|
      cameramodel = d.css("UniqueCameraModelRestriction").text

      calibrationIlluminant2 = d.css("CalibrationIlluminant2").text.to_i

      if calibrationIlluminant2 != 21
        puts "In Adobe DCP, camera \"#{cameramodel}\" has CalibrationIlluminant2 which is not 21: #{calibrationIlluminant2}"
        return
      end

      colorMatrix2 = d.css("ColorMatrix2")
      colorMatrix2_rows = colorMatrix2.attribute("Rows").value.to_i
      colorMatrix2_cols = colorMatrix2.attribute("Cols").value.to_i

      matrix = Array.new(colorMatrix2_rows * colorMatrix2_cols)

      colorMatrix2.css("Element").each do |e|
        row = e.attribute("Row").value.to_i
        col = e.attribute("Col").value.to_i

        val = e.text.to_f

        # :(
        #val = (10000 * val).to_i

        matrix[colorMatrix2_cols*row + col] = val
      end

      hash[cameramodel] = matrix
    end
  end
end

xml_hash = {}
Dir.foreach(DCP_XMPs) do |item|
  next if item[-8..-1] != ".dcp.xml"

  parse_dcp_xml(xml_hash, File.expand_path(item, DCP_XMPs))
end

xmprev = Hash.new{ |h,k| h[k]=[] }
xml_hash.each{ |k,v| xmprev[v] << k }

xmprev.delete_if {|key, value| value.size == 1 }

xmprev.each do |mat, hash|
  puts "DCP Matrix #{mat} is listed for several cameras: #{hash}"
end

num_nonmatching_mat = 0
xml_hash.each do |cam, mat|
  if not entries.key?(cam)
    # puts "DCP camera name does not seem to be present in adobe_coeff: #{cam}"
    next
  end

  if mat == entries[cam]
    # puts "Matching cam: #{cam}"
    next
  end

  #if not list_cams_with_dup_matrixes.include?(cam)
    # puts "Matching cam: #{cam}"
    #next
  #end

  diff = entries[cam].zip(mat).map { |x, y| (x - y).abs }.reduce(0, :+)

  if (entries[cam]-mat).size < 9
    # puts "Skipping non-matching cam: #{cam}"
    next
  end

  num_nonmatching_mat += 1

  puts "DCP matrix for camera #{cam} does not match adobe_coeff: DCP: #{mat}; adobe_coeff: #{entries[cam]}; diff: #{diff}"
end

if num_nonmatching_mat
  puts "Count of non-match matrixes: #{num_nonmatching_mat}"
end


# vim: tabstop=2 expandtab shiftwidth=2 softtabstop=2
# kate: tab-width: 2; replace-tabs on; indent-width 2; tab-indents: off;
# kate: indent-mode ruby; remove-trailing-spaces modified;
