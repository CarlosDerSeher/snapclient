# DSP Processor Dynamic Configuration Schema

## Overview
This document defines the JSON structure for DSP processor capabilities that enables dynamic UI generation.

## JSON Schema Example

```json
{
  "version": "1.0",
  "flows": [
    {
      "id": "dspfEQBassTreble",
      "name": "Bass & Treble EQ",
      "description": "Simple 2-band equalizer with bass and treble controls",
      "parameters": [
        {
          "key": "fc_1",
          "name": "Bass Frequency",
          "type": "float",
          "unit": "Hz",
          "min": 50.0,
          "max": 500.0,
          "default": 300.0,
          "step": 10.0,
          "ui_control": "slider"
        },
        {
          "key": "gain_1",
          "name": "Bass Gain",
          "type": "float",
          "unit": "dB",
          "min": -12.0,
          "max": 12.0,
          "default": 0.0,
          "step": 1.0,
          "ui_control": "slider"
        },
        {
          "key": "fc_3",
          "name": "Treble Frequency",
          "type": "float",
          "unit": "Hz",
          "min": 2000.0,
          "max": 12000.0,
          "default": 4000.0,
          "step": 100.0,
          "ui_control": "slider"
        },
        {
          "key": "gain_3",
          "name": "Treble Gain",
          "type": "float",
          "unit": "dB",
          "min": -12.0,
          "max": 12.0,
          "default": 0.0,
          "step": 1.0,
          "ui_control": "slider"
        }
      ]
    },
    {
      "id": "dspfBassBoost",
      "name": "Bass Boost",
      "description": "Fixed +6dB bass enhancement",
      "parameters": [
        {
          "key": "fc_1",
          "name": "Bass Frequency",
          "type": "float",
          "unit": "Hz",
          "min": 50.0,
          "max": 500.0,
          "default": 300.0,
          "step": 10.0,
          "ui_control": "slider"
        }
      ]
    },
    {
      "id": "dspfBiamp",
      "name": "Bi-Amp Crossover",
      "description": "Channel 0: Low-pass, Channel 1: High-pass",
      "parameters": [
        {
          "key": "fc_1",
          "name": "Crossover Frequency",
          "type": "float",
          "unit": "Hz",
          "min": 80.0,
          "max": 500.0,
          "default": 300.0,
          "step": 10.0,
          "ui_control": "slider"
        },
        {
          "key": "gain_1",
          "name": "Low-Pass Gain",
          "type": "float",
          "unit": "dB",
          "min": -12.0,
          "max": 12.0,
          "default": 0.0,
          "step": 1.0,
          "ui_control": "slider"
        },
        {
          "key": "fc_3",
          "name": "High-Pass Frequency",
          "type": "float",
          "unit": "Hz",
          "min": 80.0,
          "max": 500.0,
          "default": 100.0,
          "step": 10.0,
          "ui_control": "slider"
        },
        {
          "key": "gain_3",
          "name": "High-Pass Gain",
          "type": "float",
          "unit": "dB",
          "min": -12.0,
          "max": 12.0,
          "default": 0.0,
          "step": 1.0,
          "ui_control": "slider"
        }
      ]
    },
    {
      "id": "dspfStereo",
      "name": "Stereo Pass-Through",
      "description": "No DSP processing, optional soft volume",
      "parameters": []
    }
  ],
  "current_flow": "dspfEQBassTreble"
}
```

## Parameter Types
- `float`: Floating-point value
- `int`: Integer value
- `enum`: Enumerated value (requires `options` array)

## UI Control Types
- `slider`: Continuous range slider
- `number`: Numeric input box
- `select`: Dropdown menu (for enums)
- `toggle`: On/off switch
