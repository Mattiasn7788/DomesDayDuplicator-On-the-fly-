/************************************************************************

    AudioResampler.cpp

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

#include "AudioResampler.h"
#include <QDebug>
#include <algorithm>
#include <cstring>

AudioResampler::AudioResampler()
    : swrContext(nullptr)
    , inputSampleRate(0)
    , outputSampleRate(0)
    , initialized(false)
{
}

AudioResampler::~AudioResampler()
{
    cleanup();
}

bool AudioResampler::initialize(uint32_t inputSampleRate, uint32_t outputSampleRate)
{
    // Clean up any existing context first
    cleanup();
    
    this->inputSampleRate = inputSampleRate;
    this->outputSampleRate = outputSampleRate;
    
    // If input and output rates are the same, no resampling needed
    if (inputSampleRate == outputSampleRate) {
        initialized = true;
        return true;
    }
    
    // Try FFmpeg 5.0+ API first (swr_alloc_set_opts2), fallback to older API
#if LIBSWRESAMPLE_VERSION_INT >= AV_VERSION_INT(4, 5, 100)  // FFmpeg 5.0+
    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_MONO;
    AVChannelLayout in_ch_layout = AV_CHANNEL_LAYOUT_MONO;
    
    int ret = swr_alloc_set_opts2(&swrContext,
        &out_ch_layout,               // output channel layout
        AV_SAMPLE_FMT_S16,            // output sample format
        outputSampleRate,             // output sample rate
        &in_ch_layout,                // input channel layout
        AV_SAMPLE_FMT_S16,            // input sample format
        inputSampleRate,              // input sample rate
        0,                            // log offset
        nullptr                       // log context
    );
    
    if (ret < 0) {
        qDebug() << "AudioResampler::initialize(): Failed to allocate resampling context, error:" << ret;
        return false;
    }
#else
    // Use older FFmpeg 4.x compatible API
    swrContext = swr_alloc_set_opts(
        nullptr,                       // existing context (nullptr = allocate new)
        AV_CH_LAYOUT_MONO,            // output channel layout (int64_t)
        AV_SAMPLE_FMT_S16,            // output sample format
        outputSampleRate,             // output sample rate
        AV_CH_LAYOUT_MONO,            // input channel layout (int64_t)
        AV_SAMPLE_FMT_S16,            // input sample format
        inputSampleRate,              // input sample rate
        0,                            // log offset
        nullptr                       // log context
    );
    
    if (!swrContext) {
        qDebug() << "AudioResampler::initialize(): Failed to allocate resampling context";
        return false;
    }
#endif
    
    
    qDebug() << "AudioResampler: Configured" << inputSampleRate << "->" << outputSampleRate;
    
    // Initialize the resampling context
    if (swr_init(swrContext) < 0) {
        qDebug() << "AudioResampler::initialize(): Failed to initialize resampling context";
        swr_free(&swrContext);
        swrContext = nullptr;
        return false;
    }
    
    initialized = true;
    qDebug() << "AudioResampler::initialize(): Successfully initialized resampler" 
             << inputSampleRate << "Hz ->" << outputSampleRate << "Hz";
    
    return true;
}

void AudioResampler::cleanup()
{
    if (swrContext) {
        swr_free(&swrContext);
        swrContext = nullptr;
    }
    initialized = false;
}

int AudioResampler::resample(const int16_t* inputData, int inputSampleCount, int16_t* outputData, int outputBufferSampleCount)
{
    if (!initialized) {
        return -1;
    }
    
    // If no resampling needed (same input/output rates), just copy data
    if (inputSampleRate == outputSampleRate) {
        int samplesToCopy = std::min(inputSampleCount, outputBufferSampleCount);
        memcpy(outputData, inputData, samplesToCopy * sizeof(int16_t));
        return samplesToCopy;
    }
    
    if (!swrContext) {
        return -1;
    }
    
    // Perform the resampling
    int outputSamples = swr_convert(swrContext, 
                                   (uint8_t**)&outputData, outputBufferSampleCount,
                                   (const uint8_t**)&inputData, inputSampleCount);
    
    if (outputSamples < 0) {
        qDebug() << "AudioResampler::resample(): Error during resampling:" << outputSamples;
        return -1;
    }
    
    return outputSamples;
}

int AudioResampler::getExpectedOutputSampleCount(int inputSampleCount) const
{
    if (!initialized || inputSampleRate == outputSampleRate) {
        return inputSampleCount;
    }
    
    // Calculate expected output sample count based on ratio
    return (int64_t)inputSampleCount * outputSampleRate / inputSampleRate;
}
