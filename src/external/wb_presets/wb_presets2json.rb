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

require 'json'

# $ ./wb_presets2json.rb | python3 -m json.tool > wb_presets.json

class WbPresetTuningInstanceClass
  attr_reader :tuning
  attr_reader :coeffients # array of 4x string or number

  def to_json(options = nil)
    {'tuning' => @tuning.to_i, 'coeffients' => @coeffients.map(&:to_f)}.to_json
  end

  def initialize(tuning, coeffients)
    exit if coeffients.size != 4

    @tuning     = tuning
    @coeffients = coeffients
  end
end

class WbPresetPresetInstanceClass
  attr_reader :name
  attr_reader :tunings # array of WbPresetTuningInstanceClass

  def to_json(options = nil)
    if @name.strip[-1].upcase != "K"
      {'name' => @name, 'tunings' => @tunings}.to_json
    else
      {'temperature' => @name.strip[0..-1].to_i, 'tunings' => @tunings}.to_json
    end
  end

  def initialize(name, tunings)
    @name    = name
    @tunings = tunings.sort_by { |e| e.tuning }
  end
end

class WbPresetModelInstanceClass
  attr_reader :comment
  attr_reader :model
  attr_reader :presets # array of WbPresetPresetInstanceClass

  def to_json(options = nil)
    if @comment.size != 0
      {'comment' => @comment, 'model' => @model, 'presets' => @presets}.to_json
    else
      {'model' => @model, 'presets' => @presets}.to_json
    end
  end

  def initialize(comment = "", model, presets)
    @comment = comment
    @model   = model
    @presets = presets
  end
end

class WbPresetMakerInstanceClass
  attr_reader :maker
  attr_reader :models # array of WbPresetModelInstanceClass

  def to_json(options = nil)
    {'maker' => @maker, 'models' => @models}.to_json
  end

  def initialize(maker, models)
    @maker  = maker
    @models = models.sort_by { |e| e.model }
  end
end

VERSION = 0

class WbPresetsClass
  attr_reader :version
  attr_reader :wb_presets # array of WbPresetMakerInstanceClass

  def to_json(options = nil)
    {'version' => @version, 'wb_presets' => @wb_presets}.to_json
  end

  def initialize(version, wb_presets)
    @version  = version
    @wb_presets = wb_presets.sort_by { |e| e.maker }
  end
end


module WbPresetsCommon
  def self.map_to_hash(hash, map)
    hash = Hash.new if not hash

    hash[map[0]] = Hash.new if not hash.key?(map[0])

    hash[map[0]][map[1]] = Hash.new if not hash[map[0]].key?(map[1])

    hash[map[0]][map[1]][map[2]] = Hash.new if not hash[map[0]][map[1]].key?(map[2])

    if hash[map[0]][map[1]][map[2]].key?(map[3].to_i)
      puts "DUPLICATE!!! #{map}"
      exit
    end

    hash[map[0]][map[1]][map[2]][map[3].to_i] = [map[4], map[5], map[6], map[7]]
  end

  # normalize so that g is 1.0
  def self.normalize(p)
    return if 1 or p[5].to_f == 1.0

    g = p[5].to_f

    r = p[4].to_f / g
    b = p[6].to_f / g
    g2 = p[7].to_f / g
    g = 1

    g2 = 0 if g2 == 0.0

    p[4] = r
    p[5] = g
    p[6] = b
    p[7] = g2
  end

  def self.parse_preset(hash, line, upcase)
    return if line[0..2] != "  {"

    lineparts = line.split('"')

    cameraname = ""

    if(upcase)
      cameraname = [lineparts[1].upcase, lineparts[3].upcase]
    else
      cameraname = [lineparts[1], lineparts[3]]
    end

    exit if cameraname.join.strip == ""

    p = line.delete('{}"').chomp(",").split(",").map(&:strip).first(8)

    if p.size != 8
      puts "#{p}"
      exit
    end

    p[0] = cameraname[0]
    p[1] = cameraname[1]

    if p[3].to_i.abs > 9
      puts ["|tuning| > 9 !", p]
      exit
    end

    if p[-1].to_f != 0.0
      puts "g2 != 0.0 #{p}"
      exit
    end

    # normalize(p)

    map_to_hash(hash, p)
  end

  def self.print_struct(map)
    map.each do |key0, value0|
      value0.each do |key1, value1|
        value1.each do |key2, value2|
          if key2.strip[-1].upcase == "K"
            key2 = "\"#{key2}\""
          end

          value2.each do |key3, value3|
            puts "  { \"#{key0}\", \"#{key1}\", #{key2}, #{key3}, { #{value3[0]}, #{value3[1]}, #{value3[2]}, #{value3[3]} } },"
          end
        end

        puts
      end
    end
  end

  def self.hash_to_object(hash)
    makers = []

    hash.each do |key0, value0|
      models = []

      value0.each do |key1, value1|
        presets = []

        value1.each do |key2, value2|
          tunings = []

          value2.each do |key3, value3|
            tunings << WbPresetTuningInstanceClass.new(key3, value3)
          end

          presets << WbPresetPresetInstanceClass.new(key2, tunings)
        end

        models << WbPresetModelInstanceClass.new(key1, presets)
      end

      makers << WbPresetMakerInstanceClass.new(key0, models)
    end

    return WbPresetsClass.new(VERSION, makers)
  end

  HPRESETS=File.expand_path("../wb_presets.c", File.dirname(__FILE__))

  def self.parse_struct()
    presets = {}
    File.open(HPRESETS) do |f|
      f.each do |line|
        parse_preset(presets, line, false)
      end
    end

    return presets
  end
end

presets = WbPresetsCommon.parse_struct()

object = WbPresetsCommon.hash_to_object(presets)

puts JSON.pretty_generate(object)
exit

# WbPresetsCommon.print_struct(presets)

# vim: tabstop=2 expandtab shiftwidth=2 softtabstop=2
# kate: tab-width: 2; replace-tabs on; indent-width 2; tab-indents: off;
# kate: indent-mode ruby; remove-trailing-spaces modified;
