# instanterchat

An Ashita v4 plugin that puts battle messages in the chat log as soon as their action packet
(0x028) arrives, instead of when the action's animation reaches them. Floating damage numbers keep
the animation's timing.

    /load instanterchat
    /instanterchat          status: on or off, results let out early, later chat lines dropped
    /instanterchat on|off   whether new actions are let out early (on at load)

Turning it off leaves the hooks in place until the plugin is unloaded, so animations already
playing don't print their chat lines a second time.

## How it works

The client keeps an action's results in a holder attached to the actor's animation scheduler. The
scheduler's script prints each result when it reaches the print command (opcode 0x2B, and 0x71 for
additional effects and spikes). Every print spawns the floating number and queues the chat line
together, in `09E090`.

instanterchat catches each holder as it's attached (`057CC0`). At the end of that frame, once the
packet handler has filled the holder in, it prints every result's chat line through `09E090` with
the floating-number branch (`9E229`) skipped for those calls. When the script's own prints come
later, they run unchanged, except that the chat line is dropped (the call at `9E35D`) for results
already printed, so only the floating number appears.

All changes are to the targets of existing `call` instructions, plus that one branch while
instanterchat's own calls run. Each is checked against the expected bytes first. On a different
client build, nothing is changed and the plugin says so.

The addresses are for the FFXiMain.dll this was built against (relative to the module base).

## Build

    ./build.sh

Needs `../.toolchain` (or `ASHITA_SDK_PATH` / `MSVC_PATH`): the Ashita v4 plugin SDK under
`ashita/plugins/sdk`, and the MSVC CRT and Windows SDK for x86 as laid out by
`xwin --accept-license --arch x86 splat --output msvc`.

