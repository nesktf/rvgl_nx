## RVGL Nintendo Switch wrapper

This is a wrapper of the Android version of RVGL. It loads the original game binary, patches it and runs it.

This wrapper is based on the [Max Payne switch wrapper by fgsfdsfgs](https://github.com/fgsfdsfgs/max_nx)

### Installation
1. Create a folder called `rvgl` in the `switch` folder on your SD card.
2. Get your game assets from your original copy of the game, or from [Re-Volt IO](https://re-volt.io/downloads/misc). Add the music and the dreamcast pack if you want to. Put everything in `/switch/rvgl/assets`.
3. Download the [rvgl APK](https://rvgl.org/downloads/rvgl_23.1030a1_android.apk). Make sure it's the version `23.1030a1`.
4. Extract the `assets/` folder into `/switch/rvgl/assets` (overwrite files if asked):
5. Extract the files `lib/arm64-v8a/libmain.so`, `lib/arm64-v8a/libsndfile.so` and `lib/arm64-v8a/libunistring.so` into `/switch/rvgl`.
6. Put the `rvgl_nx.nro` binary in `/switch/rvgl`.

### Playing the game
Most things should work out of the box. The game creates a default profile on first load with all the switch controller mappings.

I've tested both hosting and joining a multiplayer game with a Linux PC. You can press the X button in any screen that has a text input field to show the on screen keyboard.

The touch screen controls should also work, if you want to use them just modify the overlay opacity in the controller settings.

User made content should work just fine, but I haven't tested anything outside of the dreamcast pack.

### Building
Install devkitA64 and the following libraries:
- `switch-mesa`
- `switch-mpg123`
- `switch-openal-soft`
- `switch-sdl2`
- `switch-sdl2_image`
- `switch-zlib`

Make sure that you have `$DEVKITPRO` defined in your env vars, clone the project and compile as usual.
```sh
git clone https://github.com/nesktf/rvgl_nx.git && cd rvgl_nx
make
```

### Slop disclosure
I've used generative AI models to do most of the binary analysis over the game libraries and all the necessary function patching. Check the [SLOP_SUMMARY](./SLOP_SUMMARY.md) file for a summary on what the model did to run the game.

This project has no affiliation with the original Re-Volt game or the RVGL project. I made this for personal use to play with friends and I'm just sharing it in case somebody finds it useful.

### Credits
- RV Team for RVGL
- fgsfdsfgs for the original dynamic library loader for the switch
- Every other android wrapper port for the switch I guess, since those were probably in the model weights :p
