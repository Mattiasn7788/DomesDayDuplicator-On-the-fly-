#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# SPDX-License-Identifier: GPL-3.0
#
# RTL-SDR VHS HiFi capture - raw u8 output only
# Based on original script by Adam

from gnuradio import analog
from gnuradio import blocks
import math
from gnuradio import filter
from gnuradio.filter import firdes
from gnuradio import gr
from gnuradio.fft import window
import sys
import signal
import os
from argparse import ArgumentParser
from gnuradio import soapy


class rtlsdr_to_u8_only(gr.top_block):

    def __init__(self):
        gr.top_block.__init__(self, "RTL-SDR to u8 only", catch_exceptions=True)

        self.PAL = PAL = 1600000
        self.NTSC = NTSC = 1500000
        self.system = system = PAL
        self.samp_rate = samp_rate = 1024000

        # RTL-SDR source
        dev = 'driver=rtlsdr'
        stream_args = 'bufflen=16384'
        tune_args = ['']
        settings = ['']

        def _set_gain_mode(channel, agc):
            self.soapy_rtlsdr_source_0.set_gain_mode(channel, agc)
            if not agc:
                self.soapy_rtlsdr_source_0.set_gain(channel, self._gain_value)
        self.set_gain_mode = _set_gain_mode

        def _set_gain(channel, name, gain):
            self._gain_value = gain
            if not self.soapy_rtlsdr_source_0.get_gain_mode(channel):
                self.soapy_rtlsdr_source_0.set_gain(channel, gain)
        self.set_gain = _set_gain

        def _set_bias(bias):
            if 'biastee' in self._setting_keys:
                self.soapy_rtlsdr_source_0.write_setting('biastee', bias)
        self.set_bias = _set_bias

        self.soapy_rtlsdr_source_0 = soapy.source(dev, "fc32", 1, 'rtl=0,bias=1,direct_samp=0',
                                                   stream_args, tune_args, settings)
        self._setting_keys = [a.key for a in self.soapy_rtlsdr_source_0.get_setting_info()]
        self.soapy_rtlsdr_source_0.set_sample_rate(0, samp_rate)
        self.soapy_rtlsdr_source_0.set_frequency(0, system)
        self.soapy_rtlsdr_source_0.set_frequency_correction(0, 0)
        self.set_bias(bool(False))
        self._gain_value = 3
        self.set_gain_mode(0, bool(1))
        self.set_gain(0, 'TUNER', 3)

        # Baseband chain: resample → freq shift → complex to real → scale → u8
        self.mmse_resampler_xx_0 = filter.mmse_resampler_cc(0, (samp_rate/8000000))
        self.blocks_freqshift_cc_0 = blocks.rotator_cc(2.0*math.pi*system/8000000)
        self.blocks_complex_to_real_0 = blocks.complex_to_real(1)
        self.blocks_add_const_vxx_0 = blocks.add_const_ff(1)
        self.blocks_multiply_const_vxx_0 = blocks.multiply_const_ff(128)
        self.analog_rail_ff_1 = analog.rail_ff(0, 255)
        self.blocks_float_to_uchar_0 = blocks.float_to_uchar(1, 1, 0)
        _init_u8 = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gnuradio_hifi_init.u8').replace('\\', '/')
        self.blocks_file_sink_0 = blocks.file_sink(gr.sizeof_char*1, _init_u8, False)
        self.blocks_file_sink_0.set_unbuffered(False)

        # Connections
        self.connect((self.soapy_rtlsdr_source_0, 0), (self.mmse_resampler_xx_0, 0))
        self.connect((self.mmse_resampler_xx_0, 0), (self.blocks_freqshift_cc_0, 0))
        self.connect((self.blocks_freqshift_cc_0, 0), (self.blocks_complex_to_real_0, 0))
        self.connect((self.blocks_complex_to_real_0, 0), (self.blocks_add_const_vxx_0, 0))
        self.connect((self.blocks_add_const_vxx_0, 0), (self.blocks_multiply_const_vxx_0, 0))
        self.connect((self.blocks_multiply_const_vxx_0, 0), (self.analog_rail_ff_1, 0))
        self.connect((self.analog_rail_ff_1, 0), (self.blocks_float_to_uchar_0, 0))
        self.connect((self.blocks_float_to_uchar_0, 0), (self.blocks_file_sink_0, 0))

    def set_system(self, system):
        self.system = system
        self.blocks_freqshift_cc_0.set_phase_inc(2.0*math.pi*self.system/8000000)
        self.soapy_rtlsdr_source_0.set_frequency(0, self.system)


def main(top_block_cls=rtlsdr_to_u8_only, options=None):
    parser = ArgumentParser()
    parser.add_argument('--output', default='E:\\Hifi\\hifi', help='Output base path (without extension)')
    parser.add_argument('--system', default='PAL', choices=['PAL', 'NTSC'], help='TV system')
    parser.add_argument('--gain', type=float, default=0, help='Tuner gain in dB, 0 = auto')
    args = parser.parse_args()

    tb = top_block_cls()

    tb.blocks_file_sink_0.open(args.output + '.u8')
    tb.set_system(tb.PAL if args.system == 'PAL' else tb.NTSC)

    if args.gain == 0:
        tb.soapy_rtlsdr_source_0.set_gain_mode(0, True)
    else:
        tb.soapy_rtlsdr_source_0.set_gain_mode(0, False)
        tb.soapy_rtlsdr_source_0.set_gain(0, args.gain)

    def sig_handler(sig=None, frame=None):
        tb.stop()
        tb.wait()
        sys.exit(0)

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    tb.start()

    try:
        input('Press Enter to quit: ')
    except EOFError:
        pass
    tb.stop()
    tb.wait()


if __name__ == '__main__':
    main()
