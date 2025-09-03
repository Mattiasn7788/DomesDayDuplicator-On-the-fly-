/************************************************************************

    AudioResampler.h

    Audio resampling utility for the Domesday Duplicator
    DomesdayDuplicator - LaserDisc RF sampler
    Copyright (C) 2018-2019 Simon Inns
    Copyright (C) 2024 Harry (resampling implementation)

    This file is part of Domesday Duplicator.

    Domesday Duplicator is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#pragma once

#include <memory>
#include <cstdint>
#include <vector>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

class AudioResampler
{
public:
    AudioResampler();
    ~AudioResampler();

    // Initialize resampler with input and output sample rates
    bool initialize(uint32_t inputSampleRate, uint32_t outputSampleRate);
    
    // Clean up resources
    void cleanup();
    
    // Resample audio data - input is 16-bit signed samples, output is 16-bit signed samples
    // Returns the number of output samples produced, or -1 on error
    int resample(const int16_t* inputData, int inputSampleCount, int16_t* outputData, int outputBufferSampleCount);
    
    // Get the expected output sample count for a given input sample count
    int getExpectedOutputSampleCount(int inputSampleCount) const;
    
    // Check if resampler is initialized and ready
    bool isInitialized() const { return initialized; }
    
    // Get the configured output sample rate
    uint32_t getOutputSampleRate() const { return outputSampleRate; }

private:
    struct SwrContext* swrContext;
    uint32_t inputSampleRate;
    uint32_t outputSampleRate;
    bool initialized;
    
    // Disable copy constructor and assignment
    AudioResampler(const AudioResampler&) = delete;
    AudioResampler& operator=(const AudioResampler&) = delete;
};
