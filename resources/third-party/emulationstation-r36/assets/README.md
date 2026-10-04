# Font Awesome runtime fallback asset

`fontawesome-webfont.ttf` is the unchanged Font Awesome 4.7.0 webfont from the upstream `FortAwesome/Font-Awesome` repository, tag `v4.7.0`, peeled commit `a8386aae19e200ddb0f6845b5feeee5eb7013687`.

- Upstream font: `https://raw.githubusercontent.com/FortAwesome/Font-Awesome/v4.7.0/fonts/fontawesome-webfont.ttf`
- Upstream README/license statement: `https://github.com/FortAwesome/Font-Awesome/blob/v4.7.0/README.md`
- Font SHA-256: `aa58f33f239a0fb02f5c7a6c45c043d7a9ac9a093335806694ecd6d4edc0d6a8`
- License: SIL Open Font License 1.1, as stated by the upstream README. The canonical license text is included as `OFL-1.1.txt` (SHA-256 `1a7adaa2c86cedfd6c7f5c0c7c72fd6d3e02cd0c9593f21fdb53c89bb2b130ec`); the official license page is <https://openfontlicense.org/open-font-license-official-text/>.

The font is not modified. Font Awesome's v4.7.0 README says attribution is appreciated but not required; the upstream project is Dave Gandy / Font Awesome. This recipe includes the font because pinned FCAMOD `es-core/src/resources/Font.cpp` searches for `:/fontawesome-webfont.ttf`, and `GuiTools.cpp` uses the Font Awesome codepoints U+F07B (folder) and U+F013 (tools/script). The pinned FCAMOD `source/resources` tree lacks the file, although the maintained R36 runtime resource has this exact font hash. `apply-r36-overrides.sh` copies this checked-in, hash-pinned asset to the temporary FCAMOD source resource directory without replacing a differing file.
