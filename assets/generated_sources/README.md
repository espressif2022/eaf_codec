# Generated visual masters

These two checked-in masters provide realistic visual complexity without using customer
or third-party source material. They were created with the built-in OpenAI image generation
tool on 2026-11-08, then used as immutable inputs to `bench/generate_visual_assets.py`.
Derived animation frames are deterministic for the recorded Pillow version.

| File | Purpose | SHA-256 |
| :--- | :--- | :--- |
| `robot_mascot.png` | Original transparent character for UI-style animation | `5c32669727c0e0661bd47f35168334ca160a2356083281a1ce2bc8f697edf2e2` |
| `mountain_lake.png` | Original photorealistic landscape for pan and water motion | `9b2b7f54fa12f0f6a0ec78ce1f9ebe1eecc5a64e8f1b7d42a828f361fba0646c` |

The character prompt requested an original friendly orange/white robot on a removable flat
background, with no text, logo or external character reference. The landscape prompt requested
a geographically generic mountain lake with forest, rocks, clouds, mist and reflections, with
no people, buildings, signs, logos or watermark.

These masters are visual benchmark fixtures, not a pixel-reproducible image-model benchmark.
Their checked-in bytes and SHA-256 values are the authoritative source.
The normalized prompt text and creation metadata are retained in `provenance.json`.
