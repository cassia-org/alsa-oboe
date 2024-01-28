### ALSA <-> Oboe I/O PCM Plugin

#### Description

ALSA's userspace library ([`alsa-lib`](https://github.com/alsa-project/alsa-lib/)) has a plugin system that allows for userspace software devices rather than a device backed by a kernel driver. This is already utilized for various plugins at the [ALSA plugins repository](https://github.com/alsa-project/alsa-plugins/), including the [JACK](https://github.com/alsa-project/alsa-plugins/tree/master/jack) and [PulseAudio](https://github.com/alsa-project/alsa-plugins/tree/master/pulse) plugin which translate ALSA calls into their corresponding API, similar to what this plugin does.

This plugin allows using an unmodified version of `alsa-lib` to call into [Oboe](https://github.com/google/oboe), a C++ library for high-performance audio on Android, allowing for any applications that use ALSA (such as Wine and SDL-based applications, which are the primary targets) to have functional audio on Android without any other changes.

#### Configuration ([`.asoundrc`](https://www.alsa-project.org/wiki/Asoundrc))

* **Basic**: This will only support anything directly exposed by the plugin, that being mono/stereo `S16`/`S24_3`/`S32`/`FLOAT` LE @ 8kHz-48kHz audio.
```
pcm.!default {
    type oboe
    hint {
        show {
            @func refer
            name defaults.namehint.basic
        }
        description "Oboe PCM"
    }
}
```
* **Advanced**: Adding `plug` in front of the plugin allows for any unsupported format, channel or rate to be automatically converted into a supported equivalent.
```
pcm.!default {
    type plug
    slave {
        pcm {
            type mmap_emul
            slave.pcm {
                type oboe
            }
        }
        format S16_LE
        rate unchanged
        channels unchanged
    }
    hint {
        show {
            @func refer
            name defaults.namehint.basic
        }
        description "Oboe PCM"
    }
}
```