# R36 EmulationStation source candidate

This directory contains a source-pinned EmulationStation-FCAMOD build recipe and the one R36-specific source override that is present in this checkout. It does not contain a firmware image, does not install a binary, and does not publish an OTA.

The candidate base is FCAMOD branch `351v` at `74498be31cd016af6a42d00310f876d7256eff52` (tree observed 2026-10-03). Vanilla dArkOS's `build_emulationstation.sh` selects `351v` for its RG351MP target but clones a moving branch name at build time. `scripts/fetch-pinned-source.sh` pins this snapshot and initializes its recorded submodule. This is a reproducible source starting point, not proof that the existing R36 frontend was built from it. The physical device binary and firmware-source overlay have different SHA-256 values, and the original R36 frontend source/build revision is unavailable.

## Source feature checks

The target branch already contains the relevant portable UI/source behavior. The individual feature commits named in vanilla notes are not all ancestors by their original commit IDs; upstream merged equivalent code under these full merge IDs:

| Feature | Source evidence at pinned `351v` | Merge/lineage evidence |
|---|---|---|
| Wi-Fi status indicator toggle | `es-app/src/guis/GuiMenu.cpp:1311-1319` exposes `SHOW NETWORK ICON`; `es-core/src/Settings.cpp:108` defaults `ShowNetworkIndicator` to true; `es-core/src/components/ControllerActivityComponent.cpp:395-412` gets an IPv4 address and gates the status on that setting. | Indicator commit `44c43bae0c36629ed6ee552c3ca0ad607cb4fba1` is reachable; original UI commit `971087a7eaa853d7019ca55033f9397a15bee19` is not an ancestor, but merge `287070bd0246b7228786c18ba4c7f357753bab78` is and the code is present in the pinned files. |
| TheGamesDB user key | `es-app/src/guis/GuiMenu.cpp:248-253` adds `API KEY`; `es-app/src/scrapers/GamesDBJSONScraperResources.cpp:56-59` reads `GamesDBApiKey` before the build fallback. | Original commit `a1c5ef5d1fb1f64db03f55e7dd4c7f0a2177edf6` is not an ancestor; merge `558fbfbb93726e7c83579f037ab8a96a651b5095` is an ancestor and the implementation is present in both pinned files. No API credential is set or stored by this build recipe. |
| GuiTools menu labels | `es-app/src/guis/GuiTools.cpp:43-46,106,116` constructs `NameResolver` and resolves folder/script labels; `es-core/src/utils/NameResolver.cpp:127-169` uses `.name`, `# NAME:`, then filename fallback. | Initial implementation `656853ab0de1326468ccdf4d7b18397349e1f867` is reachable. Later commit `d610557c538b0cb12766771c64c92db3dda64e80` is not an ancestor, but the four resulting files (`GuiTools.cpp/.h`, `NameResolver.cpp/.h`) have identical content on the inspected `351v` and `master` tips; do not count this as a code gap. |
| Battery icon refresh after theme change | `es-core/src/components/ControllerActivityComponent.cpp:389-392` clears cached theme texture/image and resets level. | `c728158106409913a9e406e15b17ddf10e5403a0` is reachable from `351v` and predates the 2026-03-10 R36 source release. |

The Wi-Fi implementation in the pinned branch queries `wlan0`. This candidate keeps vanilla behavior unchanged; no interface-discovery change is included without device evidence. Polish/Italian locale changes and BatteryPlus integration are not part of this build candidate. The `351v` language-selector block is commented out, and the R36 image has its own battery subsystem.

## R36-specific override

`patches/0001-r36-volume-control.patch` replaces only `es-core/src/VolumeControl.cpp` with the exact source retained at `overrides/VolumeControl.cpp` and in the firmware overlay at `resources/github/EmulationStation-fcamod/VolumeControl.cpp` (SHA-256 `763dd7c2608799b437a5a18ba2c5ec3005e892a46b032e3b8e5f99f63359252a`). It selects ALSA element `Playback` on card `rockchiprk817co` and contains the R36-specific nonlinear volume read/write mapping. Replacing this source with the generic upstream file would lose the R36 mixer selection and level mapping.

The exact retained override has two whitespace diagnostics under `git diff --check` (a trailing space on the mixer-card line and indentation before a tab in an error return). The patch deliberately preserves the source byte-for-byte; the preparation script verifies its SHA-256 and does not rewrite it to silence style diagnostics.

## Prepare and build

From the firmware fork root:

```sh
resources/third-party/emulationstation-r36/scripts/fetch-pinned-source.sh /tmp/EmulationStation-fcamod-r36
resources/third-party/emulationstation-r36/scripts/apply-r36-overrides.sh /tmp/EmulationStation-fcamod-r36
resources/third-party/emulationstation-r36/scripts/build-r36-candidate.sh /tmp/EmulationStation-fcamod-r36
```

The preparation script refuses another FCAMOD revision, checks that the Wi-Fi toggle and user API-key behavior are already in the pinned source, and applies the R36 volume override, dependent-type portability fix, and `batteryIndicator` theme-property adaptation. The latter accepts Colorful v2's existing `networkIcon` path using FCAMOD's inherited `applyTheme()` handler; the firmware's theme files remain untouched. It also installs the pinned Font Awesome 4.7 fallback font into the temporary source resource tree. FCAMOD's Linux `Font.cpp` searches for this font for its `U+F07B` folder and `U+F013` options glyphs, but its checked-in resource tree omits it; the maintained R36 runtime already supplies the same font with SHA-256 `aa58f33f239a0fb02f5c7a6c45c043d7a9ac9a093335806694ecd6d4edc0d6a8`. The asset is pinned, hash-checked, and copied idempotently; see `assets/README.md` for source and license provenance. These preparations do not replace the upstream source tree's other assets or R36 locale. The build script defines empty `GAMESDB_APIKEY` and `SCREENSCRAPER_DEV_LOGIN` macros. These enable both providers without embedding GamesDB or ScreenScraper credentials; see the authentication note below.

The build needs the FCAMOD dependencies listed in its upstream `README.md`: CMake, C++ compiler, SDL2, SDL2_mixer, FreeImage, FreeType, cURL, VLC, RapidJSON, Boost, Eigen, ALSA, OpenGLES and the pinned submodules. Do not install FCAMOD's bundled Debian 10 Mali `.deb` files on R36S. Use an AArch64 Debian 13 build host or a matching AArch64 cross compiler and R36 sysroot. Set `CMAKE_TOOLCHAIN_FILE`, `CMAKE_SYSROOT`, `CC`, and `CXX` as appropriate for a cross build. The upstream CMake sets the executable output directory to the source tree, so the candidate appears as `FCAMOD_SOURCE/emulationstation`; the script refuses to overwrite an existing file there and never copies the result into the firmware overlay or device.

### Scraper providers and ScreenScraper authentication

The build recipe defines both provider gates as empty CMake values. In this pinned source, `SCREENSCRAPER_DEV_LOGIN` and `GAMESDB_APIKEY` are tested for definition, so both ScreenScraper and TheGamesDB appear in the `SCRAPE FROM` selector. The empty GamesDB fallback does not replace the API key entered by the user in EmulationStation settings.

An empty `SCREENSCRAPER_DEV_LOGIN` is sufficient to compile and expose the ScreenScraper provider and its user-name/password settings, but it is **not sufficient to authenticate API requests**. FCAMOD concatenates that macro into request URLs as developer parameters; separately, it appends the user's `ssid`/`sspassword` when those settings are configured. ScreenScraper's [official API documentation](https://www.screenscraper.fr/webapi2.php) lists developer ID, developer password, and software name as request parameters, distinct from the optional user credentials. Do not claim that ScreenScraper scraping works from this credential-free build. No developer credentials, user credentials, or old-binary credential values are extracted, checked into source, or included in this recipe.

Live ScreenScraper requests in a distributable build need an authorized credential design first. A compile-time developer password is embedded in the executable and can be recovered from it; passing it through a CMake command-line definition can also expose it in process listings, CMake caches, or build logs. This public recipe intentionally supplies no such secret. Provider/source/UI availability is restored; authenticated service use remains blocked until credentials can be supplied through a safe, authorized design and then tested.

The earlier review-host note that no CMake build had been run is superseded by the isolated cross-build recorded below and in `research/vanilla-audit/es-candidate-build-20261003.md`. That build procedure is pinned by recorded compiler, sysroot, and package inputs, but is not yet a bit-for-bit reproducible build from a clean host. Record those toolchain inputs before treating an output as a release candidate.

## Verification gates before any future installation

1. Confirm the source checkout is exactly the pinned commit and submodule gitlinks.
2. Apply the override and compare it byte-for-byte with the retained R36 source file.
3. Build for AArch64 and inspect ELF machine, interpreter, linked libraries and resource layout.
4. Compare the candidate against the running device UI and current launcher/service integration. Test EmulationStation start/exit, Wi-Fi toggle/display, TheGamesDB on-screen key entry, tools menu, volume controls, theme reload, and both `/roms` and `/roms2` library views.
5. Only after those checks should a separate OTA review consider the candidate binary. This directory does not authorize or perform that installation.

## Provenance and license

- Source repository: <https://github.com/christianhaitian/EmulationStation-fcamod>
- Source ref: `351v` at `74498be31cd016af6a42d00310f876d7256eff52`.
- Feature references: `971087a7eaa853d7019ca55033f9397a15bee19`, merge `287070bd0246b7228786c18ba4c7f357753bab78`; `a1c5ef5d1fb1f64db03f55e7dd4c7f0a2177edf6`, merge `558fbfbb93726e7c83579f037ab8a96a651b5095`; `656853ab0de1326468ccdf4d7b18397349e1f867`; `d610557c538b0cb12766771c64c92db3dda64e80`; `c728158106409913a9e406e15b17ddf10e5403a0`.
- FCAMOD's root `LICENSE.md` at the pinned source states the MIT License. Its text is retained in `UPSTREAM-LICENSE-MIT.txt`. Submodules and bundled third-party dependencies have their own licenses; this source patchset does not redistribute their contents.

## Superseding cross-build evidence — 2026-10-03

The host cross-build has now completed using Clang22/LLD22 and an isolated AArch64 Debian13 sysroot. The earlier paragraph describing missing host build inputs is superseded for this dated candidate. The explicit two-line dependent-type patch is retained and applied idempotently. CMake4 requires the recorded policy minimum3.5. No compiler/source change installs a runtime on the device.

For cross builds, provide the reviewed R36 graphics input through R36_EGL_LINK_LIBRARY (a libEGL.so alias) and R36_GLES_INCLUDE_DIR. The captured native R36 MaliG31 object has SHA2562a067ea38256c3de139b2a5396d3101f7c7777b1e19d59017b2c02172500d0ea and no SONAME. LLD rejects malformed .dynsym section metadata in that vendor object; the isolated build used a private link-only copy with sh_info corrected from3 to10, SHA2562d3a892f3f04a19a508d870197df48c600b19843c5dfa2d7828c4bc9a09da533. Neither copy is included in this repository or an OTA, and no device graphics library is edited. Final graphics DT_NEEDED must be libEGL.so, with no GLVND/GLESv1_CM dependency or RPATH/RUNPATH; the recipe rejects those unexpected outputs. Review GCC/BFD behavior separately if using another toolchain.

The package lock, compiler/link commands and signed snapshot evidence are retained in the project build record. Candidate SHA256859d3159a29de59a08b767dd5647524f31a75dceef698b6fcc69b99a9ba38bd0 passed the physical R36 loader, `--help` and a 20-second isolated-home startup/clean shutdown with GO-Super detection and Playback mixer initialization. A later provider-gate rebuild is recorded in `research/vanilla-audit/es-dual-scraper-recipe-validation-20261003.md`; its fresh ELF has both provider request symbols. This establishes startup and compile-time provider presence only: selector visibility, authenticated ScreenScraper use, visible rendering, controller navigation, tools return, Wi-Fi transitions, key entry and theme cache behavior remain required before a binary release. No binary is added by this source-recipe update.
