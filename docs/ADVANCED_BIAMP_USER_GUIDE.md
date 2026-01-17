# Advanced Bi-Amp Crossover User Guide

This guide covers all configuration options available in the Advanced Bi-Amp mode for the TAS5805M DAC. This mode enables active crossover functionality, allowing a single stereo amplifier to drive a 2-way speaker with separate woofer and tweeter outputs.

## Overview

In Advanced Bi-Amp mode:
- **Left channel** → Low-pass filtered → **Woofer**
- **Right channel** → High-pass filtered → **Tweeter**

The system provides 15 biquad filter stages per channel, allocated as follows:

| Band | Woofer (Left) | Tweeter (Right) |
|------|---------------|-----------------|
| 0 | Gain/Phase | Gain/Phase |
| 1 | Subsonic HPF | Time Alignment |
| 2-5 | Lowpass Crossover | Highpass Crossover |
| 6-11 | PEQ (6 bands) | PEQ (6 bands) |
| 12 | Baffle Step | Breakup Notch |
| 13 | Bass Loudness | Air/Brilliance |
| 14 | (Spare) | Treble Loudness |

---

## Crossover Settings

### Frequency
- **Range:** 20 - 20,000 Hz
- **Default:** 2000 Hz
- **Purpose:** Sets the crossover point where the woofer and tweeter frequency ranges meet.

**Guidelines:**
- Check your speaker/driver specifications for recommended crossover frequency
- Typical 2-way speakers: 1500-3500 Hz
- Small woofers (3-4"): 3000-4000 Hz
- Larger woofers (5-6.5"): 1800-2500 Hz
- Always cross over above the tweeter's resonance frequency (Fs)

### Slope
- **Options:** 12 dB/oct, 24 dB/oct (LR), 48 dB/oct
- **Default:** 24 dB/oct

| Slope | Biquads Used | Characteristics |
|-------|--------------|-----------------|
| 12 dB/oct | 1 | Gentle rolloff, wide overlap between drivers |
| 24 dB/oct | 2 | Standard Linkwitz-Riley, flat summed response |
| 48 dB/oct | 4 | Steep rolloff, minimal driver overlap |

**Recommendation:** 24 dB/oct (Linkwitz-Riley) is the most commonly used and provides excellent phase coherence at the crossover point.

### Type
- **Options:** Butterworth, Linkwitz-Riley
- **Default:** Linkwitz-Riley

| Type | Characteristics |
|------|-----------------|
| Butterworth | -3dB at crossover frequency, +3dB summed response bump |
| Linkwitz-Riley | -6dB at crossover frequency, flat summed response |

**Recommendation:** Linkwitz-Riley is preferred for most applications as it provides a flat combined response.

### Sample Rate
- **Options:** 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz
- **Default:** 48 kHz
- **Purpose:** Must match the audio source sample rate for accurate filter calculations.

**Note:** Mismatched sample rates will cause incorrect crossover frequencies.

---

## Low Output (Woofer) Settings

### Output Gain
- **Range:** -24 to +24 dB (0.5 dB steps)
- **Default:** 0 dB
- **Purpose:** Adjusts the woofer output level relative to the tweeter.

**Usage:**
- Use to balance woofer and tweeter levels
- Negative values reduce woofer output
- Typically adjusted by ear or with measurement microphone

### Subsonic HPF (High-Pass Filter)
- **Range:** 0 - 80 Hz (5 Hz steps)
- **Default:** 0 (disabled)
- **Purpose:** Protects the woofer from ultra-low frequencies that cause excessive excursion.

**Guidelines:**
- Set to just below the woofer's usable low-frequency limit
- Sealed boxes: typically 30-50 Hz
- Ported boxes: set at or slightly below port tuning frequency
- Prevents wasted amplifier power on inaudible frequencies

### Baffle Step Compensation

Baffle step is an acoustic phenomenon where low frequencies diffract around the speaker cabinet while high frequencies beam forward, causing a 6dB level difference.

#### Baffle Width
- **Range:** 0 - 50 cm (0 = disabled)
- **Purpose:** Enter the width of your speaker's front baffle.

The system calculates the step frequency using:
```
Step Frequency = 343 / (π × width_in_meters)
```

| Baffle Width | Step Frequency |
|--------------|----------------|
| 15 cm | ~728 Hz |
| 20 cm | ~546 Hz |
| 25 cm | ~437 Hz |
| 30 cm | ~364 Hz |

#### Placement
- **Options:** Freestanding (+6dB), Near Wall (+3dB), Corner (0dB)
- **Purpose:** Room boundaries provide natural bass reinforcement, reducing the compensation needed.

| Placement | Compensation | When to Use |
|-----------|--------------|-------------|
| Freestanding | +6 dB | Speaker >1m from any wall |
| Near Wall | +3 dB | Speaker within 0.5m of back wall |
| Corner | 0 dB | Speaker in or near a corner |

### Parametric EQ (PEQ) - 6 Bands

Each PEQ band allows precise frequency response correction.

#### Frequency
- **Range:** 20 - 20,000 Hz (0 = band disabled)
- **Purpose:** Center frequency of the filter.

#### Gain
- **Range:** -15 to +15 dB (0.5 dB steps)
- **Purpose:** Boost or cut at the center frequency.

#### Q (Quality Factor)
- **Range:** 0.5 - 10.0
- **Purpose:** Controls the width of the filter.

| Q Value | Bandwidth | Use Case |
|---------|-----------|----------|
| 0.5-1.0 | Very wide | Broad tonal shaping |
| 1.0-2.0 | Wide | General EQ adjustments |
| 2.0-5.0 | Medium | Room mode correction |
| 5.0-10.0 | Narrow | Notching specific resonances |

**Typical Uses:**
- Room mode correction (narrow cuts at problematic frequencies)
- Driver response smoothing
- Voicing adjustments

### Phase
- **Options:** Normal, Invert
- **Default:** Normal
- **Purpose:** Inverts the polarity of the woofer output.

**When to Use:**
- If woofer and tweeter are acoustically out of phase at crossover
- Test by ear: correct phase typically gives fuller sound with better imaging
- Some crossover slopes require phase inversion for proper summing

---

## High Output (Tweeter) Settings

### Output Gain
- **Range:** -24 to +24 dB (0.5 dB steps)
- **Default:** 0 dB
- **Purpose:** Adjusts the tweeter output level relative to the woofer.

### Tweeter Delay (Time Alignment)
- **Range:** 0 - 50 mm
- **Default:** 0 (disabled)
- **Purpose:** Delays the tweeter signal to align acoustically with the woofer.

**Why It's Needed:**
The acoustic center of a woofer is typically 15-25mm behind the cone surface, while a tweeter's acoustic center is only 5-10mm behind the dome. If both drivers are flush-mounted, the tweeter's sound arrives first, causing phase issues at the crossover frequency.

**How to Set:**
1. **Physical Measurement:** Measure the difference between driver acoustic centers
2. **Listening Test:** Adjust until transients sound tight and focused
3. **With Measurement:** Align impulse response peaks in measurement software

**Conversion:**
- 10mm delay ≈ 29 microseconds
- 20mm delay ≈ 58 microseconds
- 30mm delay ≈ 87 microseconds

### Breakup Notch

Most tweeters exhibit a resonance peak at their upper frequency limit (breakup mode). This notch filter tames that peak for smoother response.

#### Frequency
- **Range:** 0 - 25,000 Hz (0 = disabled)
- **Purpose:** Set to the tweeter's breakup frequency.

**Finding the Breakup Frequency:**
- Check the tweeter's datasheet/frequency response graph
- Look for a sharp peak near the upper limit
- Typical dome tweeters: 20-28 kHz
- Lower quality tweeters: may be 15-20 kHz

#### Depth
- **Range:** 0 to -12 dB (0.5 dB steps)
- **Purpose:** Amount of cut at the breakup frequency.

**Guidelines:**
- Start with -3 to -6 dB
- Match the height of the breakup peak in the frequency response
- Excessive notching can dull the high frequencies

#### Q (Quality Factor)
- **Range:** 2.0 - 10.0
- **Default:** 5.0
- **Purpose:** Width of the notch filter.

**Guidelines:**
- Higher Q = narrower notch (more surgical)
- Match the width of the breakup peak
- Typical values: 4-8

### Air / Brilliance Shelf
- **Range:** -6 to +6 dB (0.5 dB steps)
- **Default:** 0 dB
- **Corner Frequency:** Fixed at 10 kHz
- **Purpose:** Adjusts the overall "air" and sparkle in the treble.

**Usage:**
| Setting | Effect |
|---------|--------|
| +1 to +3 dB | Adds openness, detail, "air" |
| 0 dB | Neutral |
| -1 to -3 dB | Reduces brightness, less fatigue |

**When to Adjust:**
- Recording sounds dull → slight boost
- Recording sounds harsh/fatiguing → slight cut
- Personal preference for treble presentation

### Parametric EQ (PEQ) - 6 Bands

Same functionality as woofer PEQ. Common tweeter uses:
- Presence adjustment (2-5 kHz)
- Sibilance reduction (5-8 kHz)
- Response smoothing

### Phase
- **Options:** Normal, Invert
- **Default:** Normal
- **Purpose:** Inverts the polarity of the tweeter output.

---

## Loudness Compensation

Volume-dependent EQ that boosts bass and treble at lower listening levels, compensating for the ear's reduced sensitivity to frequency extremes at low volumes (Fletcher-Munson curves).

### Enable/Disable
- **Options:** On, Off
- **Default:** Off

### Zone Configuration

The volume range (0-100%) is divided into 5 zones, each with independent bass and treble boost settings.

#### Thresholds
Define the volume percentage boundaries between zones:
- **Zone 1:** 0% to Threshold 1
- **Zone 2:** Threshold 1 to Threshold 2
- **Zone 3:** Threshold 2 to Threshold 3
- **Zone 4:** Threshold 3 to Threshold 4
- **Zone 5:** Threshold 4 to 100%

**Default Thresholds:** 20%, 40%, 60%, 80%

#### Bass Boost (per zone)
- **Range:** -12 to +12 dB
- **Frequency:** Low shelf at ~200 Hz (woofer channel)

**Default Values:**
| Zone | Volume Range | Bass Boost |
|------|--------------|------------|
| 1 | 0-20% | +6 dB |
| 2 | 20-40% | +4 dB |
| 3 | 40-60% | +2 dB |
| 4 | 60-80% | +1 dB |
| 5 | 80-100% | 0 dB |

#### Treble Boost (per zone)
- **Range:** -12 to +12 dB
- **Frequency:** High shelf at ~5 kHz (tweeter channel)

**Default Values:**
| Zone | Volume Range | Treble Boost |
|------|--------------|--------------|
| 1 | 0-20% | +4 dB |
| 2 | 20-40% | +3 dB |
| 3 | 40-60% | +2 dB |
| 4 | 60-80% | +1 dB |
| 5 | 80-100% | 0 dB |

---

## Preset Export/Import

Save and share your bi-amp configurations.

### Export
Creates a JSON file containing all bi-amp settings:
- Crossover configuration
- All woofer settings (gain, subsonic, baffle step, PEQ, phase)
- All tweeter settings (gain, delay, notch, air, PEQ, phase)
- Loudness compensation settings

### Import
Load a previously exported preset. All settings are validated before applying.

**Use Cases:**
- Backup your configuration
- Share settings with others using the same speaker design
- Quickly switch between configurations for different speakers

---

## Quick Setup Guide

### Basic Setup
1. Set **Crossover Frequency** to your speaker's specified crossover point
2. Set **Slope** to 24 dB/oct (Linkwitz-Riley)
3. Set **Sample Rate** to match your audio source
4. Adjust **Woofer Gain** and **Tweeter Gain** to balance levels

### Adding Protection
1. Set **Subsonic HPF** to protect your woofer (typically 30-50 Hz)
2. If using breakup notch, set frequency from tweeter datasheet

### Fine-Tuning
1. Add **Baffle Step Compensation** if needed for your cabinet
2. Set **Tweeter Delay** based on physical driver offset
3. Adjust **Phase** if needed for proper driver summing
4. Use **PEQ** bands for room correction or response smoothing
5. Set up **Loudness Compensation** for low-volume listening

### Final Adjustments
1. Use **Air/Brilliance** shelf for overall treble character
2. Fine-tune gains by ear with music
3. **Export** your preset for backup

---

## Troubleshooting

### Sound is thin/lacks bass
- Check Subsonic HPF isn't set too high
- Verify Baffle Step Compensation settings
- Check woofer gain and phase settings

### Harsh or fatiguing treble
- Reduce Air/Brilliance shelf
- Check for and apply Breakup Notch
- Reduce tweeter gain

### Poor imaging or "hollow" sound at crossover
- Adjust Tweeter Delay for time alignment
- Try inverting phase on one driver
- Verify crossover frequency is appropriate

### Loudness compensation too aggressive
- Reduce bass/treble boost values in each zone
- Adjust thresholds for smoother transitions

---

## Technical Specifications

| Parameter | Specification |
|-----------|---------------|
| Filter Type | IIR Biquad (Direct Form I) |
| Coefficient Format | 32-bit floating point (converted to Q5.27) |
| Biquads per Channel | 15 |
| Crossover Slopes | 12/24/48 dB/octave |
| PEQ Bands | 6 per output |
| Gain Resolution | 0.5 dB |
| Frequency Range | 20 Hz - 20 kHz |
| Q Range | 0.5 - 10.0 |
