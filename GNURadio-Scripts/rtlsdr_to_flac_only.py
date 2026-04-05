#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# SPDX-License-Identifier: GPL-3.0
#
# RTL-SDR VHS HiFi capture - FLAC output only
# Based on original script by Adam

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
import os
from argparse import ArgumentParser
from gnuradio import soapy


class rtlsdr_to_flac_only(gr.top_block):

    def __init__(self):
        gr.top_block.__init__(self, "RTL-SDR to FLAC only", catch_exceptions=True)

        self.PAL = PAL = 1600000
        self.NTSC = NTSC = 1500000
        self.volume = volume = .7
        self.system = system = PAL
        self.samp_rate = samp_rate = 1024000
        self.lowpass_transition_width = lowpass_transition_width = 5000
        self.lowpass_gain = lowpass_gain = 30
        self.lowpass_cutoff = lowpass_cutoff = 100000
        self.baseband_sample_rate = baseband_sample_rate = 384000
        self.audio_sample_rate = audio_sample_rate = 48000

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

        # Stereo channel filters and demodulators
        self.freq_xlating_fir_filter_xxx_0 = filter.freq_xlating_fir_filter_ccf(
            (int(samp_rate/256000)),
            firdes.low_pass(1, samp_rate, 2*int(samp_rate/256000), 200000),
            (-200000), samp_rate)
        self.freq_xlating_fir_filter_xxx_1 = filter.freq_xlating_fir_filter_ccf(
            (int(samp_rate/256000)),
            firdes.low_pass(1, samp_rate, 2*samp_rate/256000, 200000),
            200000, samp_rate)
        self.mmse_resampler_xx_1 = filter.mmse_resampler_cc(0, (256/int(baseband_sample_rate/1000)))
        self.mmse_resampler_xx_2 = filter.mmse_resampler_cc(0, (256/int(baseband_sample_rate/1000)))
        self.low_pass_filter_0 = filter.fir_filter_ccf(1, firdes.low_pass(
            lowpass_gain, baseband_sample_rate, lowpass_cutoff,
            lowpass_transition_width, window.WIN_HAMMING, 6.76))
        self.low_pass_filter_1 = filter.fir_filter_ccf(1, firdes.low_pass(
            lowpass_gain, baseband_sample_rate, lowpass_cutoff,
            lowpass_transition_width, window.WIN_HAMMING, 6.76))
        self.analog_wfm_rcv_0 = analog.wfm_rcv(
            quad_rate=baseband_sample_rate,
            audio_decimation=(int(baseband_sample_rate/audio_sample_rate)))
        self.analog_wfm_rcv_1 = analog.wfm_rcv(
            quad_rate=baseband_sample_rate,
            audio_decimation=(int(baseband_sample_rate/audio_sample_rate)))
        self.blocks_multiply_const_vxx_1 = blocks.multiply_const_ff(volume)
        self.blocks_multiply_const_vxx_2 = blocks.multiply_const_ff(volume)

        # FLAC output (file opened later via open() call)
        _init_flac = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gnuradio_hifi_init.flac').replace('\\', '/')
        self.blocks_wavfile_sink_0 = blocks.wavfile_sink(
            _init_flac, 2, audio_sample_rate,
            blocks.FORMAT_FLAC, blocks.FORMAT_PCM_24, False)

        # Audio monitor
        self.audio_sink_0 = audio.sink(48000, '', True)

        # Connections
        self.connect((self.soapy_rtlsdr_source_0, 0), (self.freq_xlating_fir_filter_xxx_0, 0))
        self.connect((self.soapy_rtlsdr_source_0, 0), (self.freq_xlating_fir_filter_xxx_1, 0))
        self.connect((self.freq_xlating_fir_filter_xxx_0, 0), (self.mmse_resampler_xx_1, 0))
        self.connect((self.freq_xlating_fir_filter_xxx_1, 0), (self.mmse_resampler_xx_2, 0))
        self.connect((self.mmse_resampler_xx_1, 0), (self.low_pass_filter_1, 0))
        self.connect((self.mmse_resampler_xx_2, 0), (self.low_pass_filter_0, 0))
        self.connect((self.low_pass_filter_1, 0), (self.analog_wfm_rcv_0, 0))
        self.connect((self.low_pass_filter_0, 0), (self.analog_wfm_rcv_1, 0))
        self.connect((self.analog_wfm_rcv_0, 0), (self.blocks_multiply_const_vxx_2, 0))
        self.connect((self.analog_wfm_rcv_1, 0), (self.blocks_multiply_const_vxx_1, 0))
        self.connect((self.blocks_multiply_const_vxx_2, 0), (self.blocks_wavfile_sink_0, 0))
        self.connect((self.blocks_multiply_const_vxx_1, 0), (self.blocks_wavfile_sink_0, 1))
        self.connect((self.blocks_multiply_const_vxx_2, 0), (self.audio_sink_0, 0))
        self.connect((self.blocks_multiply_const_vxx_1, 0), (self.audio_sink_0, 1))

    def set_system(self, system):
        self.system = system
        self.soapy_rtlsdr_source_0.set_frequency(0, self.system)


def main(top_block_cls=rtlsdr_to_flac_only, options=None):
    parser = ArgumentParser()
    parser.add_argument('--output', default='E:\\Hifi\\hifi', help='Output base path (without extension)')
    parser.add_argument('--system', default='PAL', choices=['PAL', 'NTSC'], help='TV system')
    parser.add_argument('--gain', type=float, default=0, help='Tuner gain in dB, 0 = auto')
    args = parser.parse_args()

    tb = top_block_cls()

    tb.blocks_wavfile_sink_0.open(args.output + '.flac')
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
