#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# GNU Radio Python Flow Graph
# Title: RTL-SDR to flac
# Author: Adam
# Copyright: none
# Description: VHS hifi tap > RTL-SDR > flac
# GNU Radio version: 3.10.10.0

from gnuradio import analog
from gnuradio import audio
from gnuradio import blocks
import math
from gnuradio import filter
from gnuradio.filter import firdes
from gnuradio import gr
from gnuradio.fft import window
import sys
import signal
from argparse import ArgumentParser
from gnuradio.eng_arg import eng_float, intx
from gnuradio import eng_notation
from gnuradio import soapy
import os




class rtlsdr_to_flac(gr.top_block):

    def __init__(self):
        gr.top_block.__init__(self, "RTL-SDR to flac", catch_exceptions=True)

        ##################################################
        # Variables
        ##################################################
        self.PAL = PAL = 1600000
        self.volume = volume = .7
        self.system = system = PAL
        self.samp_rate = samp_rate = 1024000
        self.lowpass_transition_width = lowpass_transition_width = 5000
        self.lowpass_gain = lowpass_gain = 30
        self.lowpass_cutoff = lowpass_cutoff = 100000
        self.baseband_sample_rate = baseband_sample_rate = 384000
        self.audio_sample_rate = audio_sample_rate = 48000
        self.NTSC = NTSC = 1500000

        ##################################################
        # Blocks
        ##################################################

        self.soapy_rtlsdr_source_0 = None
        dev = 'driver=rtlsdr'
        stream_args = 'bufflen=16384'
        tune_args = ['']
        settings = ['']

        def _set_soapy_rtlsdr_source_0_gain_mode(channel, agc):
            self.soapy_rtlsdr_source_0.set_gain_mode(channel, agc)
            if not agc:
                  self.soapy_rtlsdr_source_0.set_gain(channel, self._soapy_rtlsdr_source_0_gain_value)
        self.set_soapy_rtlsdr_source_0_gain_mode = _set_soapy_rtlsdr_source_0_gain_mode

        def _set_soapy_rtlsdr_source_0_gain(channel, name, gain):
            self._soapy_rtlsdr_source_0_gain_value = gain
            if not self.soapy_rtlsdr_source_0.get_gain_mode(channel):
                self.soapy_rtlsdr_source_0.set_gain(channel, gain)
        self.set_soapy_rtlsdr_source_0_gain = _set_soapy_rtlsdr_source_0_gain

        def _set_soapy_rtlsdr_source_0_bias(bias):
            if 'biastee' in self._soapy_rtlsdr_source_0_setting_keys:
                self.soapy_rtlsdr_source_0.write_setting('biastee', bias)
        self.set_soapy_rtlsdr_source_0_bias = _set_soapy_rtlsdr_source_0_bias

        self.soapy_rtlsdr_source_0 = soapy.source(dev, "fc32", 1, 'rtl=0,bias=1,direct_samp=0',
                                  stream_args, tune_args, settings)

        self._soapy_rtlsdr_source_0_setting_keys = [a.key for a in self.soapy_rtlsdr_source_0.get_setting_info()]

        self.soapy_rtlsdr_source_0.set_sample_rate(0, samp_rate)
        self.soapy_rtlsdr_source_0.set_frequency(0, system)
        self.soapy_rtlsdr_source_0.set_frequency_correction(0, 0)
        self.set_soapy_rtlsdr_source_0_bias(bool(False))
        self._soapy_rtlsdr_source_0_gain_value = 3
        self.set_soapy_rtlsdr_source_0_gain_mode(0, bool(1))
        self.set_soapy_rtlsdr_source_0_gain(0, 'TUNER', 3)
        self.mmse_resampler_xx_2 = filter.mmse_resampler_cc(0, (256/int(baseband_sample_rate/1000)))
        self.mmse_resampler_xx_1 = filter.mmse_resampler_cc(0, (256/int(baseband_sample_rate/1000)))
        self.mmse_resampler_xx_0 = filter.mmse_resampler_cc(0, (samp_rate/8000000))
        self.low_pass_filter_1 = filter.fir_filter_ccf(
            1,
            firdes.low_pass(
                lowpass_gain,
                baseband_sample_rate,
                lowpass_cutoff,
                lowpass_transition_width,
                window.WIN_HAMMING,
                6.76))
        self.low_pass_filter_0 = filter.fir_filter_ccf(
            1,
            firdes.low_pass(
                lowpass_gain,
                baseband_sample_rate,
                lowpass_cutoff,
                lowpass_transition_width,
                window.WIN_HAMMING,
                6.76))
        self.freq_xlating_fir_filter_xxx_1 = filter.freq_xlating_fir_filter_ccf((int(samp_rate/256000)), firdes.low_pass(1,samp_rate,2*samp_rate/256000, 200000), 200000, samp_rate)
        self.freq_xlating_fir_filter_xxx_0 = filter.freq_xlating_fir_filter_ccf((int(samp_rate/256000)), firdes.low_pass(1,samp_rate,2*int(samp_rate/256000), 200000), (-200000), samp_rate)
        _init_flac = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gnuradio_hifi_init.flac').replace('\\', '/')
        self.blocks_wavfile_sink_0 = blocks.wavfile_sink(
            _init_flac,
            2,
            audio_sample_rate,
            blocks.FORMAT_FLAC,
            blocks.FORMAT_PCM_24,
            False
            )
        self.blocks_multiply_const_vxx_2 = blocks.multiply_const_ff(volume)
        self.blocks_multiply_const_vxx_1 = blocks.multiply_const_ff(volume)
        self.blocks_multiply_const_vxx_0 = blocks.multiply_const_ff(128)
        self.blocks_freqshift_cc_0 = blocks.rotator_cc(2.0*math.pi*system/8000000)
        self.blocks_float_to_uchar_0 = blocks.float_to_uchar(1, 1, 0)
        _init_u8 = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gnuradio_hifi_init.u8').replace('\\', '/')
        self.blocks_file_sink_0 = blocks.file_sink(gr.sizeof_char*1, _init_u8, False)
        self.blocks_file_sink_0.set_unbuffered(False)
        self.blocks_complex_to_real_0 = blocks.complex_to_real(1)
        self.blocks_add_const_vxx_0 = blocks.add_const_ff(1)
        self.audio_sink_0 = audio.sink(48000, '', True)
        self.analog_wfm_rcv_1 = analog.wfm_rcv(
            quad_rate=baseband_sample_rate,
            audio_decimation=(int(baseband_sample_rate/audio_sample_rate)),
        )
        self.analog_wfm_rcv_0 = analog.wfm_rcv(
            quad_rate=baseband_sample_rate,
            audio_decimation=(int(baseband_sample_rate/audio_sample_rate)),
        )
        self.analog_rail_ff_1 = analog.rail_ff(0, 255)


        ##################################################
        # Connections
        ##################################################
        self.connect((self.analog_rail_ff_1, 0), (self.blocks_float_to_uchar_0, 0))
        self.connect((self.analog_wfm_rcv_0, 0), (self.blocks_multiply_const_vxx_2, 0))
        self.connect((self.analog_wfm_rcv_1, 0), (self.blocks_multiply_const_vxx_1, 0))
        self.connect((self.blocks_add_const_vxx_0, 0), (self.blocks_multiply_const_vxx_0, 0))
        self.connect((self.blocks_complex_to_real_0, 0), (self.blocks_add_const_vxx_0, 0))
        self.connect((self.blocks_float_to_uchar_0, 0), (self.blocks_file_sink_0, 0))
        self.connect((self.blocks_freqshift_cc_0, 0), (self.blocks_complex_to_real_0, 0))
        self.connect((self.blocks_multiply_const_vxx_0, 0), (self.analog_rail_ff_1, 0))
        self.connect((self.blocks_multiply_const_vxx_1, 0), (self.audio_sink_0, 1))
        self.connect((self.blocks_multiply_const_vxx_1, 0), (self.blocks_wavfile_sink_0, 1))
        self.connect((self.blocks_multiply_const_vxx_2, 0), (self.audio_sink_0, 0))
        self.connect((self.blocks_multiply_const_vxx_2, 0), (self.blocks_wavfile_sink_0, 0))
        self.connect((self.freq_xlating_fir_filter_xxx_0, 0), (self.mmse_resampler_xx_1, 0))
        self.connect((self.freq_xlating_fir_filter_xxx_1, 0), (self.mmse_resampler_xx_2, 0))
        self.connect((self.low_pass_filter_0, 0), (self.analog_wfm_rcv_1, 0))
        self.connect((self.low_pass_filter_1, 0), (self.analog_wfm_rcv_0, 0))
        self.connect((self.mmse_resampler_xx_0, 0), (self.blocks_freqshift_cc_0, 0))
        self.connect((self.mmse_resampler_xx_1, 0), (self.low_pass_filter_1, 0))
        self.connect((self.mmse_resampler_xx_2, 0), (self.low_pass_filter_0, 0))
        self.connect((self.soapy_rtlsdr_source_0, 0), (self.freq_xlating_fir_filter_xxx_0, 0))
        self.connect((self.soapy_rtlsdr_source_0, 0), (self.freq_xlating_fir_filter_xxx_1, 0))
        self.connect((self.soapy_rtlsdr_source_0, 0), (self.mmse_resampler_xx_0, 0))


    def get_PAL(self):
        return self.PAL

    def set_PAL(self, PAL):
        self.PAL = PAL
        self.set_system(self.PAL)

    def get_volume(self):
        return self.volume

    def set_volume(self, volume):
        self.volume = volume
        self.blocks_multiply_const_vxx_1.set_k(self.volume)
        self.blocks_multiply_const_vxx_2.set_k(self.volume)

    def get_system(self):
        return self.system

    def set_system(self, system):
        self.system = system
        self.blocks_freqshift_cc_0.set_phase_inc(2.0*math.pi*self.system/8000000)
        self.soapy_rtlsdr_source_0.set_frequency(0, self.system)

    def get_samp_rate(self):
        return self.samp_rate

    def set_samp_rate(self, samp_rate):
        self.samp_rate = samp_rate
        self.freq_xlating_fir_filter_xxx_0.set_taps(firdes.low_pass(1,self.samp_rate,2*int(self.samp_rate/256000), 200000))
        self.freq_xlating_fir_filter_xxx_1.set_taps(firdes.low_pass(1,self.samp_rate,2*self.samp_rate/256000, 200000))
        self.mmse_resampler_xx_0.set_resamp_ratio((self.samp_rate/8000000))
        self.soapy_rtlsdr_source_0.set_sample_rate(0, self.samp_rate)

    def get_lowpass_transition_width(self):
        return self.lowpass_transition_width

    def set_lowpass_transition_width(self, lowpass_transition_width):
        self.lowpass_transition_width = lowpass_transition_width
        self.low_pass_filter_0.set_taps(firdes.low_pass(self.lowpass_gain, self.baseband_sample_rate, self.lowpass_cutoff, self.lowpass_transition_width, window.WIN_HAMMING, 6.76))
        self.low_pass_filter_1.set_taps(firdes.low_pass(self.lowpass_gain, self.baseband_sample_rate, self.lowpass_cutoff, self.lowpass_transition_width, window.WIN_HAMMING, 6.76))

    def get_lowpass_gain(self):
        return self.lowpass_gain

    def set_lowpass_gain(self, lowpass_gain):
        self.lowpass_gain = lowpass_gain
        self.low_pass_filter_0.set_taps(firdes.low_pass(self.lowpass_gain, self.baseband_sample_rate, self.lowpass_cutoff, self.lowpass_transition_width, window.WIN_HAMMING, 6.76))
        self.low_pass_filter_1.set_taps(firdes.low_pass(self.lowpass_gain, self.baseband_sample_rate, self.lowpass_cutoff, self.lowpass_transition_width, window.WIN_HAMMING, 6.76))

    def get_lowpass_cutoff(self):
        return self.lowpass_cutoff

    def set_lowpass_cutoff(self, lowpass_cutoff):
        self.lowpass_cutoff = lowpass_cutoff
        self.low_pass_filter_0.set_taps(firdes.low_pass(self.lowpass_gain, self.baseband_sample_rate, self.lowpass_cutoff, self.lowpass_transition_width, window.WIN_HAMMING, 6.76))
        self.low_pass_filter_1.set_taps(firdes.low_pass(self.lowpass_gain, self.baseband_sample_rate, self.lowpass_cutoff, self.lowpass_transition_width, window.WIN_HAMMING, 6.76))

    def get_baseband_sample_rate(self):
        return self.baseband_sample_rate

    def set_baseband_sample_rate(self, baseband_sample_rate):
        self.baseband_sample_rate = baseband_sample_rate
        self.low_pass_filter_0.set_taps(firdes.low_pass(self.lowpass_gain, self.baseband_sample_rate, self.lowpass_cutoff, self.lowpass_transition_width, window.WIN_HAMMING, 6.76))
        self.low_pass_filter_1.set_taps(firdes.low_pass(self.lowpass_gain, self.baseband_sample_rate, self.lowpass_cutoff, self.lowpass_transition_width, window.WIN_HAMMING, 6.76))
        self.mmse_resampler_xx_1.set_resamp_ratio((256/int(self.baseband_sample_rate/1000)))
        self.mmse_resampler_xx_2.set_resamp_ratio((256/int(self.baseband_sample_rate/1000)))

    def get_audio_sample_rate(self):
        return self.audio_sample_rate

    def set_audio_sample_rate(self, audio_sample_rate):
        self.audio_sample_rate = audio_sample_rate

    def get_NTSC(self):
        return self.NTSC

    def set_NTSC(self, NTSC):
        self.NTSC = NTSC


def main(top_block_cls=rtlsdr_to_flac, options=None):
    parser = ArgumentParser()
    parser.add_argument('--output', default='E:\\Hifi\\hifi', help='Output base path (without extension)')
    parser.add_argument('--system', default='PAL', choices=['PAL', 'NTSC'], help='TV system')
    parser.add_argument('--gain', type=float, default=0, help='Tuner gain in dB, 0 = auto (default: 0)')
    args = parser.parse_args()

    tb = top_block_cls()

    tb.blocks_wavfile_sink_0.open(args.output + '.flac')
    tb.blocks_file_sink_0.open(args.output + '.u8')
    tb.set_system(tb.PAL if args.system == 'PAL' else tb.NTSC)

    # Apply gain: 0 = auto, otherwise manual
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
