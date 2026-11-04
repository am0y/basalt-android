# Editor artwork

- `glass-icons.png`: monochrome atlas built from **Lucide Static 0.468.0**.
- `lucide/`: pinned original SVGs and ISC license, downloaded from the public npm package by `tests/build_editor_assets.py`. Subsequent builds use these local sources.
- `simple-icons/`: Discord brand glyph from Simple Icons 14.0.0, with its CC0 license.
- `basalt-logo.png`: transparent, padded rendition of the supplied **Basalt-B.png** logo. The original silhouette is preserved.

Rebuild icons with `uv run --with cairosvg --with pillow python tests/build_editor_assets.py`.
Pass `--logo /path/to/Basalt-B.png` only when regenerating the brand asset.
