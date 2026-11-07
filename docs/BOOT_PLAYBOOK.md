# Starting StageRig: the playbook

What should happen every time, and what to do when it doesn't.

## Start

1. Switch on the audio interface and the keyboards. The order doesn't matter, and
   one switched on late is picked up by itself.
2. Start StageRig:
   - from a source build: `build/StageRig_artefacts/Release/StageRig`
   - from a package: `StageRig`, or from the applications menu
3. Run `scripts/stagerig-check`. It only looks, changes nothing, and lists anything
   marked **FIX** or **LOOK**.
4. Before a show, also run **Options ▸ Pre-show check...**, which knows about the
   plugins and the songs.

A rebuilt program takes effect only after a restart. The check tells you when the
running one is out of date.

## What comes back by itself

| After a normal quit or a crash | |
|---|---|
| The setup file | Saved on quit, and written about once a minute (sooner after a program change) |
| The song, set position, scene, programs and tempo | As they were; a backing track is armed, never started |
| The chosen input, and the instrument and page the knobs are on | As they were |
| Perform, Stage, Songs and Keys | As they were |
| The window's size and place | As at the last normal quit |
| Audio device, tablet, how each keyboard is driven | As set |

A crash restarts StageRig by itself, up to five times in two minutes. Starting with a
setup file named on the command line opens that file at its saved programs instead.

## When something is wrong

| What you see | Do this |
|---|---|
| **Keyboard's keys are silent** | Run the check. Is the device connected? Is its input's channel right? Is the input on a program with an instrument? |
| **Knobs, lights and screens are dead, keys play** | Options ▸ Your keyboards. An MK1 set to Plain MIDI stays dark on purpose. Otherwise unplug it for ten seconds. If that fails, power it off at its switch. |
| **The S88 isn't on USB at all** | Check its power and cable, and use a port on the computer itself: the rear port works, the internal hub has failed before. |
| **Knob moves but you hear nothing** | Its parameter may belong to an effect that's switched off. See what it turns in the Knobs window. |
| **No sound at all** | Audio... in the toolbar: the right device? Then PANIC, then the check. |
| **Crashes at every start** | After five in two minutes the watchman stops. Note the last line it printed, then `coredumpctl list`. |
| **Tablet says "That code did not match"** | Type the code from the Tablet window on the computer. |
| **Tablet says "Can't reach StageRig"** | Same network? Is the address the one the check prints? Is a firewall in the way? |

## Testing beside a running rig

Give the copy its own folder with `STAGERIG_SETTINGS_DIR`. It reads the setup its settings
name but saves only inside that folder, so it can't touch the rig's file. Also change
`remotePort` in the copy's settings, and leave its MK1 on Plain MIDI so it doesn't take
the keyboard from the rig.
