# Audioreactive usermod

Enables controlling LEDs via audio input. Audio source can be a microphone or analog-in (AUX) using an appropriate adapter.
Supported microphones range from analog (MAX4466, MAX9814, ...) to digital (INMP441, ICS-43434, ...).

Does audio processing and provides data structure that specially written effects can use.

**does not** provide effects or draw anything to an LED strip/matrix.

## Additional Documentation
This usermod is an evolution of [SR-WLED](https://github.com/atuline/WLED), and a lot of documentation and information can be found in the [SR-WLED wiki](https://github.com/atuline/WLED/wiki):
* [getting started with audio](https://github.com/atuline/WLED/wiki/First-Time-Setup#sound)
* [Sound settings](https://github.com/atuline/WLED/wiki/Sound-Settings) - similar to options on the usemod settings page in WLED.
* [Digital Audio](https://github.com/atuline/WLED/wiki/Digital-Microphone-Hookup)
* [Analog Audio](https://github.com/atuline/WLED/wiki/Analog-Audio-Input-Options)
* [UDP Sound sync](https://github.com/atuline/WLED/wiki/UDP-Sound-Sync)


## Supported MCUs

This has mostly been rewritten to suppot WLED-MM-P4 by @TroyHacks - so some things may be dropped or different. This code assumes you know what you're doing. 

This audioreactive usermod works best on "classic ESP32" (dual core), and on ESP32-S3 which also has dual core and hardware floating point support and accellerated FFT, and of course the ESP32-P4 which the latest speed deamon and also has accellerated FFT (so do some other chipsets this could run on, but aren't entirely listed yet.)

It might compile successfully for ESP32-S2 and ESP32-C3, however might not work well, as other WLED functions will become slow. Audio processing requires a lot of computing power, which can be problematic on smaller MCUs like -S2 and -C3. **Not tested below the ESP32-P4.**

Analog audio is only possible on "classic" ESP32, but not on other MCUs like ESP32-S3.

ESP8266 is not supported, don't bother trying to compile this version for that. 

## Installation 

### using Espressif's Accelerated ESP-DSP library

* `build_flags` = `-D USERMOD_AUDIOREACTIVE`

## Configuration

All parameters are runtime configurable. Some may require a hard reset after changing them (I2S microphone or selected GPIOs).

If you want to define default GPIOs during compile time, use the following (default values in parentheses):

- `-D SR_DMTYPE=x` : defines digital microphone type: 0=analog, 1=generic I2S (default), 2=ES7243 I2S, 3=SPH0645 I2S, 4=generic I2S with master clock, 5=PDM I2S, 6=ES8388, 7=WM8978, 8=AC101, 9=ES8311, 10=ES8311-IDF, 11=ES8388-IDF, 12=ES8374-IDF, 13=ZL38063-IDF, 14=ES8389-IDF, 15=ES7210-IDF (rec), 16=ES7243-IDF (rec), 17=ES7243E-IDF (rec), 18=CJC8910-IDF
- `-D AUDIOPIN=x`  : GPIO for analog microphone/AUX-in (36)
- `-D I2S_SDPIN=x` : GPIO for SD pin on digital microphone (32)
- `-D I2S_WSPIN=x` : GPIO for WS pin on digital microphone (15)
- `-D I2S_CKPIN=x` : GPIO for SCK pin on digital microphone (14)
- `-D MCLK_PIN=x`  : GPIO for master clock pin on digital Line-In boards (-1)
- `-D ES7243_SDAPIN` : GPIO for I2C SDA pin on ES7243 microphone (-1)
- `-D ES7243_SCLPIN` : GPIO for I2C SCL pin on ES7243 microphone (-1)

Other options:

- `-D UM_AUDIOREACTIVE_ENABLE` : makes usermod default enabled (not the same as include into build option!)
- `-D UM_AUDIOREACTIVE_DYNAMICS_LIMITER_OFF` : disables rise/fall limiter default

### GEQ Level / AGC / Compression Toggles

The `experiments` block in the AudioReactive settings has four runtime
toggles that adjust how the GEQ (graphic equalizer) bars are scaled.
All default OFF except `AutoLevel` (default ON). The toggles are:

- **`AutoLevel`** (default ON): per-band slow-decay AGC. Each band
  tracks its own peak with a slow-decay envelope and applies a per-band
  gain so the loudest band fills the display. Pink-noise (1/f) sources
  end up roughly flat in the display because each band settles
  independently. Combines well with `BandCompress` for a smooth, responsive
  visualizer.

- **`BandCompress`** (default OFF): per-band log compression. Applies
  `log(1+x) / log(1+bandMax)` so peaks are softened without boosting
  the floor. Each band compresses against its own current peak —
  bands keep their shape but transients don't blow out the display.
  Use with `AutoLevel` for the most "alive" look; without AGC the
  compression is barely visible.

- **`NoiseFloorSub`** (default OFF): per-band slow-minimum subtraction.
  Tracks the noise floor of each band (slowest minimum) and subtracts it,
  so quiet bands with high residual noise show their actual signal
  instead of always appearing at the noise level. Most useful with
  distant or noisy mics.

- **`HistogramNorm`** (default OFF): single global gain via loudest-band
  peak. Periodically picks a gain that makes the loudest band hit ~85%
  of the display. Mutually exclusive with `AutoLevel` (skipped when AGC
  is on, since both would double-correct). A simpler alternative to
  per-band AGC if you just want a single global gain.

- **`Band0Lifter`** (default OFF): "dance music fixer" — when band 1 is
  non-zero, add 15% of band 1's value to band 0. Compensates for mics
  with poor sub-bass response (e.g. the ESP32-P4 EV board's onboard
  analog MEMS mic) by using band 1 (60-120 Hz, reliably captured) as a
  weak hint for band 0. Bands stay visually distinct: the additive
  coupling is small enough that band 0 retains its own shape, just
  shifted up when band 1 is active.

### Recommended Combinations

Different content types benefit from different combinations:

- **Music playback (default AGC)**: just `AutoLevel`. Music is already
  mastered with proper levels; AGC rebalances bands to fill the display
  without changing the relative mix.

- **Live microphone / speech**: `AutoLevel` + `NoiseFloorSub` +
  `BandCompress`. The mic's noise floor is subtracted, AGC levels the
  bands, and compression prevents a single loud syllable from blowing
  out a bar.

- **Quiet environment / distant mic**: `NoiseFloorSub` only (no AGC).
  AGC would amplify the noise floor along with the signal; the natural
  variation between bands is informative.

- **Loud environment / clipping**: `AutoLevel` only. Don't subtract
  the floor (that would remove real signal) and don't compress
  (transients should still feel alive).

- **Visualizer for a screen / demo**: `AutoLevel` + `BandCompress`.
  Aggressive AGC + compression makes every band show activity — looks
  "alive" all the time.

- **Clean / smooth**: `AutoLevel` only. Single control, smooth display.

- **Raw / debugging**: all OFF. Bands show actual values; tune
  `pinkIndex` (0-10) and other parameters knowing you're seeing the
  true signal.

**Don't combine `AutoLevel` + `HistogramNorm`** — they're both gain
adjustments; the code already skips `HistogramNorm` when `AutoLevel` is
on to avoid double-correction.

**Don't combine `BandCompress` alone without AGC** — without AGC the
input range is so wide that compression is barely visible.

**Set-and-forget for most users**: `AutoLevel` (default) +
`NoiseFloorSub` + `BandCompress` handles nearly all content types
gracefully with no user intervention.

**NOTE** I2S is used for analog audio sampling. Hence, the analog *buttons* (i.e. potentiometers) are disabled when running this usermod with an analog microphone.

### Advanced Compile-Time Options
You can use the following additional flags in your `build_flags`
* `-D SR_SQUELCH=x`  : Default "squelch" setting (10)
* `-D SR_GAIN=x`     : Default "gain" setting (60)
* `-D I2S_USE_RIGHT_CHANNEL`: Use RIGHT instead of LEFT channel (not recommended unless you strictly need this).
* `-D I2S_USE_16BIT_SAMPLES`: Use 16bit instead of 32bit for internal sample buffers. Reduces sampling quality, but frees some RAM ressources (not recommended unless you absolutely need this).
* `-D I2S_GRAB_ADC1_COMPLETELY`: Experimental: continuously sample analog ADC microphone. Only effective on ESP32. WARNING this _will_ cause conflicts(lock-up) with any analogRead() call.
* `-D MIC_LOGGER`     : (debugging) Logs samples from the microphone to serial USB. Use with serial plotter (Arduino IDE)
* `-D SR_DEBUG`       : (debugging) Additional error diagnostics and debug info on serial USB.

## Release notes

* 2026-06 Moved to pure Espressif IDF v5.5 calls and ESP-DSP FFT accelleration, added esp_codec_dev supported codecs and added AutoLevel experiments - by @TroyHacks
* 2022-06 Ported from [soundreactive WLED](https://github.com/atuline/WLED) - by @blazoncek (AKA Blaz Kristan) and the [SR-WLED team](https://github.com/atuline/WLED/wiki#sound-reactive-wled-fork-team).
* 2022-11 Updated to align with "[MoonModules/WLED](https://amg.wled.me)" audioreactive usermod - by @softhack007 (AKA Frank M&ouml;hle).
