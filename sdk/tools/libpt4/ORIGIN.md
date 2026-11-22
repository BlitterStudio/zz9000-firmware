# libpt4 in this repository

`include/` and `src/` here are libpt4, the C replayer of ProTracker 4.1, copied
unchanged from <https://github.com/akustikrausch/protracker4> at commit
`b311eb0ba2c89719d5b725ba8eff892284a23800` (directory `libpt4/`). `LICENSE`
and `README.md` are the upstream files. The upstream build files, command-line
player and test programs are not copied.

libpt4 is under the zlib licence (`LICENSE`). The rest of the ProTracker 4.1
repository is under different terms and is not copied here.

The firmware host tests in `test/audio` use libpt4 as the reference for the
voice engine, and `zz9k-voiceplay` uses it to play modules through the engine.
The test modules in `test/audio/fixtures/pt4/` come from `libpt4/testdata/` at
the same commit and carry the same licence.

To update, copy the same files from a newer commit, change the commit above,
and run the audio host tests.
