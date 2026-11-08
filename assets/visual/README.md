# Visual benchmark animations

These frames are derived from the checked-in generated masters in
`assets/generated_sources`. They complement the procedural stress scenes with content that
resembles UI character animation and real photographic scenery.

| Scene | Coverage |
| :--- | :--- |
| `robot_float` | Transparent character, antialiased edges, rigid-body motion and scale |
| `landscape_pan` | Dense natural detail, sky gradients, slow camera pan/zoom |
| `landscape_water` | Mostly static photo with localized fine water motion |

Regenerate them with:

```bash
python3 bench/generate_visual_assets.py
```

`manifest.json` records master hashes, Pillow version and derived RGBA pixel hashes.
