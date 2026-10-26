# Third-party notices

- Firmware application code retains the original MIT license and copyright notices in `LICENSE`. This snapshot derives from the Egga firmware and its Xiaozhi component lineage.
- `main/components/78__esp-ml307`: MIT; preserve its bundled `LICENSE` and manifest. Upstream: https://github.com/78/esp-ml307
- `main/components/78__esp-wifi-connect`: MIT as declared by its manifest. Upstream: https://github.com/78/esp-wifi-connect
- `main/components/78__xiaozhi-fonts`: source component MIT as declared by its manifest. Upstream: https://github.com/78/xiaozhi-fonts . Font data is governed separately below. Egga icon glyphs are retained from the project's existing font assets.
- Montserrat, Source Han Sans SC: SIL Open Font License 1.1. DejaVu Sans: Bitstream Vera / DejaVu license. Complete notices accompany the font sources in `third_party/fonts/`. Sources are distributed with LVGL v9.3.0 at https://github.com/lvgl/lvgl/tree/v9.3.0/scripts/built_in_font . Generated subsets remain subject to their font licenses; the project MIT license does not replace them.
- `lv_font_conv` 1.5.3 is a development tool under MIT: https://github.com/lvgl/lv_font_conv . Its dependencies retain their respective licenses.
- ESP-IDF and dependencies fetched by the IDF Component Manager retain their own licenses. See `main/idf_component.yml` and the fetched component manifests and license files. They are not relicensed by this repository.

The original bundled examination vocabulary, recordings, Windows Calibri glyphs, unidentified Zfull glyph data, and third-party emoji images are excluded. The example deck is original content under the project MIT license. No commercial cloud-service entitlement or trademark license is granted.

- Noto Sans Symbols 2: SIL OFL 1.1, copyright 2022 The Noto Project Authors. Source: https://github.com/google/fonts/tree/main/ofl/notosanssymbols2 . Its bundled font and license supply supplemental symbols missing from Source Han Sans.
