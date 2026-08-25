# classicube-textanywhere-plugin
Put text anywhere on the screen.

#### Setup

1) Create `ClassiCube.exe` by either:
    1) Compiling the game, see `Cross compiling for windows (64 bit)` in [main readme](/readme.md#cross-compiling-for-windows-64-bit)
    2) Downloading 64 bit ClassiCube from https://www.classicube.net/download/#dl-win
2) Install the `mingw-w64-tools` package (if it isn't already)
3) Generate the list of exported symbols from `ClassiCube.exe` by running:
    * `gendef ClassiCube.exe`
4) Create a linkable library from the exported symbols list by running: 
    * `x86_64-w64-mingw32-dlltool -d ClassiCube.def -l libClassiCube.a -D ClassiCube.exe`

#### Compiling

`x86_64-w64-mingw32-gcc TextAWPlugin.c -o text-win64.dll -s -shared -L . -lClassiCube`

Then put `text-win64.dll` into your game's `plugins` folder. Done.

## How to use
Usage: `/client help text`

For adding `/client text add <id1> <x> <y> <scale> <rotation> <text>`

For editing `/client text add <id1> <modified x> <modified y> <modifed scale> <modified rotation> <modified text>`

For removing `/client text remove <id>`

For clearing every single one `/client text clear`

For servers its `&7text add/remove/clear`

### Example

You can do `/client text add 1 0.5 0.1 1 0 &eExample`
