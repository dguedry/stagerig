# StageRig user manual

StageRig is a live-performance plugin host for Linux, for keyboards and for
audio. Your keyboards play instruments; a microphone, a guitar or a hardware
synth plugged into the audio interface runs through effects. You build a
program for each sound you need, and change sounds from the keyboard, a
footswitch or a tablet while you play.

This manual covers everyday use. For building from source, the process model
and the code layout, see the [README](../README.md).

- [The idea in one minute](#the-idea-in-one-minute)
- [First run](#first-run)
- [The main window](#the-main-window)
- [Inputs: keyboards, and audio](#inputs-keyboards-and-audio)
  - [Audio inputs](#audio-inputs)
- [Programs: your sounds](#programs-your-sounds)
- [Songs and sets](#songs-and-sets)
- [Charts on the stand](#charts-on-the-stand)
- [Outputs and the master chain](#outputs-and-the-master-chain)
- [Slots: instruments inside a program](#slots-instruments-inside-a-program)
- [Splits and layers](#splits-and-layers)
- [Effects](#effects)
- [MIDI mappings: knobs and pedals](#midi-mappings-knobs-and-pedals)
- [Reading the stage display](#reading-the-stage-display)
- [Selecting programs from a tablet](#selecting-programs-from-a-tablet)
- [Tempo](#tempo)
- [Printing a program map](#printing-a-program-map)
- [The on-screen keyboard](#the-on-screen-keyboard)
- [Audio settings and latency](#audio-settings-and-latency)
- [Saving, autosave and plugin state](#saving-autosave-and-plugin-state)
- [Playing live](#playing-live)
- [Plugin formats](#plugin-formats)
- [Windows plugins](#windows-plugins)
- [When something goes wrong](#when-something-goes-wrong)
- [Files StageRig uses](#files-stagerig-uses)
- [Keyboard and mouse reference](#keyboard-and-mouse-reference)

## The idea in one minute

An **input** is a source of sound or of notes, and there are two kinds:

- a **keyboard** -- a MIDI device, or one MIDI channel of it -- which plays
  instruments;
- an **audio input** -- one or two of the audio interface's input channels: a
  microphone, a guitar, a hardware synth -- which runs through effects.

Each input holds **128 programs**. On a keyboard, a program is one sound of
your set: one or more **slots**, each a plugin instrument with its own key
range, velocity range, transpose, gain, pan and effects. On an audio input, a
program is an **effect chain** with an input trim -- the clean vocal, the
vocal with the long reverb, the vocoder.

While you play, a Program Change message selects the program on that input --
from the keyboard itself, or from a footswitch for an audio input. Every input
changes independently, so the left hand can stay on a bass while the right
hand moves through leads, and the vocal can go to its ballad reverb without
touching either.

```
Keyboard ──> Input "Upper" (ch 1)    ──> Program 007 "Solo"   ──> Slot 1  Lead synth   C4-C8
                                                              └─> Slot 2  String pad   C4-C8, soft only
Keyboard ──> Input "Lower" (ch 2)    ──> Program 002 "Bass"   ──> Slot 1  Electric bass C0-B3
Mic      ──> Input "Vocal" (in 1)    ──> Program 004 "Ballad" ──> EQ ──> Plate reverb
```

Every input lands on an **output** -- a pair of the interface's output
channels -- with its own master effects and volume, so the keyboards and the
vocal can go to the desk on separate channels.

## First run

1. Start StageRig. It opens with two empty inputs, **Upper** and **Lower**.
2. Open **Audio...** and choose your output. StageRig defaults to the JACK
   device type at 48 kHz, which on a PipeWire system is the lowest-latency
   path. See [Audio settings and latency](#audio-settings-and-latency).
3. Open **Plugins...** and press **Scan VST3**, **Scan LV2** or **Scan LADSPA**.
   The scan runs in the background; you can close the window and keep working.
   Each plugin is tested in a separate process, so a plugin that crashes or
   hangs cannot take the scan down with it.
4. Select **Upper** on the left, choose its **MIDI device** and **Channel**.
5. Click program **000** in the middle list, name it, and press
   **+ Add instrument...**.
6. Play. Press **Save As...** when the set is worth keeping.

**Help** in the toolbar opens this manual inside StageRig, so the answer to
"why won't the tablet connect" is reachable from the stage without finding a
browser. The list on the left jumps to a section, and the search box narrows to
whichever section answers what you typed.

## The main window

| Area | What it does |
|---|---|
| **Toolbar** (top) | New / Open / Save / Save As, **Tablet**, **Help**, **Audio...**, **Plugins...**, **Options** (preload all programs, hold notes across program changes, show songs and set lists, outputs and master effects, rescan MIDI, print the program map), the tempo and **Tap**, release **Tail**, **Stage**, **Keyboard**, **PANIC**. |
| **Inputs** (left) | Your keyboards. Select one to edit its programs; its device, channel and Program Change setting are below the list. |
| **Programs** (middle) | The 128 programs of the selected input. Clicking one activates it. The name box, **Copy**, **Paste** and **Clear** are below. |
| **Plugins** (right, top) | The slots of the current program, each with its controls and effect chain, plus the program effect chain. |
| **MIDI mappings** (right, bottom) | Controller-to-parameter mappings for this program, with Learn and Suggest. |
| **Keyboard** (bottom, optional) | An on-screen keyboard for testing without a controller. |
| **Status bar** | Current file, CPU, sample rate, block size and latency, and warnings such as *late N* or *clock held by another app*. |

**PANIC** sends all-notes-off to every loaded plugin. Use it if a note hangs.

The panes are separated by thin dividers (three dots in the gap). Drag one to
give a pane more room -- the set list wider when song names are long, say --
and the widths are remembered. When the window is too narrow for them all,
the panes are squeezed for the time being -- programs first, then inputs,
then the set list -- and spread out again as the window grows; the right-hand
side always keeps room for the mappings table.

## Inputs: keyboards, and audio

An input is a keyboard, usually. **+ Add** also offers an **audio input**: a
microphone, a guitar, a hardware synth or anything else plugged into the audio
interface. Its programs are effect chains -- there is no instrument to load,
the signal is the instrument -- and in every other way it is an input like the
others: it has 128 programs, it changes them from a footswitch, a song or the
tablet, its effects take MIDI mappings, and a program change leaves the
outgoing chain to ring out for the tail. See *Audio inputs* below.

Each keyboard input has:

- **MIDI device** — any ALSA MIDI source. **Options ▸ Rescan MIDI devices** picks
  up a keyboard you plugged in after starting. If a saved device is missing,
  the list shows it as *(disconnected)* and reconnects when it returns.
- **Channel** — `Omni` accepts every channel, or pick one. Two inputs can share
  one physical keyboard by taking different channels, which is how a single
  controller drives both a left-hand and a right-hand part.
- **Respond to Program Change** — untick for an input whose sounds you want to
  change only by hand.
- **PC on** — which channel this input's Program Change messages arrive on.
  *Same as notes* is the default and is right for most keyboards. Some
  workstations send program changes on a fixed channel instead of the one they
  play on: a Roland Jupiter-50, for example, sends its registration changes on
  channel 16 while its keyboard parts play on 1, 3 and 4, so setting **PC on**
  to *Ch 16* makes the front-panel registration buttons select StageRig
  programs while the notes still arrive on their own channel. *Any channel*
  accepts program changes from anywhere on that MIDI port, which is the setting
  to try when program changes are not getting through and you do not know what
  the keyboard sends.

**+ Add** and **- Remove** manage the list. There is no fixed limit of two; add
an input for a pedalboard, a second tier, or a drum pad.

### Audio inputs

An audio input has, in place of the instruments:

- **Audio channels** — which of the interface's inputs it listens to: one
  channel (mono, sent to both sides) or a pair. The list offers the channels
  that are open in **Audio...**; if it says no input is open, choose an input
  device and tick its channels there first.
- **Control MIDI device**, **Channel** and the Program Change settings work as
  for a keyboard. Program changes step through the effect chains the way they
  step through sounds on a keyboard; controllers drive mapped effect
  parameters, the tap pedal, the song footswitches and an output's volume.
  Notes are passed to the effects too -- a vocoder or harmoniser wants them --
  though there is no instrument here to play them.
- Each program has an **Input trim** (in the header of the effects panel) and
  an effects chain, added with **+ Add effect** on the chain. A program with no
  effects passes the signal straight through at its trim.
- A **level meter** shows what is arriving, before the trim: in the input's row
  of the list, beside the trim, under the program name on the stage display,
  and in the input's header on the tablet. Two bars, green to -12 dB, amber to
  -3, red above, and a mark that stays lit for two seconds after the signal
  touches full scale. Set the interface's gain so the loudest you play stays
  out of the red; the trim is for balancing programs, not for rescuing a hot
  input.

The tablet's **GUI** button opens the chain's effects for an audio input, the
way it opens instruments for a keyboard.

## Programs: your sounds

The middle column is 128 program slots, numbered the way your keyboard's
Program Change messages are (000-127). A program in **bold with a plugin count**
has something in it.

- **Click** a program to make it current and edit it.
- **Name** it in the box below the list. Names appear in the list and make a
  set list readable from a distance.
- **Copy** / **Paste** duplicate a whole program, including plugin state, to
  another number. This is the fast way to make a variation of a sound.
- **Clear** empties a program.

**Preload all programs** (under **Options** in the toolbar) loads every non-empty program at start and
keeps it loaded, so program changes are instant. Without it, a program loads
when you select it, which takes as long as the plugin needs (Kontakt is several
seconds). Preloading costs RAM: each plugin instance is a full copy.

**Hold notes across program changes** (under **Options**, on by default) means
a program change leaves whatever you are holding to the previous program: keys still down and the sustain pedal keep
sounding there, following the wheels and pedals, until you let go. New notes go
to the new program from the moment of the change. Turn it off for a clean cut
at the change -- percussive sounds, or a switch you want to land exactly on the
beat.

**Tail** is how long the outgoing program keeps rendering after that -- after
you let go with Hold on, after the change with it off -- so releases and
reverbs fade naturally instead of being cut off. Four seconds is a good default.

### Grouping programs

Each program can carry a group: **Organs**, **Strings**, **Brass**, whatever
suits your set. Type it in the box under the program name, or press the small
button beside it to pick a group already in use on that input.

A group is a label, not a folder. A program's number is fixed, because that is
what your keyboard sends as a Program Change, so grouping never moves or
renumbers anything. What it does is make 128 programs findable: the tablet shows
a row of group buttons to filter by, and the printed map prints each group as
its own section.

Groups belong to an input, like everything else about a program. Program 007 on
Upper has nothing to do with 007 on Lower, and neither do their groups, so both
keyboards can have their own "Keys" meaning different sounds. Programs with no
group are listed under **Other**, never hidden, and if you use no groups at all
nothing changes anywhere.

## Songs and sets

A program is a sound. A **song** is the moment the band starts playing: one
program on each keyboard, at the right tempo. A **set** is the order the songs
come in. Without this layer, starting a number means picking Upper 002, then
Lower 001, then the tempo -- three actions, at the wrong moment.

Turn on **Options ▸ Show songs and set lists**. The panel appears at the left,
with every song on top and the running
order of the chosen set below.

### Making a song

Get the sound right first: pick the program on each keyboard, set the tempo.
Then press **+ Add**. The dialog shows exactly what is about to be stored and
asks for a name.

That is the whole idea -- a song remembers what was loaded when you captured it,
rather than being filled in by hand. **Capture** stores the current state into a
song you already have, for when you change your mind about a sound.

A song holds a **program number per input**, not a copy of the plugins. So
editing program 002 changes every song that uses it: one place to fix a sound.
An input the song does not name is left alone, which is what you want for a
keyboard you are not using in that number.

- **Edit** sets the name, the stage notes and the tempo.
- **Stage notes** are free text shown under the song on the tablet while it is
  playing -- the key, a count-in, "watch the horn stabs": whatever you would
  write on a paper set list.
- A **tempo** of blank means "leave the tempo alone", which is not the same as
  zero. Use it for a song where the tempo is whatever you last tapped.
- **^ v** beside the songs list reorder it. Sets keep playing the same order:
  they follow the song, not its position.

### Making a set

**Set...** makes one, renames it, moves it or deletes it. With a song selected,
**Add to set >** puts it at the end; **^ v** beside the set order it, and
**< Remove** takes it out of the set without deleting the song.

A song can appear in more than one set, and more than once in the same set --
encores happen. Deleting a song takes it out of every set that used it.

### Playing a set

Click a song to load it: every input it names, and its tempo, at once.

Two footswitches step through the set. **Footswitches...** learns them -- press
the pedal rather than looking up what it sends. They work whatever is loaded and
whatever input they arrive on, and they say where you are ("2/8 Green Onions")
in the status bar, because a pedal gives no feedback of its own. Stepping stops
at the end rather than wrapping round to the first number.

### On the tablet

The tablet opens on the set list: the current song large, with what each
keyboard is playing and your notes, the rest of the running order compact. Tap
any song to jump to it, or use PREV/NEXT.

Songs that are not in the current set are listed underneath, under **NOT IN THIS
SET** -- a song you have captured but not yet ordered is still one you may want
to play. Tapping one loads it without changing the set or where the footswitches
step from.

With more than eight songs a **Find a song** box appears. It matches the name,
the stage notes, and the programs -- so "piano" or "002" finds a song you cannot
quite name.

## Charts on the stand

A song can carry its sheet music -- a lead sheet, a scan, a typed chord
chart -- and the tablet shows it. Press **Chart...** under the songs list to
attach a file, or drop one onto a song. PDF, PNG, JPEG, GIF and WebP are
accepted, plain text (`.txt`) is shown as it is in a monospace face, and a
ChordPro file (`.cho`, `.crd`, `.chordpro`) has its chords set above the
words. A song may have several; they appear as tabs.

The file is copied, not referred to: it goes into a folder beside the setup
named after it -- `MySetup.stagerig.json` keeps its charts in
`MySetup.stagerig.charts/` -- so the pair can be moved or backed up
together, and **Save As** brings the folder along. Removing a chart from a
song takes it off the song and leaves the file in the folder.

A PDF is rendered to page images when it is attached, so every tablet shows
it the same way and can turn it a page at a time: tap the right third of the
page or swipe left for the next page, the left third or swipe right for the
previous. The rendering uses `pdftoppm` (package `poppler-utils`); where it
is not installed the PDF is handed to the browser as it is, which on some
tablets means scrolling rather than paging.

On the tablet, a song with a chart shows a **Chart** button on its row. It
opens the chart in its own tab, which **follows the set**: when the song
changes -- from a footswitch, the tablet or a program change -- the tab moves
to the new song's chart, and says so when the new song has none. Press
**Follow** to pin it to the song you are looking at instead. A song's
`notes` line is still the place for a key or a count-in; a chart is for what
does not fit on one line.

## Outputs and the master chain

Everything an input makes lands on an **output**: a pair of the audio
interface's channels. A new setup has one, *Main*, on channels 1-2, and that is
all most rigs need. **Options ▸ Outputs & master effects...** is where there
can be more:

- **+ Add** makes an output on the next free pair -- *Output 2* on 3-4, say. If
  the interface has not opened those channels yet, open **Audio...** and tick
  them; the **Device channels** list offers only channels that are open.
- Each input has an **Output** chooser in its editor. Put the lower manual on
  its own output and the desk gets it on its own channels; put a click track's
  input on the in-ear pair.
- Every output has a **master volume** and a **master effects** chain, applied
  to everything that lands on it: a limiter or EQ on the way to the desk. These
  effects are not part of any program, so a program change never restarts
  them.
- **Learn volume CC** waits for the next pedal or knob you move, on any
  keyboard and channel, and makes it that output's volume -- a master fader
  without touching a plugin. A fader taper: half way is -12 dB.

The tablet's **GUI** chooser lists an output's master effects under every input
that lands on it. Removing an output moves its inputs to the first one; the
last output cannot be removed.

## Slots: instruments inside a program

Each slot is one plugin instrument. The controls, left to right:

| Control | What it does |
|---|---|
| **Checkbox** | Enables the slot. Off is silent but stays loaded. |
| **Icon** | The plugin's own icon where it has one, otherwise a coloured badge with its initials. |
| **Name** | Plugin name and format. Red means it failed to load; hover for the reason. |
| **Edit GUI** | Opens the plugin's own editor in its own window. Shows **Reload** instead if the plugin died. |
| **X** | Removes the slot. |
| **GAIN** | Level of this slot, applied after its effects. Double-click for 0 dB. |
| **PAN** | Stereo position, `centre`, `L 40`, `R 25`. Double-click for centre. |
| **TRANSPOSE** | Shifts incoming notes in semitones; `+12 st` is an octave up. |
| **MIDI CHANNEL** | *Keep incoming channel*, or force one. Multi-timbral plugins such as Kontakt play the instrument assigned to that channel. |
| **KEY RANGE** | Two handles: the lowest and highest key that reach this slot. The caption spells the range out. |
| **VELOCITY** | Two handles: the softest and loudest note-on that reach this slot. |
| **VELOCITY CURVE** | Reshapes how hard your playing appears to the plugin, from `100 % softer` through `linear` to `+100 % louder`. |

**+ Add instrument...** adds a slot. The menu is grouped by manufacturer with
each plugin's icon.

## Splits and layers

Key range and velocity range are how a live rig is built.

**A split** — bass below middle C, piano above:

1. Add the bass. Set **KEY RANGE** from the lowest key to `B3`.
2. Add the piano. Set its **KEY RANGE** from `C4` up.

**A layer** — piano everywhere, strings on top:

1. Add both with the full key range. Both sound together.
2. Set the strings' **GAIN** lower so they sit under the piano.

**A velocity switch** — soft playing gives a pad, hard playing gives a horn:

1. Add the pad, set **VELOCITY** from `1` to `80`.
2. Add the horn, set **VELOCITY** from `81` to `127`.

Notes outside a slot's velocity range never reach it; note-offs always pass, so
nothing hangs when you change ranges while holding a chord.

**The velocity curve** fixes the mismatch between a controller's feel and a
library's programming. If a sound needs to be hammered to speak, move the curve
towards *louder*; if it blasts at the slightest touch, move it towards *softer*.
The extremes of your playing are untouched, only the feel in between changes.

## Effects

There are two places for effects:

- **Per slot** — **+ Add effect...** under a slot inserts an effect after that
  instrument. The order is instrument, then the effects top to bottom, then the
  slot's gain and pan.
- **Per program** — the **PROGRAM EFFECTS** section at the bottom processes the
  mix of every slot, which is where a shared reverb or a master compressor goes.

Each effect has a checkbox to bypass it, **^** and **v** to reorder, **Edit GUI**
and **X**. Effects receive the same MIDI as their slot, so tempo-synced and
MIDI-controlled effects work.

## MIDI mappings: knobs and pedals

A mapping sends a controller message to one plugin parameter. Mappings belong
to the program, so the same knob can do different things in different sounds.

**To make one:**

1. Choose the target plugin in the first box (it lists the slots and effects of
   this program).
2. Choose the parameter. The list is searchable: type part of the name and use
   the arrow keys. See the note on Kontakt below.
3. Press **Learn MIDI** and move the control on your keyboard. The source
   appears next to the button.
4. Set **Min** and **Max** if you want to limit the range. Reversing them
   (min above max) inverts the control.
5. Tick **Pass through** to *also* send the original message to the plugin.
6. Press **Add**.

**Use touched parameter** is often faster: open the plugin's GUI, move the knob
you care about, press the button, and StageRig selects that parameter.

**Suggest...** proposes a whole set at once. If you saved a template for that
plugin it offers that; otherwise it matches parameter names against the General
MIDI Level 2 controllers (CC 74 cutoff, 71 resonance, 73/75/70/72 attack, decay,
sustain and release, 76/77 LFO rate and depth, 7 volume, 10 pan, 91 reverb,
93 chorus, 94 detune, 5 portamento, 12 drive, 13 delay mix). Untick anything
you do not want and press **Add selected**. **Save template** stores the current
mappings as that plugin's default for future programs.

**A note on VST3 and MIDI controllers.** VST3 plugins do not receive MIDI
controllers directly. Instead they publish one parameter per controller per MIDI
channel, and the host converts. Kontakt, for example, exposes 4145 parameters,
of which 2096 are these per-channel controller proxies. The parameter picker
therefore lists the controller parameters for the channel your input plays on
first, labelled `(ch 1)` with the controller number, then the plugin's own
parameters, and puts the other channels behind a toggle. To map a knob to a
Kontakt instrument's own control, assign a host-automation slot in Kontakt
(Automation → Host Automation, drag a slot onto the knob) and map to that
`#NNN` parameter, or use *Use touched parameter*.

## Reading the stage display

**Stage** in the toolbar puts a large readout at the top of the window: for each
input, the program number and the program's name, big enough to read from a
stand while you play. When you pick programs by number on the keyboard, this is
your confirmation that the right sound loaded. A program still loading is marked
*...loading*, which explains a keyboard that is briefly silent, and an empty
program is greyed.

## Selecting programs from a tablet

**Tablet** in the toolbar starts a small web app on your network. It lists each
input with its current program in large type and every program you have set up;
tap one to switch to it. There is a PANIC button too. Nothing is installed on
the tablet: it is a web page.

![The tablet set list: the song you are on in large type, the running order, and a search](images/tablet-setlist.png)

Switching to **Programs** shows every program on every keyboard instead:

![The tablet showing the programs of both keyboards, grouped by kind](images/tablet-app.png)

The current program for each input is highlighted, and the big number at the top
of each block is what a keyboard would send as a Program Change. Tapping a
program switches to it immediately, and the page follows along if you change
programs at the laptop instead.

StageRig shows the address and code to get here:

![The Tablet control dialog](images/tablet-control.png)

There are two ways in, and the fast one is the square code. Point the tablet's
camera at it and it opens StageRig directly, with no address to read off a
screen. **Options ▸ Print program map** puts the same code on the printed sheet, so a page taped
to the keyboard is also the way in.

To type it instead, enter the address once, then the six-character code. The
tablet remembers the code, and it does not change when StageRig restarts, so a
home-screen shortcut keeps working. It is case-insensitive and avoids characters
that misread, so `0`/`O` and `1`/`I` never appear.

![Entering the code on the tablet](images/tablet-code.png)

Tablet control stays on across restarts. If it was on when you quit, StageRig
starts the web app again at launch, so a tablet on a stand keeps working without
anyone going back to the laptop. Turning it off is remembered the same way.

A program picked from the tablet is a real change to the setup, exactly like
picking one on the laptop. It is written to the setup file you have open, within
a couple of seconds rather than waiting for the next periodic save, because
someone who changes a program from a music stand has no way to reach Ctrl+S.

Anyone on your network who has the code can change your sounds, which is the
right level of care for a stage tool and no more: do not expect it to be safe on
an untrusted network.

### Showing a plugin's own window

**GUI** at the right of each input's header opens the current program's plugin
interface on the tablet, mirrored from the computer. That is the one to reach
for mid-set: it is level with the program name, so it needs no scrolling.

If the program is a split or a layer with more than one instrument, GUI asks
which one you meant -- an organ over a pad has two interfaces and only you know
which. Programs with no plugins have no button at all.

![IK Multimedia's Hammond B-3X mirrored to the tablet: the real drawbars, the Leslie and the vibrato, drawn by the plugin itself](images/tablet-plugin-gui.png)

That is the plugin itself, not a drawing of it, and it is fully interactive:
pull those drawbars, kick the Leslie from slow to fast, open a menu, load a
different patch.
Input travels back to the plugin, so the tablet works as a second screen and
mouse rather than a remote with a fixed set of buttons -- and a laptop or phone
on the same network does the same, because it is only a web page.

StageRig used to offer faders you picked per plugin and labelled by hand, and
they are gone: what you get now is whatever the plugin shows, with nothing to
set up first.

The reason is that the faders could never be trusted to tell the truth. A
plugin's controls often cannot be followed any other way. The parameters worth
putting on a tablet for a Kontakt library are its MIDI controller inputs: values
go in, and nothing comes back. Asking Kontakt what CC 12 is set to returns
whatever was last written to it, not where the drawbar actually sits, so a fader
on the tablet could drive that organ but never follow it. Mirroring the window
sidesteps that, because what you see is what the plugin is showing -- and it
needs no setting up for each plugin, which matters when a rig has a dozen.

This needs `x11vnc`, `websockify`, `xdotool` and `novnc` installed; StageRig
says so if any are missing.

It works on a Wayland desktop as well as an X11 one. StageRig is an X11
application and plugin editors are X11 windows, so both run through XWayland,
which every Wayland desktop provides and which `x11vnc` can read and write like
any other X display. This is tested: a plugin's own window mirrors to the
tablet under XWayland exactly as it does on X11.

A Wayland session without XWayland is the one case that does not work, because
everything here reads the screen through X. The tools are still installed and
still start, so rather than leave the tablet showing a black rectangle,
StageRig checks that an X display is actually reachable and says what to
install. Install `xwayland` and log back in, or use an X11 session.

The stream travels on the same port as the tablet app, so nothing extra has to
be opened in a firewall, and it stops when StageRig does.

The tab is named after the program, so several open plugin windows are told
apart at a glance, and pressing **GUI** again for the same instrument brings its
existing tab forward rather than opening another.

The page opens ready to play. A touch presses where you put it and holds for
as long as you hold it, so a key on the plugin's own keyboard sounds for as long
as your finger is on it, and sliding across the keys glissandos. The same
applies to a drawbar or a knob: press it and drag.

One finger at a time, though. The mirror carries a single pointer -- it is a
mouse, as far as the plugin knows -- so a second finger cannot be a second
note, and is ignored while the first is down. Chords come from the keyboard.

This is why the page handles touches itself rather than leaving them to noVNC,
which reads them as gestures. A gesture has to be recognised before it can be
acted on, so a tap arrived as a press and a release in the same instant -- a
pop rather than a note -- and only after a wait to rule out a pinch.

Because playing owns the touches, panning and zooming have their own controls:
**+** and **-** zoom a step at a time, and **Fit** scales the whole window onto
the screen. **Move** hands the touches back to the browser so you can pinch and
drag the plugin around, and stops sending them to it; press it again to play.

**Full** next to it hides the browser's address bar, which is worth doing on any
screen: the bar costs a fifth of the height on a small one, and a plugin window
wants all of it. Android and desktop browsers have this. iPhones and iPads do not --
Safari offers no way for a page to go fullscreen -- so on those, add StageRig
to the home screen and open plugin windows from there, which has the same
effect and is described below.

**The plugin window has to be uncovered on the computer.** The tablet's taps
are delivered to the computer's screen as real mouse clicks, and a click goes
to whatever window is on top at that spot. If the browser, StageRig's own
window or anything else overlaps the plugin's editor, the pointer will visibly
move to the right place and the click will land on the wrong window -- which
looks like the plugin ignoring you. StageRig brings the editor to the front
each time you press GUI, but if you then click something else on the computer
and it covers the editor, move it aside. Trying it from the computer's own
browser is the classic case: the browser you are clicking in is exactly what is
covering the plugin.

It is for setting up and adjusting rather than playing. A plugin window on a
tablet is a picture of an interface built for a mouse, so expect to tap
precisely and to miss sometimes.

### If the tablet cannot connect

The most common cause is a firewall on the computer, not anything wrong with
StageRig or the tablet. The symptom is confusing, because the desktop can open
the address itself perfectly well while the tablet just times out: traffic from
the same machine never passes the firewall, so only the tablet is blocked.

StageRig warns about this when it can detect it. To allow the port from your
own networks only, and not from the internet:

```
sudo ufw allow from 192.168.0.0/16 to any port 7777 proto tcp
sudo ufw allow from 10.0.0.0/8 to any port 7777 proto tcp
sudo ufw allow from 172.16.0.0/12 to any port 7777 proto tcp
```

Those three ranges are the private address blocks, so the rule covers your home
wifi, a venue's network and StageRig's own hotspot, while refusing anything
from outside. If you use a different firewall the principle is the same: open
TCP 7777 for private networks.

### Adding it to the home screen

On Android, Chrome offers to install the page once it has loaded, and you can
also use **Add to Home Screen** from its menu. It then opens fullscreen with its
own icon, like an app.

This is worth doing for the screen space alone. An address bar costs a fifth of
the height on a small screen, and the installed shortcut has none -- for the
program list and for the mirrored plugin windows it opens, which is where the
space matters most.

On an iPhone or iPad, use **Share** then **Add to Home Screen** in Safari. iOS
does not offer an automatic prompt for this.

If no automatic prompt appears on Android, the menu item still works. Browsers
only volunteer the prompt on an address they consider secure, which in practice
means `https`, and StageRig serves plain `http` on your own network. That is a
deliberate trade: a certificate for a private address is not something a stage
tool should be demanding of you. The manual route works regardless, and the
installed shortcut behaves identically.

### When the venue has no usable wifi

Guest networks often stop devices from seeing each other, and plenty of stages
have no wifi at all. **Create a wifi network** in the Tablet dialog makes this
computer serve its own network instead, so the tablet joins the laptop directly
and needs nothing from the venue.

![Serving our own wifi network](images/tablet-control-hotspot.png)

The first time, StageRig asks which wifi adapter to use and remembers the
answer, along with the network name and password. There is nothing to set up on
the tablet: it gets an address automatically when it joins, exactly like joining
any other wifi, and it still has internet through whatever this computer is
connected to.

One radio usually cannot be a client and an access point at once, so if this
computer has a single wifi adapter it will leave its current network while the
hotspot runs. That is normally what you want on stage. If you would rather keep
both, a cheap USB wifi adapter gives you a second radio, and StageRig prefers
whichever adapter is free. The adapter list marks which is which, so the choice
is not a guess.

Pick a network name and password you are happy to reuse, because the tablet then
reconnects on its own at the next gig. Use **Stop the wifi network** in the same
dialog to return this computer to its normal network.

### How long loading takes

Plugins load in parallel, as many at once as the machine can usefully manage --
half its hardware threads, between four and twelve. Loading is mostly waiting:
for Wine to start, for a plugin to read its own files. `parallelLoads` in the
settings overrides the number if you want to.

What parallelism cannot shorten is one slow plugin. Hammond B-3X takes about
seven seconds to initialise itself under Wine whatever else is happening, so a
program using it is ready in about seven seconds, and a rig of eight plugins is
ready in about the same -- they are all starting at once.

Closing a plugin's window hides it rather than destroying it, so opening it
again is immediate: a quarter of a second instead of five. Building the editor
is most of what opening a GUI costs, and it is the same editor each time.

## Tempo

Plugins with a tempo-synced delay, tremolo or arpeggiator ask the host what the
tempo is. The toolbar shows it, and **Tap** sets it: tap four times in time and
it follows. One tempo serves the whole rig, because a band plays a song at a
tempo rather than an instrument at one, and it is saved with the setup.

When you already know the tempo, type it: click the number beside **Tap** and it
becomes a text box. Type, press Return, and that is the tempo; Escape leaves it
alone. Anything outside 20 to 300 is pulled to the nearer end rather than
refused, and fractions are kept, so 97.5 stays 97.5.

Right-click **Tap** to set a MIDI controller that taps it. Choose **Learn** and
press the pedal or button you want to use: that is easier than looking up what
your keyboard sends, and it works even if something is already assigned. Typing
a controller number by hand is still there if you prefer it.

A footswitch is the usual choice; most send CC 64 or CC 80. That controller
works on any input and in any program, so it taps the tempo whatever you happen
to be playing, and only the press counts, not the release.

The tablet app shows the tempo and a TAP button in its header, beside PANIC, so
you can set it from the music stand without giving up screen space to it. Tapping there is as accurate as tapping on the laptop:
the timing is measured where the tempo lives rather than on the tablet, and a
local network adds about three milliseconds, far below what a finger can
control.

Taps more than two seconds apart start a fresh count rather than averaging
across the gap, so you can stop, think, and start again without a stale tempo
dragging the result. Only the last eight taps count, so speeding up is followed
rather than averaged away.

StageRig has no transport: there is nothing to start or stop. Plugins are told
the music is always running, which is what a live rig looks like from inside a
plugin. If a plugin's delay still does not sync, it may simply never ask the
host; running StageRig with `STAGERIG_REPORT_TEMPO=1` prints whether each
plugin asked and what it was told.

## Printing a program map

**Options ▸ Print program map** writes a sheet of which program number plays which sound, and
opens it in your browser; print it from there. Only programs you have filled in
are listed, with each program's number, its name and the instruments in it,
grouped by input with the MIDI device and channel. Tape it to the keyboard and
you never have to remember that the Rhodes is 001.

If tablet control is running when you print, the sheet also carries the square
code from the Tablet dialog, so the page on the keyboard doubles as the way onto
the tablet selector.

![The code as printed](images/qr-example.png)

## The on-screen keyboard

The **Keyboard** button opens a keyboard at the bottom of the window with
velocity, sustain, mod wheel, pitch bend and a free CC number and value. It
plays into the selected input on that input's channel, through exactly the same
path as a real keyboard, so it works for auditioning sounds, checking splits and
doing MIDI learn without a controller connected.

Click the keys with the mouse, or focus the keyboard and use the computer
keyboard: `A S D F G H J K` for white keys, `W E T Y U` for sharps, `Z` and `X`
to shift octave.

## Audio settings and latency

**Audio...** has two parts. The top is StageRig's own **Live latency** section:

- **PipeWire quantum** — the block size to ask for. 128 samples is 2.7 ms at
  48 kHz and a good live default; 64 if the machine is quiet and has realtime
  scheduling.
- **Take over the PipeWire clock while StageRig runs** — forces the graph to
  that block size at start and restores the previous value on quit.
- A status line shows the device, the block size actually delivered, the
  resulting latency, whether realtime scheduling is available, and warnings.

The lower part is the device chooser: output device and channels, and -- for
audio inputs -- the input device and which of its channels to open. Only open
channels are offered to an audio input.

Below that is the standard device selector: device type, output, sample rate
and buffer size.

**For the lowest latency:**

- Use the **JACK** device type. On PipeWire this makes StageRig a native node
  in the graph, with no resampling and no extra buffering. The ALSA type goes
  through PipeWire's ALSA emulation.
- Give yourself **realtime scheduling**. Run `ulimit -r`; if it prints 0, add
  yourself to the `pipewire` group with `sudo usermod -aG pipewire $USER` and
  log out and back in. Small blocks without realtime priority cause dropouts.
- **Close other audio applications.** A DAW such as Bitwig forces the PipeWire
  clock to its own block size every few seconds; StageRig then shows *clock
  held by another app* in the status bar and cannot get its own block size.

**A USB interface on the ALSA type.** An interface PipeWire does not manage
(the Audio window offers it under the ALSA type) is driven by StageRig
directly, and StageRig runs its audio thread at realtime priority for it. Two
settings are easy to pick there and sound like a broken cable, so the status
line and the status bar warn about them: a buffer below 128 samples (a
full-speed USB interface cannot keep up with one USB packet per block; 256 is
safe, 128 often works), and an input device different from the output device
(two clocks, which drift and crackle -- use the same interface for both, or
untick the input channels).

*late N* in the status bar counts blocks where a plugin did not answer in time.
A few during loading are normal. A number that climbs while you play means the
block size is too small for the plugins in that program.

## Saving, autosave and plugin state

A setup is one `.stagerig.json` file holding every input, program, plugin and
mapping, including each plugin's own state.

- **Save** / **Save As...** write the file. The last file reopens next start.
  New saves use `.stagerig.json`; a `.performer.json` from before the rename
  opens as before and keeps its name.
- Plugin state is captured whenever you leave a program, close a plugin's
  window, unload a program or save, so tweaks made inside a plugin GUI
  persist.
- StageRig autosaves about once a minute to
  `~/.config/StageRig/autosave.stagerig.json` and again on quit. If that file
  is newer than the setup you open, the status bar tells you. The autosave
  asks only the plugins whose state may have changed since it was last read
  -- asking a plugin for its state can stall its audio for a moment, and
  asking twenty at once was audible -- so a quiet minute of playing costs
  nothing. Save and quit ask every plugin, so press Save between songs.

A setup file is plain JSON and safe to keep in version control.

## Playing live

A short checklist:

- Turn on **Options ▸ Preload all programs** and give StageRig time to load everything
  before the first song. The status bar stops showing loading activity when it
  is done.
- Check that the status bar shows the block size you expect and no *late*
  count.
- Leave **Hold notes across program changes** on and set the **Tail** long
  enough that program changes do not cut your release.
- Learn where **PANIC** is.
- Keep the set on one input per manual, with program numbers matching the
  patch numbers on your keyboard, so you never touch the computer.

## Plugin formats

StageRig hosts **VST3**, **CLAP**, **LV2** and **LADSPA** natively. Windows
VST3s and CLAPs bridged with yabridge appear as ordinary plugins of that format.

**CLAP** plugins are scanned from `~/.clap`, `/usr/lib/clap` and
`/usr/local/lib/clap`, and load like any other format: parameters appear in the
slot, MIDI reaches the plugin, audio comes back, the plugin's own window opens,
and its state is saved with your setup. Both native Linux CLAP plugins and
Windows ones bridged by yabridge work.

JUCE itself has no CLAP support, so StageRig talks to the CLAP API directly.
One thing follows from that: a `.clap` bundle can hold several plugins -- Surge
XT ships the synth and its effects rack in one file -- so each entry in the
Plugins window is one plugin from a bundle, not one file.

Stepped parameters -- a waveform selector, say -- snap to their nearest step, so
the value a plugin reports back may differ slightly from what was sent while
naming the same setting. That is normal and not a sign of a lost setting.

## Windows plugins

Windows VST3 and CLAP plugins bridged with [yabridge](https://github.com/robbert-vdh/yabridge)
appear as ordinary plugins of that format. Scan the folders yabridge puts them
in, usually `~/.vst3/yabridge` for VST3s and `~/.clap/yabridge` for CLAPs.

Setting the Wine side up by hand is the fiddly part, and
[vstenv](https://github.com/dguedry/vstenv) exists to do it for you: it
provisions its own Wine, installs the vendor manager applications (Native
Access, IK Product Manager, Steinberg Download Assistant, Audio Modeling
Software Center) and the products they deliver, and bridges everything into
`~/.vst3/yabridge` for StageRig to scan. Nothing in StageRig depends on it --
any yabridge setup works -- but it saves a lot of tedium.

A prefix vstenv manages records which Wine belongs to it, and yabridge is taught
to read that record, so the right Wine runs the plugin whatever started it. That
matters because the wrong Wine version would try to update the prefix and
corrupt it. StageRig needs to do nothing for this to work.

For a prefix set up by hand, StageRig still follows the convention that relies on the session: it puts `~/.local/bin`
first on a helper's PATH when a `wine` shim is there, and applies `WINELOADER`
and `WINEFSYNC` from `~/.config/environment.d/*.conf`, since a desktop launcher
and a terminal do not always agree about those.

While a vendor's installer replaces a plugin, the bridged file is briefly
missing. StageRig reports *yabridge link points to a missing file* rather than
blacklisting the plugin; scan again once the install has finished.

## When something goes wrong

| What you see | What it means | What to do |
|---|---|---|
| A slot is red, with **Reload** instead of Edit GUI | That plugin's process died. Only that plugin is affected. | Press **Reload**; it restarts with the saved state. |
| A plugin shows *Deactivated* in the Plugins window | It hung or crashed twice while being scanned. | Select it and press **Remove selected** to give it another chance. |
| *late N* climbing while playing | Plugins are missing the block deadline. | Raise the block size in **Audio...**, or check realtime scheduling. |
| *clock held by another app* | Another audio application forces the PipeWire block size. | Close it, or set its block size to match. |
| Static or crackle on a USB interface under the ALSA type | The buffer is below 128 samples, or the input is on a different device from the output. | Set 256 (or 128) in **Audio...**, and the same interface for input and output; the status line says which it is. |
| A few seconds of static about once a minute | A plugin stalling its audio while its state is read for the autosave. | Fixed in 0.1.56: only changed plugins are asked. Press **Save** between songs rather than mid-song. |
| No sound, plugin loaded | Slot disabled, key or velocity range excludes what you play, gain at minimum, or wrong MIDI channel. | Check the slot's controls; the on-screen keyboard rules out the controller. |
| A note hangs | A plugin missed a note-off. | Press **PANIC**. |
| The parameter list seems endless | A VST3 with thousands of per-channel controller parameters. | Type in the search box; see the mapping section above. |
| An empty plugin editor that will not respond | An ARA-only plugin outside an ARA host. | Use the plugin's non-ARA version. |

## Files StageRig uses

| Path | What it is |
|---|---|
| `~/.config/StageRig/StageRig.settings` | Window, audio device, scan folders, known plugins, tuning options. |
| `~/.config/StageRig/autosave.stagerig.json` | The rolling autosave. |
| `~/.config/StageRig/mapping-templates.json` | Your saved per-plugin mapping templates. |
| `~/.config/StageRig/icons/` | Cached plugin icons. |
| `~/.config/StageRig/RecentlyCrashedPluginsList` | Plugins that crashed while scanning. |
| your `*.stagerig.json` | Your set. Keep it wherever you like. |

Advanced options can be set in `StageRig.settings`: `parallelLoads` (how many
plugins load at once, default 4), `parallelBridgedLoads`, `bridgedColdStartSolo`,
`pipewireQuantum` and `pipewireTakeover`.

## Keyboard and mouse reference

| Action | Where |
|---|---|
| Double-click a gain, pan, transpose or curve bar | Resets it to its neutral value. |
| Drag a range slider's handles | Sets the low and high ends of a key or velocity range. |
| Type in the parameter picker | Filters thousands of parameters; arrows and Page keys move, Return picks, Escape cancels. |
| `A S D F G H J K` / `W E T Y U` | White and black keys on the on-screen keyboard. |
| `Z` / `X` | Shift the on-screen keyboard down or up an octave. |
| Escape | Closes a dialog. |
