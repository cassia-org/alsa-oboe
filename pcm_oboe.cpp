/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2024 Cassia Team (https://github.com/cassia-org)
 */

#include <alsa/asoundlib.h>
#include <alsa/pcm.h>
#include <alsa/pcm_external.h>
#include <alsa/pcm_ioplug.h>
#include <oboe/Oboe.h>

#include <initializer_list>
#include <memory>
#include <mutex>

/**
 * @brief An ALSA PCM I/O plugin that uses Oboe for playing audio on Android.
 * @note This currently only supports playback, capture is not supported.
 * @note The default backend is currently OpenSL as AAudio is broken on some devices.
 */
class OboePcm {
  private:
    std::mutex mutex;
    std::shared_ptr<oboe::AudioStream> stream;
    constexpr static int64_t TimeoutNanoseconds{36000000000}; //!< An hour in nanoseconds, this is an arbitrarily long timeout that should never be reached.

    static int Start(snd_pcm_ioplug_t* ext) {
        auto* self{static_cast<OboePcm*>(ext->private_data)};
        std::scoped_lock lock{self->mutex};
        if (!self->stream)
            return -EBADFD; // This should be checked by pcm_ioplug but we'll do it here too.

        oboe::Result result{self->stream->requestStart()};
        if (result != oboe::Result::OK) {
            std::cerr << "[ALSA Oboe] Failed to start stream: " << oboe::convertToText(result) << std::endl;
            return -1;
        }

        return 0;
    }

    static int Stop(snd_pcm_ioplug_t* ext) {
        auto* self{static_cast<OboePcm*>(ext->private_data)};
        std::scoped_lock lock{self->mutex};
        if (!self->stream)
            return -EBADFD;

        oboe::StreamState state{self->stream->getState()};
        if (state == oboe::StreamState::Stopped || state == oboe::StreamState::Flushed)
            return 0; // We don't need to do anything if the stream is already stopped.

        oboe::Result result{self->stream->requestPause()};
        if (result != oboe::Result::OK) {
            std::cerr << "[ALSA Oboe] Failed to pause stream: " << oboe::convertToText(result) << std::endl;
            return -1;
        }

        state = self->stream->getState();
        while (state != oboe::StreamState::Paused) {
            // AAudio documentation states that requestFlush() is valid while the stream is Pausing.
            // However, in practice it returns InvalidState, so we'll just wait for the stream to pause.
            result = self->stream->waitForStateChange(state, &state, TimeoutNanoseconds);
            if (result != oboe::Result::OK) {
                std::cerr << "[ALSA Oboe] Failed to wait for pause: " << oboe::convertToText(result) << std::endl;
                return -1;
            }
        }

        result = self->stream->requestFlush();
        if (result != oboe::Result::OK) {
            std::cerr << "[ALSA Oboe] Failed to flush stream: " << oboe::convertToText(result) << std::endl;
            return -1;
        }

        state = self->stream->getState();
        while (state != oboe::StreamState::Flushed) {
            result = self->stream->waitForStateChange(oboe::StreamState::Flushing, &state, TimeoutNanoseconds);
            if (result != oboe::Result::OK) {
                std::cerr << "[ALSA Oboe] Failed to wait for flush: " << oboe::convertToText(result) << std::endl;
                return -1;
            }
        }

        return 0;
    }

    static snd_pcm_sframes_t Pointer(snd_pcm_ioplug_t* ext) {
        auto* self{static_cast<OboePcm*>(ext->private_data)};
        std::scoped_lock lock{self->mutex};
        if (!self->stream)
            return -EBADFD;

        // Note: This function would return an error for any Xruns but we don't bother as Oboe automatically recovers from them.

        int64_t framesWritten{self->stream->getFramesWritten()};
        if (framesWritten < 0) {
            std::cerr << "[ALSA Oboe] Failed to get frames written: " << framesWritten << std::endl;
            return -1;
        }

        // We don't care about the device ring buffer position as Oboe handles writing samples to it.
        // Instead, we just need to return the current position relative to the imaginary ALSA buffer size.
        return framesWritten % ext->buffer_size;
    }

    static snd_pcm_sframes_t Transfer(snd_pcm_ioplug_t* ext, const snd_pcm_channel_area_t* areas, snd_pcm_uframes_t offset, snd_pcm_uframes_t size) {
        auto* self{static_cast<OboePcm*>(ext->private_data)};
        std::unique_lock lock{self->mutex};
        if (!self->stream)
            return -EBADFD;
        if (size == 0)
            return 0;

        if (self->stream->getState() != oboe::StreamState::Started) {
            // ALSA expects us to automatically start the stream if it's not started.
            oboe::Result result{self->stream->requestStart()};
            if (result != oboe::Result::OK) {
                std::cerr << "[ALSA Oboe] Failed to start stream from transfer: " << oboe::convertToText(result) << std::endl;
                return -1;
            }
        }

        auto& firstArea{areas[0]};
        auto* address{reinterpret_cast<uint8_t*>(firstArea.addr)};

#ifndef NDEBUG
        uint channelOffset{0};
        for (unsigned int c{0}; c < ext->channels; ++c) {
            auto& area{areas[c]};
            if (area.addr != firstArea.addr || area.step != firstArea.step || area.first >= firstArea.step) {
                std::cerr << "[ALSA Oboe] Attempt to transfer non-interleaved samples" << std::endl;
                return -1;
            }
        }
#endif

        oboe::ResultWithValue<int32_t> result{self->stream->write(address, size, ext->nonblock ? 0 : TimeoutNanoseconds)};
        if (result != oboe::Result::OK) {
            std::cerr << "[ALSA Oboe] Failed to write samples to stream: " << oboe::convertToText(result.error()) << std::endl;
            return -1;
        } else if (result.value() == 0) {
            if (!ext->nonblock)
                std::cerr << "[ALSA Oboe] Cannot write samples in blocking mode" << std::endl;
            return -EAGAIN; // Oboe will return 0 if the stream is non-blocking and there's no space in the buffer.
        }

        return result.value();
    }

    static int Close(snd_pcm_ioplug_t* ext) {
        if (ext->private_data) {
            ext->private_data = nullptr;
            auto* self{static_cast<OboePcm*>(ext->private_data)};
            delete self;
        }
        return 0;
    }

    static int Prepare(snd_pcm_ioplug_t* ext) {
        auto* self{static_cast<OboePcm*>(ext->private_data)};
        std::scoped_lock lock{self->mutex};
        if (self->stream)
            return 0;

        oboe::AudioStreamBuilder builder;
        builder.setUsage(oboe::Usage::Game)
            ->setDirection(oboe::Direction::Output)
            // Note: There is some instability related to using LowLatency mode on certain devices.
            // Notably, while running mono 16-bit 48kHz audio on certain QCOM devices, the HAL simply raises a SIGABRT with no logs.
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)
            ->setFormat([fmt = ext->format]() {
                switch (fmt) {
                    case SND_PCM_FORMAT_S16_LE:
                        return oboe::AudioFormat::I16;
                    case SND_PCM_FORMAT_FLOAT_LE:
                        return oboe::AudioFormat::Float;
                    case SND_PCM_FORMAT_S24_3LE:
                        return oboe::AudioFormat::I24;
                    case SND_PCM_FORMAT_S32_LE:
                        return oboe::AudioFormat::I32;
                    default:
                        return oboe::AudioFormat::Invalid;
                }
            }())
            ->setFormatConversionAllowed(true)
            ->setChannelCount(ext->channels)
            ->setChannelConversionAllowed(true)
            ->setSampleRate(ext->rate)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setBufferCapacityInFrames(ext->buffer_size)
            ->setAudioApi(oboe::AudioApi::OpenSLES);

        oboe::Result result{builder.openStream(self->stream)};
        if (result != oboe::Result::OK) {
            std::cerr << "[ALSA Oboe] Failed to open stream: " << oboe::convertToText(result) << std::endl;
            return -1;
        }

        if (self->stream->getBufferCapacityInFrames() < ext->buffer_size) {
            // Note: This should never happen with AAudio, but it's possible with OpenSL ES.
            std::cerr << "[ALSA Oboe] Buffer size smaller than requested: " << self->stream->getBufferCapacityInFrames() << " < " << ext->buffer_size << std::endl;
            self->stream.reset();
            return -EIO;
        }

        return 0;
    }

    static int Drain(snd_pcm_ioplug_t* ext) {
        auto self{static_cast<OboePcm*>(ext->private_data)};
        std::scoped_lock lock{self->mutex};
        if (!self->stream)
            return -EBADFD;

        // We need to wait for the stream to read all samples during a drain.
        // According to AAudio documentation, requestStop() guarantees that the stream's contents have been written to the device.
        // However, in practice it doesn't seem to be the case, so we'll just poll the frames read until it reaches the frames written.

        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        start.tv_sec += 1;

        while (true) {
            int64_t framesRead{self->stream->getFramesRead()};
            if (framesRead < 0) {
                std::cerr << "[ALSA Oboe] Failed to get frames read: " << framesRead << std::endl;
                return -1;
            }

            int64_t framesWritten{self->stream->getFramesWritten()};
            if (framesWritten < 0) {
                std::cerr << "[ALSA Oboe] Failed to get frames written: " << framesWritten << std::endl;
                return -1;
            }

            if (framesRead == framesWritten)
                break;

            usleep(1000);

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((now.tv_sec > start.tv_sec || (now.tv_sec == start.tv_sec && now.tv_nsec > start.tv_nsec)) && framesRead == 0) {
                // AAudio has a bug where it won't read any samples until an arbitrary minimum amount of samples have been written.
                // We just wait for a second and if no samples have been read, we'll assume that AAudio is broken.
                break;
            }
        }

        oboe::Result result{self->stream->requestStop()};
        if (result != oboe::Result::OK) {
            std::cerr << "[ALSA Oboe] Failed to stop stream: " << oboe::convertToText(result) << std::endl;
            return -1;
        }

        oboe::StreamState state{self->stream->getState()};
        while (state != oboe::StreamState::Stopped) {
            result = self->stream->waitForStateChange(state, &state, TimeoutNanoseconds);
            if (result != oboe::Result::OK) {
                std::cerr << "[ALSA Oboe] Failed to wait for stop: " << oboe::convertToText(result) << std::endl;
                return -1;
            }
        }

        return 0;
    }

    static int Pause(snd_pcm_ioplug_t* ext, int enable) {
        auto self{static_cast<OboePcm*>(ext->private_data)};
        std::scoped_lock lock{self->mutex};
        if (!self->stream)
            return -EBADFD;

        oboe::Result result{self->stream->requestPause()};
        if (result != oboe::Result::OK) {
            std::cerr << "[ALSA Oboe] Failed to pause stream: " << oboe::convertToText(result) << std::endl;
            return -1;
        }

        return 0;
    }

    constexpr static snd_pcm_ioplug_callback_t Callbacks{
        .start = &Start,
        .stop = &Stop,
        .pointer = &Pointer,
        .transfer = &Transfer,
        .close = &Close,
        .prepare = &Prepare,
        .drain = &Drain,
        .pause = &Pause,
        .resume = &Start,
    };

  public:
    snd_pcm_ioplug_t plug{
        .version = SND_PCM_IOPLUG_VERSION,
        .name = "ALSA <-> Oboe PCM I/O Plugin",
        .mmap_rw = false,
        .callback = &Callbacks,
        .private_data = this,
    };

    OboePcm() = default;

    int Initialize(const char* name, snd_pcm_stream_t stream, int mode) {
        if (stream != SND_PCM_STREAM_PLAYBACK)
            return -EINVAL; // We only support playback for now.

        int err{snd_pcm_ioplug_create(&plug, name, stream, mode)};
        if (err < 0)
            return err;

        auto setParamList{[io = &plug](int type, std::initializer_list<unsigned int> list) {
            return snd_pcm_ioplug_set_param_list(io, type, list.size(), list.begin());
        }};

        err = setParamList(SND_PCM_IOPLUG_HW_ACCESS, {SND_PCM_ACCESS_RW_INTERLEAVED});
        if (err < 0)
            return err;

        err = setParamList(SND_PCM_IOPLUG_HW_FORMAT, {
                                                         SND_PCM_FORMAT_S16_LE,
                                                         SND_PCM_FORMAT_FLOAT_LE,
                                                         SND_PCM_FORMAT_S24_3LE,
                                                         SND_PCM_FORMAT_S32_LE,
                                                     });
        if (err < 0)
            return err;

        // We could support more than 2 channels, but it's fairly complicated due to channel mappings.
        err = snd_pcm_ioplug_set_param_minmax(&plug, SND_PCM_IOPLUG_HW_CHANNELS, 1, 2);
        if (err < 0)
            return err;

        // Oboe supports any sample rate with sample rate conversion, but we'll limit it to a reasonable range (8kHz - 192kHz).
        err = snd_pcm_ioplug_set_param_minmax(&plug, SND_PCM_IOPLUG_HW_RATE, 8000, 192000);
        if (err < 0)
            return err;

        // Oboe will decide the period/buffer size internally after starting the stream and it's not a detail that we can expose properly.
        // We set arbitrary values that should be reasonable for most use cases.
        err = snd_pcm_ioplug_set_param_minmax(&plug, SND_PCM_IOPLUG_HW_PERIODS, 2, 4);
        if (err < 0)
            return err;
        err = snd_pcm_ioplug_set_param_minmax(&plug, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 32 * 1024, 64 * 1024);
        if (err < 0)
            return err;

        return 0;
    }

    ~OboePcm() {
        std::scoped_lock lock{mutex};
        stream.reset();
    }
};

extern "C" {
SND_PCM_PLUGIN_DEFINE_FUNC(oboe) {
    // Note: We don't need to do anything with the config, so we can just ignore it.

    OboePcm* plugin{new (std::nothrow) OboePcm{}};
    if (!plugin)
        return -ENOMEM;

    int err{plugin->Initialize(name, stream, mode)};
    if (err < 0) {
        delete plugin;
        return err;
    }

    *pcmp = plugin->plug.pcm;
    return 0;
}

SND_PCM_PLUGIN_SYMBOL(oboe);
}