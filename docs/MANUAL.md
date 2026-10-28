# StageRig user manual

StageRig is a live-performance plugin host for Linux: it runs your instrument
and effect plugins on stage. Your keyboards play instruments; a microphone, a
guitar or a hardware synth plugged into the audio interface runs through
effects. You build a program for each sound you need, and change sounds from
the keyboard, a footswitch or a tablet while you play.

This manual covers everyday use. For building from source and how the program
works inside, see the [README](../README.md).

- [The idea in one minute](#the-idea-in-one-minute)
- [First run](#first-run)
- [The main window](#the-main-window)
- [Inputs: keyboards, and audio](#inputs-keyboards-and-audio)
  - [Audio inputs](#audio-inputs)
- [Programs: your sounds](#programs-your-sounds)
  - [Grouping programs](#grouping-programs)
- [Instruments inside a program](#instruments-inside-a-program)
  - [Routing by channel](#routing-by-channel)
  - [MIDI devices](#midi-devices)
  - [An instrument's MIDI chain](#an-instruments-midi-chain)
- [Splits and layers](#splits-and-layers)
- [Effects](#effects)
- [MIDI mappings: knobs and pedals](#midi-mappings-knobs-and-pedals)
- [Outputs and master effects](#outputs-and-master-effects)
- [Songs and set lists](#songs-and-set-lists)
  - [Making a song](#making-a-song)
  - [Making a set list](#making-a-set-list)
  - [Playing a set list](#playing-a-set-list)
- [Charts on the stand](#charts-on-the-stand)
- [Backing tracks](#backing-tracks)
  - [Audio files](#audio-files)
  - [MIDI files](#midi-files)
  - [The Backing track input's program](#the-backing-track-inputs-program)
  - [Sections and vamps](#sections-and-vamps)
  - [Playing it](#playing-it)
- [Reading the stage display](#reading-the-stage-display)
- [The tablet](#the-tablet)
  - [Set list](#set-list)
  - [Programs](#programs)
  - [Showing a plugin's own window](#showing-a-plugins-own-window)
  - [If the tablet cannot connect](#if-the-tablet-cannot-connect)
  - [When the venue has no usable wifi](#when-the-venue-has-no-usable-wifi)
  - [Adding it to the home screen](#adding-it-to-the-home-screen)
- [Tempo](#tempo)
- [Printing the program list](#printing-the-program-list)
- [The on-screen keyboard](#the-on-screen-keyboard)
- [Audio settings and latency](#audio-settings-and-latency)
- [Saving your setup](#saving-your-setup)
- [Playing live](#playing-live)
- [How long loading takes](#how-long-loading-takes)
- [Plugin formats](#plugin-formats)
- [Good free plugins to start with](#good-free-plugins-to-start-with)
- [Windows plugins](#windows-plugins)
- [When something goes wrong](#when-something-goes-wrong)
- [Files StageRig uses](#files-stagerig-uses)
- [Keyboard and mouse reference](#keyboard-and-mouse-reference)
- [Appendix: If you know Ableton Live](#appendix-if-you-know-ableton-live)

## The idea in one minute

An **input** is where notes or sound come from, and there are three kinds:

- a **keyboard** -- a MIDI device, or one MIDI channel of it -- which plays
  instruments;
- an **audio input** -- one or two of the audio interface's input channels: a
  microphone, a guitar, a hardware synth -- which runs through effects;
- the **Backing track** -- the songs' MIDI files, which play instruments
  exactly as a keyboard would. It appears when you add the first MIDI file.

Each input holds **128 programs**. On a keyboard, a program is one sound of
your show: one or more **instruments**, each with its own key range, velocity
range, transpose, gain, pan and effects. An instrument is a plugin, a MIDI
device (a hardware synth, a lighting controller), the built-in General MIDI
player, or another keyboard's program. On an audio input, a program is a row
of **effects** with an input trim -- the clean vocal, the vocal with the long
reverb, the vocoder.

You change programs with the keyboard's own patch buttons (a MIDI Program
Change), a footswitch, a click on the laptop, or a tap on the tablet. Every
input changes independently, so the left hand can stay on a bass while the
right hand moves through leads, and the vocal can go to its ballad reverb
without touching either. A **song** selects every input's program at once,
with its tempo, and can bring a backing track; a **set list** is the songs in
order.

```
Keyboard  ──> Input "Upper" (ch 1)  ──> Program 007 "Solo"    ──> Lead synth   C4-C8
                                                              └─> String pad   C4-C8, soft only
Keyboard  ──> Input "Lower" (ch 2)  ──> Program 002 "Bass"    ──> Electric bass C0-B3
Mic       ──> Input "Vocal" (in 1)  ──> Program 004 "Ballad"  ──> EQ ──> Plate reverb
MIDI file ──> Input "Backing track" ──> Program "Oye Como Va" ──> General MIDI player (drums, bass)
                                                              └─> Upper: Big Strings (the strings part, channel 5)
```

Every input lands on an **output** -- a pair of the interface's output
channels -- with its own master effects and volume, so the keyboards and the
vocal can go to the desk on separate channels.

Everything here -- inputs, programs, songs, set lists, and each plugin's own
settings -- is one file, the **setup** (`*.stagerig.json`). **Save As...**
names it.

If one sentence has to do: *a program is the sound a keyboard is on; a song is
which program every keyboard is on; a set list is the songs in order.* If you
know Ableton Live, the [appendix](#appendix-if-you-know-ableton-live) maps its
words onto these.

## First run

1. Start StageRig. It opens with two empty inputs, **Upper** and **Lower**,
   and the songs panel on the left.
2. Open **Audio...** and choose your output. StageRig defaults to the JACK
   device type at 48 kHz. On most current Linux desktops the sound system is
   PipeWire, which speaks JACK, and that is the lowest-latency way in. See
   [Audio settings and latency](#audio-settings-and-latency).
3. Open **Plugins...** and press **Scan VST3**, **Scan LV2**, **Scan LADSPA**
   or **Scan CLAP** -- these are the plugin formats Linux plugins come in; a
   plugin's download page says which it ships. **Scan folder...** looks in a
   folder of your own. The scan runs in the background; you can close the
   window and keep working, and a plugin that crashes while being looked at
   cannot take the scan down with it. No plugins yet? See
   [Good free plugins to start with](#good-free-plugins-to-start-with).
4. Select **Upper** on the left, choose its **MIDI device** and **Channel**.
5. Click program **000** in the middle list, name it, and press
   **+ Add instrument... ▸ Plugin instrument**.
6. Play. Press **Save As...** when the setup is worth keeping.

**Help** in the toolbar opens this manual inside StageRig, so the answer to
"why won't the tablet connect" is reachable from the stage without finding a
browser. The list on the left jumps to a section, and the search box narrows to
whichever section answers what you typed.

## The main window

| Area | What it does |
|---|---|
| **Toolbar** (top) | **New**, **Open...**, **Save**, **Save As...**, **Tablet**, **Help**, **Audio...**, **Plugins...**, **Options**, the tempo and **Tap**, **Tail** with its seconds box, the **Songs**, **Stage** and **Keyboard** toggles, **PANIC**. |
| **Songs** (far left, on by default) | Every song on top, the running order of the chosen set list below, and the backing track's **Play**. |
| **Inputs** (left) | Your keyboards, audio inputs, and the Backing track. Select one to edit its programs; its name, device, channel and Program Change settings are below the list. |
| **Programs** (middle) | The 128 programs of the selected input. Clicking one activates it. The name box, the group box, **Copy**, **Paste** and **Clear** are below. |
| **Instruments** (right, top) | The instruments of the current program, each with its controls, MIDI chain and effects, then **PROGRAM EFFECTS**. |
| **MIDI mappings** (right, bottom) | Knobs and pedals mapped to plugin parameters in this program, with Learn and Suggest. |
| **Keyboard** (bottom, optional) | An on-screen keyboard for testing without a controller. |
| **Status bar** | Current file, CPU, sample rate, buffer size and latency, and warnings such as *N dropouts* or *clock held by another app*. |

**New** starts an empty setup, and asks first when the current one has unsaved
changes. **Open...** opens a setup file.

**Options** holds the things set once: **Preload all programs (instant
changes; uses more memory)**, **Let held notes ring through a program
change**, **Show songs and set lists**, **Outputs & master effects...**,
**Rescan MIDI devices**, **Print the program list (for the stand)...** and
**General MIDI sounds (soundfont file)...**. Each is explained where it
matters below.

**Tail** is how many seconds the previous program keeps sounding after a
program change, so releases and reverbs fade instead of being cut. Set it in
the box beside the word; four seconds is a good default.

**PANIC** sends all-notes-off to every instrument and MIDI device. Use it if a
note hangs.

The panes are separated by thin dividers (three dots in the gap). Drag one to
give a pane more room, and the widths are remembered. A narrow window squeezes
the panes for the time being and spreads them out again as it grows.

## Inputs: keyboards, and audio

An input is a keyboard, usually. **+ Add** under the list offers **Keyboard
(MIDI)**, **Audio input (microphone, guitar, hardware synth)** and **Backing
track (the songs' MIDI files)**.

An audio input's programs are effects -- there is no instrument to load, the
signal is the instrument -- and in every other way it is an input like the
others: it has 128 programs, it changes them from a footswitch, a song or the
tablet, its effects take MIDI mappings, and a program change leaves the
outgoing effects to ring out for the tail. See *Audio inputs* below.

The **Backing track** is the input the songs' MIDI files play into. It
appears by itself when the first MIDI file is added to a song, and each song
has its own program there, made for it by StageRig: the instruments in that
program are what the file's parts play -- see *MIDI files* under *Backing
tracks*. It has no device or channel of its own.

Each keyboard input has, below the list:

- **Name** -- what the input is called everywhere: in the songs, on the
  tablet, on the printed list. Type in the box.
- **MIDI device** -- any MIDI device the system can see (USB or DIN).
  **Options ▸ Rescan MIDI devices** picks up a keyboard you plugged in after
  starting. If a saved device is missing, the list shows it as
  *(disconnected)* and reconnects when it returns.
- **Channel** -- **All channels** takes everything the device sends, or pick
  one. Two inputs can share one physical keyboard by taking different
  channels, which is how a single controller drives both a left-hand and a
  right-hand sound.
- **Respond to Program Change** -- untick for an input whose sounds you want to
  change only by hand.
- **Arrive on** -- which channel this input's Program Change messages arrive
  on. *Same as notes* is the default and is right for most keyboards. Some
  workstations send program changes on a fixed channel instead of the one they
  play on: a Roland Jupiter-50 sends its registration changes on channel 16
  while its parts play on 1, 3 and 4, so **Arrive on** *Ch 16* lets its
  front-panel buttons select StageRig programs. *Any channel* accepts program
  changes from anywhere on that MIDI port: the setting to try when they are
  not getting through and you do not know what the keyboard sends.
- **Output** -- which output this input's sound goes to. See *Outputs and
  master effects*.

**- Remove** takes an input and all its programs away, and asks first. There
is no fixed limit of two; add an input for a pedalboard, a second tier, or a
drum pad.

### Audio inputs

An audio input has, in place of the instruments:

- **Audio channels** -- which of the interface's inputs it listens to: one
  channel (mono, sent to both sides) or a pair. The list offers the channels
  that are open in **Audio...**; if it says no input is open, choose an input
  device and tick its channels there first.
- **Control MIDI device**, **Channel** and the Program Change settings work as
  for a keyboard. Program changes step through the programs the way they step
  through sounds on a keyboard; controllers drive mapped effect parameters, the
  tap pedal, the song footswitches and an output's volume. Notes are passed to
  the effects too -- a vocoder or harmoniser wants them -- though there is no
  instrument here to play them.
- Each program has an **INPUT TRIM** (in the header of the effects panel) and
  its effects, added with **+ Add effect**. A program with no effects passes
  the signal straight through at its trim.
- A **level meter** shows what is arriving, before the trim: in the input's
  row, beside the trim, on the stage display and on the tablet. Green to
  -12 dB, amber to -3, red above, and a mark that stays lit for two seconds
  after the signal touches full scale. Set the interface's gain so the loudest
  you play stays out of the red; the trim is for balancing programs, not for
  rescuing a hot input.

The tablet's **GUI** button opens an audio input's effects the way it opens a
keyboard's instruments.

## Programs: your sounds

The middle column is the 128 programs, numbered the way your keyboard's
Program Change messages are (000-127), and the same numbers appear everywhere
StageRig names a program. A program in **bold with a plugin count** has
something in it.

- **Click** a program to make it current and edit it.
- **Name** it in the box below the list. Names appear in the list and make a
  set list readable from a distance.
- **Copy** / **Paste** duplicate a whole program, including plugin state, to
  another number. This is the fast way to make a variation of a sound. To move
  a program to another number: **Copy**, click the new number, **Paste**, then
  **Clear** the old one.
- **Clear** empties a program, and asks first.

**Options ▸ Preload all programs (instant changes; uses more memory)** loads
every non-empty program at start and keeps it loaded, so program changes are
instant. Without it, a program loads when you select it, which takes as long
as the plugin needs (Kontakt is several seconds). Preloading costs RAM: each
plugin instance is a full copy.

**Options ▸ Let held notes ring through a program change** (on by default)
means a program change leaves whatever you are holding to the previous
program: keys still down and the sustain pedal keep sounding there, following
the wheels and pedals, until you let go. New notes go to the new program from
the moment of the change. Turn it off for a clean cut at the change --
percussive sounds, or a switch you want to land exactly on the beat.

**Tail** (in the toolbar) is how long the outgoing program keeps sounding after
that -- after you let go with the option on, after the change with it off.

### Grouping programs

Each program can carry a group: **Organs**, **Strings**, **Brass**, whatever
suits your show. Type it in the box under the program name, or press the small
button beside it to pick a group already in use on that input.

A group is a label, not a folder: a program's number is fixed, because that is
what your keyboard sends, so grouping never moves or renumbers anything. It
makes 128 programs findable: the tablet's Programs tab shows a row of group
buttons to filter by, and the printed list prints each group as its own
section. Groups belong to an input, so both keyboards can have their own
"Keys". Programs with no group are never hidden: the tablet lists them under
**Ungrouped**, the printed list under *Other*.

## Instruments inside a program

Each instrument is one lane of the program (a slot, in plugin-host terms), and
the notes go through it in this order:

```
the instrument's notes -> [ MIDI chain ] -> the sound -> [ effects ] -> gain, pan
```

- **The sound** is a plugin instrument; a MIDI device -- a hardware synth or
  module, a lighting controller -- played through a MIDI port (see *MIDI
  devices*); the built-in **General MIDI player**; or **another keyboard's
  program**, which hands the notes to one of that input's programs by name,
  or to whatever it is playing.
- **The MIDI chain** works on the notes before the sound hears them: MIDI
  effects (an arpeggiator, a chord generator), and MIDI devices that should
  play along. See *An instrument's MIDI chain* below.
- **The effects** work on the sound -- see *Effects*.

The instrument's own controls come first: its key and velocity ranges,
transpose, curve and channels decide which notes enter at all. Left to right:

| Control | What it does |
|---|---|
| **Checkbox** | Turns the instrument on. Off is silent but stays loaded. |
| **Icon** | The plugin's own icon where it has one, otherwise a coloured badge with its initials. |
| **Name** | Plugin name and format. Red means it failed to load; hover for the reason. |
| **Window** | Opens the plugin's own window. Shows **Reload** instead if the plugin crashed; a MIDI device has **Device...** here, the General MIDI player **Sound...**. |
| **X** | Removes the instrument, and asks first. |
| **GAIN** | Level of this instrument, applied after its effects. Double-click for 0 dB. |
| **PAN** | Stereo position, `centre`, `L 40`, `R 25`. Double-click for centre. |
| **TRANSPOSE** | Shifts this instrument's incoming notes in semitones; `+12 st` is an octave up. (A song's **Transpose** is a different thing: it shifts every keyboard for that number. See *Making a song*.) |
| **FROM CHANNEL** | Which MIDI channels this instrument takes notes from: *All channels*, normally, or any set of them. With the input on *All channels*, one program can give each channel its own sound -- see *Routing by channel* below. |
| **TO CHANNEL** | Which channel the instrument itself is on: *Same channel* as the notes came in on, normally. Set one for a hardware synth, or a multitimbral plugin such as Kontakt with a sound per channel. |
| **KEY RANGE** | Two handles: the lowest and highest key that reach this instrument. The caption spells the range out. |
| **VELOCITY** | Two handles: the softest and loudest note-on that reach this instrument. |
| **VELOCITY CURVE** | Reshapes how hard your playing appears to the plugin, from `100 % softer` through `linear` to `+100 % louder`. |

**+ Add instrument...** asks which kind:

- **Plugin instrument** -- your instrument plugins, grouped by manufacturer.
- **MIDI device (hardware synth, module, lights)** -- the MIDI ports of the
  interfaces plugged in. With none connected it says so; plug one in and use
  **Options ▸ Rescan MIDI devices**.
- **General MIDI player (built in)** -- the standard 128 sounds and drum kits.
  **Sound...** on the instrument picks the one it plays -- a piano, strings, a
  kit on channel 10 -- or leaves it *as the notes say*, which lets program
  changes in the notes choose: what a MIDI file's parts do (see *Backing
  tracks*). A quick sound with nothing to install.
- **Another keyboard's program** -- the notes go to one of that input's
  programs by name (*Upper: Big Strings*), kept loaded and playing beside
  whatever Upper itself is on; or to **whatever that keyboard is playing**.
  A layer from one keyboard on the other, or a MIDI file part.

**+ Add...** under an instrument adds to it:

- **Effect on the sound** -- reverb, EQ, an amp: after the instrument.
- **MIDI effect on the notes** -- an arpeggiator, a chord generator: before it.
  Three are built in, under **Built in** at the top of that list, with
  nothing to install and nothing to open: **Arpeggiator** plays the notes you
  hold one at a time, in time with the rig's tempo (rate, pattern, octaves,
  note length and latch under **Settings...**); **Chord** makes every note a
  chord of the kind you pick; **Scale** moves any note off the scale down to
  the nearest note on it, so a wrong note is never wrong; **Run** turns each
  key into a scale run, as a string section plays one: press the third and
  the notes climb the scale from it, in time, and the last one sits there
  while the key is held. A tap plays the whole run too: let go early and it
  still lands, the last note lifting a step later. Its settings are the key
  and scale; **Length** (an
  octave, two octaves, or any count of notes); **Takes**, how long the whole
  run lasts at the song's tempo, from a quarter of a beat to a bar, with the
  last note landing exactly at the end, so *an octave in a beat* is two
  picks; **Starts on**, the key pressed or the 2nd to 7th above it in the
  scale, so pressing C can run from E; and whether it runs **up** or
  **down**, or **to the key** (begins below and lands on the note you
  pressed). A chord pressed is a run in every voice, harmonised in the
  scale. Each shows its settings in words on its row.
- **Send to a MIDI device too** -- the instrument's notes go out of a MIDI port
  as well, so a hardware synth doubles the plugin, or a lighting controller
  follows the keys.

### Routing by channel

A program's instruments hear everything their input lets through, unless an
instrument takes notes **from** some channels only. Set the input's channel to
*All channels* and give each instrument its channel, and one program sorts the
parts by itself:

| Instrument | From channel | Plays |
|---|---|---|
| Electric bass | Channel 2 | the left-hand zone of a keyboard that splits by channel, or a MIDI file's bass part |
| Pad | Channel 1 | the right hand, or the file's chords |

That is how a keyboard which sends its zones on different channels, or a
multi-part MIDI file on a backing track, plays several sounds from one
program -- no extra inputs needed. To hear one of them from the on-screen
keyboard, click its row: the keys move to its channel.

### MIDI devices

A MIDI device is where a keyboard's notes come from; it can also be where an
instrument's notes go. An instrument can play a hardware synth, a sound
module, a lighting controller or another computer instead of a plugin: **+ Add
instrument... ▸ MIDI device**, then the port. Everything about an instrument
still applies -- key and velocity ranges, transpose, curves, listening to a
channel, holding notes across a program change -- and the notes leave through
the port, each timed to when StageRig's own audio is heard. **PANIC** sends
notes-off to it too. **Device...** on the instrument holds what is particular
to it:

- **MIDI port** -- which port, if it changes (a different USB socket, say).
  The instrument's row turns red while the port is missing and comes back by
  itself after **Options ▸ Rescan MIDI devices**.
- **Patch recall...** -- bank select (MSB, LSB) and a program number, sent to
  the instrument when the program is selected, on the channel the instrument
  plays on. So a program called *Montage Pad* also switches the Montage to its
  pad. Leave a box empty to send nothing.
- **Audio comes back on** -- the interface inputs the instrument's outputs are
  plugged into. Then its sound joins the program: its effects, gain, pan and
  output, like a plugin. *None* leaves it to go to the desk on its own.

### An instrument's MIDI chain

The blocks between the instrument's notes and its sound, shown above its
effects with a coloured line. Add them with the instrument's **+ Add...**:

- **MIDI effect on the notes** -- any plugin, run for the notes it plays: an
  arpeggiator, a chord generator, a harmoniser, a MIDI filter. Its output is
  what the next block hears; **Window** opens it like any plugin, and its
  parameters can be mapped to knobs. Every plugin is offered, because an
  arpeggiator is filed as an instrument as often as an effect; one that
  plays no MIDI simply passes nothing on.
- **Send to a MIDI device too** -- a copy of the notes at that point goes
  out of a MIDI port, and they carry on unchanged. **Device...** chooses the
  port, the channel it **sends on** (or *Same channel*), and a **patch
  recall** sent when the program is selected. So one keyboard can play a
  plugin piano and double it on a hardware synth; with the send after an
  arpeggiator, the synth gets the arpeggio.

The blocks run top to bottom; **^** and **v** reorder them, the checkbox
bypasses one (the notes go past it), **X** removes it. A MIDI effect that is
still loading, or has crashed, is passed by the same way. **PANIC** reaches
everything after a MIDI effect even if the effect does not pass it on.

## Splits and layers

Key range and velocity range are how a live rig is built.

**A split** -- bass below middle C, piano above:

1. Add the bass. Set **KEY RANGE** from the lowest key to `B3`.
2. Add the piano. Set its **KEY RANGE** from `C4` up.

**A layer** -- piano everywhere, strings on top:

1. Add both with the full key range. Both sound together.
2. Set the strings' **GAIN** lower so they sit under the piano.

**A velocity switch** -- soft playing gives a pad, hard playing gives a horn:

1. Add the pad, set **VELOCITY** from `1` to `80`.
2. Add the horn, set **VELOCITY** from `81` to `127`.

Notes outside an instrument's velocity range never reach it; note-offs always
pass, so nothing hangs when you change ranges while holding a chord.

**The velocity curve** fixes the mismatch between a controller's feel and a
library's programming. If a sound needs to be hammered to speak, move the curve
towards *louder*; if it blasts at the slightest touch, move it towards *softer*.
The extremes of your playing are untouched, only the feel in between changes.

## Effects

There are two places for effects:

- **Per instrument** -- **+ Add... ▸ Effect on the sound** under an instrument
  puts an effect after its sound. The order is the instrument, then its effects
  top to bottom, then its gain and pan.
- **Per program** -- **PROGRAM EFFECTS** at the bottom works on the mix of
  every instrument, which is where a shared reverb or a master compressor goes.

Each effect has a checkbox to bypass it, **^** and **v** to reorder, **Window**
and **X**. Effects receive the same MIDI as their instrument, so tempo-synced
and MIDI-controlled effects work.

## MIDI mappings: knobs and pedals

A mapping sends a controller message to one plugin parameter. A CC is a MIDI
controller number: what a pedal or knob sends, so "controller 74" means CC 74.
Mappings belong to the program, so the same knob can do different things in
different sounds. The table's columns are **Controller**, **Instrument /
effect**, **Parameter**, **Min**, **Max** and **Pass on**.

**To make one:**

1. Choose the target in the first box (it lists the instruments and effects of
   this program).
2. Choose the parameter. The list is searchable: type part of the name and use
   the arrow keys. See the note on Kontakt below.
3. Press **Learn MIDI** and move the control on your keyboard. The source
   appears next to the button.
4. Set **Min** and **Max** if you want to limit the range. Reversing them
   (min above max) inverts the control.
5. Tick **Pass on** to *also* send the original message to the plugin.
6. Press **Add**.

**Use last moved** is often faster: open the plugin's own window, move the knob
you care about, press the button, and StageRig selects that parameter.

**A pedal for a layer or an effect.** The parameter list begins with the
thing itself, before any plugin parameter: **On / off (pedal down = on)**,
**On / off, toggled by each press** and, for an instrument, **Level**. Map a
footswitch to the strings layer's *toggled* entry and each press brings the
strings in or takes them out; map an expression pedal to its *Level* and the
pedal is its fader. The same works for every effect, so a pedal can be the
delay's bypass. No plugin is needed, so a hardware synth or the General MIDI
player can be switched the same way.

**One knob, several things.** A controller may appear in as many rows as you
like, each with its own target and range: one knob that opens the filter on
the pad while it closes the one on the bass (reverse Min and Max on the
second row) is two rows with the same controller. That is a macro.

To change a mapping, select it in the table, change the boxes and press
**Update**; **Remove** deletes the selected one.

**Suggest...** proposes a whole set at once. If you saved a template for that
plugin it offers that; otherwise it matches parameter names against the General
MIDI Level 2 controllers (74 cutoff, 71 resonance, 73/75/70/72 attack, decay,
sustain and release, 76/77 LFO rate and depth, 7 volume, 10 pan, 91 reverb,
93 chorus, 94 detune, 5 portamento, 12 drive, 13 delay mix). Untick anything
you do not want and press **Add selected**. **Save template** stores the current
mappings as that plugin's default for future programs.

**A note on Kontakt and plugins like it.** Some plugins list hundreds of
`(ch 1) CC nn` entries first. Those are the MIDI controllers the plugin
listens to, one per channel; the picker shows the channel your input plays on
first and puts the other channels behind a toggle. Mapping one of them is the
same as sending that controller to the plugin. For a knob the plugin does not
list, use **Use last moved**, or Kontakt's own host automation (Automation ▸
Host Automation, drag one of Kontakt's own host-automation slots onto the
knob) and map to that `#NNN` parameter.

## Outputs and master effects

Everything an input makes lands on an **output**: a pair of the audio
interface's channels. A new setup has one, *Main*, on channels 1-2, and that is
all most rigs need. **Options ▸ Outputs & master effects...** is where there
can be more:

- **+ Add** makes an output on the next free pair -- *Output 2* on 3-4, say. If
  the interface has not opened those channels yet, open **Audio...** and tick
  them; the **Interface channels** list offers only channels that are open.
  **Name** renames the output; **- Remove** takes it away and moves its
  inputs to the first one. The last output cannot be removed.
- Each input has an **Output** chooser below the inputs list. Put the lower
  manual on its own output and the desk gets it on its own channels; put a
  click track on the in-ear pair.
- Every output has a **Master volume** and **MASTER EFFECTS**, applied to
  everything that lands on it: a limiter or EQ on the way to the desk. These
  effects are not part of any program, so a program change never restarts
  them.
- **Learn volume pedal** waits for the next pedal or knob you move, on any
  keyboard and channel, and makes it that output's volume -- a master fader
  without touching a plugin. A fader taper: half way is -12 dB. **Forget
  pedal** unlearns it.

The tablet's **GUI** chooser lists an output's master effects under every input
that lands on it.

## Songs and set lists

A program is a sound. A **song** is the moment the band starts playing: one
program on each keyboard, at the right tempo. A **set list** is the order the
songs come in. Without this layer, starting a number means picking Upper 002,
then Lower 001, then the tempo -- three actions, at the wrong moment.

The songs panel is at the far left: every song on top under **SONGS**, the
running order of the chosen set list below under **SET LIST**. **Songs** in
the toolbar (or **Options ▸ Show songs and set lists**) hides and shows it.

Each song's row shows its name, then its tempo, a program per keyboard
(*002 Rhodes   001 Bass*), *♪ chart* when it has one, or *(no sounds stored
yet: press Capture)*. While a song's backing track is being read in, the line
beside **Play** says *loading*.

### Making a song

Get the sound right first: pick the program on each keyboard, set the tempo.
Then press **+**. The dialog shows exactly what is about to be stored and
asks for a name.

That is the whole idea -- a song remembers what was loaded when you captured it,
rather than being filled in by hand. To change which programs a song uses:
select the song, set each keyboard to the sound you want, press **Capture**.
It stores what each keyboard is playing now, plus the tempo and transpose.

A song holds a **program number per input**, not a copy of the plugins. So
editing program 002 changes every song that uses it: one place to fix a sound.
An input the song does not name is left alone, which is what you want for a
keyboard you are not using in that number.

- **Edit...** opens the song: **Name**, **Stage notes**, **Tempo** and
  **Transpose**, and **Save**.
- **Stage notes** are free text shown under the song on the tablet while it is
  playing -- the key, a count-in, "watch the horn stabs": whatever you would
  write on a paper set list.
- A **Tempo** of blank means "leave the tempo alone", which is not the same as
  zero. Use it for a song where the tempo is whatever you last tapped.
- **Transpose** shifts every keyboard by that many semitones while the song is
  in play -- down a step for the singer. The backing track's parts are not
  shifted. Selecting the song applies it.
- **-** deletes the selected song, and takes it out of every set list that
  used it.
- **^ v** beside the songs list reorder it. Set lists keep playing their own
  order: they follow the song, not its position.
- **Chart...** is the song's sheet music: open one, **New chord chart...**,
  **Attach a file...** and **Remove** (see *Charts on the stand*).
  **Backing track...** opens the song's track (see *Backing tracks*).

### Making a set list

**Set list...** is the menu for set lists: the ones you have are listed under
**Set lists** -- click one to make it the active set list -- then **New set
list...**, **Rename this set list...**, **Move up**, **Move down** and
**Delete this set list** (which asks first; the songs themselves are kept).
With a song selected, **Add to set list >** puts it at the end; **^ v** beside
the set list order it, and **< Remove** takes it out of the set list without
deleting the song.

A song can appear in more than one set list, and more than once in the same one
-- encores happen.

### Playing a set list

Click a song to load it: every input it names, and its tempo, at once. A click
in **SET LIST** also moves the position the footswitches step from; a click in
**SONGS** loads the song without moving it.

Loading a song is a program change on every input it names, so **Tail** and
held notes apply as they do when you change a program by hand: nothing is cut
off mid-note. Changing a program on a keyboard does not change the song.

Two footswitches step through the set list. **Footswitches...** learns them:
under **Next song** and **Previous song**, press **Learn: press the
pedal...**, then the pedal, rather than looking up what it sends. **Stop using
CC n** unlearns one. They work whatever is loaded and whatever input they
arrive on, and they say where you are ("2/8 Green Onions") in the status bar,
because a pedal gives no feedback of its own. Stepping stops at the end rather
than wrapping round to the first number. The same window learns pedals for
**Backing track: start / stop**, **Backing track: next section / release
vamp** and **Tap tempo**.

On the tablet, the Set list tab is the same thing from the stand -- see *The
tablet*.

## Charts on the stand

A song can carry its sheet music -- a lead sheet, a scan, a typed chord
chart -- and the tablet shows it. Press **Chart... ▸ Attach a file...** under
the songs list, or drop a file onto a song. PDF, PNG, JPEG, GIF and WebP are
accepted, plain text (`.txt`) is shown as it is in a monospace face, and a
ChordPro file (`.cho`, `.crd`, `.chordpro`) has its chords set above the
words. A song may have several; they appear as tabs. **Chart... ▸ Remove
<name>** takes one off the song.

The file is copied, not referred to: it goes into a folder beside the setup
named after it -- `MySetup.stagerig.json` keeps its charts in
`MySetup.stagerig.charts/` -- so the pair can be moved or backed up
together, and **Save As...** brings the folder along. Removing a chart from a
song leaves the file in the folder.

A PDF is turned into page images when it is attached, so every tablet turns
it a page at a time: tap the right third of the page or swipe left for the
next page, the left third or swipe right for the previous. This needs the
`poppler-utils` package; without it the tablet scrolls the PDF instead.

### Charts on the laptop

**Chart... ▸ Open <name>...** opens the chart in a window of its own on the
laptop, to look it over before the gig: a PDF a page at a time (**< Page**,
**Page >**, the arrow keys), an image, **-** / **+** / **Fit** for the size,
and **Open in the system app...** for anything else. A song with several
charts shows them as tabs.

A typed chart -- plain text or ChordPro -- opens with its text on the left
and the page as the stand will show it on the right, so a chord can be fixed
in the dressing room: edit, watch the page follow, press **Save** (Ctrl+S).
The tablet shows the saved version from then on. **Chart... ▸ New chord
chart...** starts one from a few lines that show the ChordPro way: chords in
square brackets before the syllable they fall on, `{title: ...}`,
`{subtitle: ...}` and `{comment: ...}` for the headings; a line without
brackets is just words. Writing notation -- a score with staves -- is a job
for MuseScore or the like; export its PDF and attach that.

On the tablet, a song with a chart shows a **Chart** button on its row. It
opens the chart in its own tab, which **follows the set list**: when the song
changes -- from a footswitch or the tablet -- the tab moves to the new song's
chart, and says so when the new song has none. Press **Follow** to pin it to
the song you are looking at instead. A song's stage notes are still the place
for a key or a count-in; a chart is for what does not fit on one line.

## Backing tracks

A song can bring a backing track: audio files that play together -- a full
mix, or drums, bass and pads on their own, plus a click and spoken cues for
your in-ears -- and MIDI files, whose parts play on instruments of your
choosing.

**Backing track...** under the songs list opens the song's track. **+ Add audio or
MIDI...** adds WAV, AIFF, FLAC, Ogg or MP3 files, or MIDI files (or drop them
on the song in the songs panel). Like charts, they are copied into the folder
beside the setup, so the setup and its tracks travel together. Adding a file
also selects the song, so **Play** plays it straight away.

### Audio files

- **Output** -- which output it plays on. Files go through that output's
  master effects and volume like everything else on it. Put the click and cue
  file on your in-ear output and nothing else there, and the audience never
  hears it.
- **Level** -- in dB, and it can be changed while the track plays.
- **X** takes the file off the song; it stays in the folder.

### MIDI files

A MIDI file is another player in the band: it plays **parts** -- the
channels it uses, each with the name the file gives it -- and you say what
each part plays on. Under the file, the window lists them:

```
demo.mid                                              [Edit notes...] [X]
   Ch 1   Piano     [Acoustic Grand Piano · General MIDI]    0.0 dB  [M]
   Ch 2   Bass      [Fingered Bass · General MIDI]           0.0 dB  [M]
   Ch 5   Strings   [Upper: Big Strings]                     0.0 dB  [M]  [Window]
   Ch 10  Drums     [Standard kit · General MIDI]            0.0 dB  [M]
```

1. **A new file plays on the General MIDI player** with the instruments it
   asks for: drums on channel 10, bass, strings, whatever each channel's
   program change says. Nothing to set up. The player's sounds come from a
   soundfont, a file of General MIDI sounds; the one your distribution
   installs is fine, and **Options ▸ General MIDI sounds (soundfont file)...**
   swaps in another. Without a player or a soundfont the parts window says so,
   and the parts can play on your own instruments meanwhile.
2. **Click a part's sound to change it.** The menu offers: the General MIDI
   instrument the file asks for (the first entry); **Another General MIDI
   instrument** or kit; **your programs**, keyboard by keyboard --
   ***Upper*'s programs ▸ 003 Big Strings** -- and, at the end of each
   keyboard's list, **Whatever *Upper* is playing**; **A MIDI device** straight
   from its port (a hardware synth, a lighting controller); anything
   **Already in this song**; or **Nothing**, which silences the part while the
   file keeps it. One instrument per part. Your programs are where your sounds
   already are -- the plugin with its patch, the split and the layer -- so a
   part goes straight to one of them.
3. **Level** is the part's level: 0 dB is as the file has it, double-click
   goes back there. **M** mutes the part -- the one you will play yourself.
4. A part on one of your programs plays that program as it is, whatever the
   keyboard itself is on: the strings part can play *Upper: Big Strings*
   while you play the piano on Upper. The program is kept loaded for as long
   as a part names it. Change the sound by changing the program, where you
   always do. A part on a plugin gets **Window**; a part on a MIDI device
   gets **Patch recall...** for the bank and program to send it.
5. **Edit notes...** opens a piano roll for the file's notes: move, resize,
   add (double-click), delete, change velocities. **+ Part** starts a new part
   on a channel the file does not use; **Snap** sets the grid; **Play** plays
   the song's track from the green marker -- the saved file, so press **Save**
   to hear your edits; **Undo** and **Redo** are there too. Closing with
   unsaved edits asks **Save** or **Discard**. The first version of the file
   is kept beside it as *.orig.mid*; to go back to it, replace the file with
   that copy by hand.

Notes follow sections, vamps and jumps like the audio does, every note is let
go when the track stops or jumps, and starting from a section in the middle
sets the General MIDI player to what the file had chosen by then. Your own
instruments keep their patches whatever the file says.

On the tablet, the same parts appear under **Parts of the track** on the Play
bar, to mute one or open its plugin from the stand (see *The tablet*).

### The Backing track input's program

To add effects to a MIDI file's instrument, or see all of them at once, select
**Backing track** under INPUTS: INSTRUMENTS shows them, with their effects,
MIDI chains and everything else an instrument has. They are ordinary
instruments in a program that StageRig makes for the song on that input --
you never choose its number, the song does. The parts window is the short way
to the same thing. To remove a part's instrument altogether, press its **X**
here.

**Count-in** clicks a number of beats at the song's tempo before the files
start: **No count-in** or **1** to **8 beats**. **Click** adds a metronome
through the whole song, accented on beat 1 of every bar (**4 beats per bar**
unless you say otherwise), following the track through vamps and jumps. Both
are **Heard on** the output you choose -- usually the in-ears, where the
audience never hears them. A song with no files and a click is a plain
metronome: Play runs it at the song's tempo until Stop. The tablet's Play bar
has a **Click** button to turn it on or off from the stand.

When a recording does not start on the beat -- a breath, a lead-in -- set
**First beat** (see *Finding the tempo* below) and the click, the count-in
and the bar lines all line up with it: the count-in's last click lands one
beat before the music's first.

### Playing the files at another tempo, or in another key

Once a song knows the tempo its files were made at (**Files at**, which
**Find the tempo** fills in), its files follow the song's own tempo: set the
song to 110 where the files are 117.4 (**Edit...** on the song) and they play
stretched to 110, drums, bass and all, with the click and the bar lines
along. The window says so (*files stretched -6%*). **Transpose the files with
the song** makes the song's transpose move the files' pitch too, and the MIDI
files' notes (drums aside), so one setting takes the whole band down a step
for the singer. Either change re-reads the files, which takes a few seconds,
so make it at the sound check, not between the verse and the chorus.

The stretching is StageRig's own and needs nothing installed. It is made for
the changes a gig needs -- a few percent of tempo, a few semitones -- and a
track pushed far from its own speed starts to smear, as every such method
does. Blank **Files at** and the files play exactly as they are.

### Finding the tempo

Under **Tempo** the window says what tempo the song uses: its own, or
whatever the tempo is now when the song sets none (**Edit...** on the song
sets it by hand). **Find the tempo in the audio...** listens to the song's
longest audio file and comes back with its tempo and where the first beat of
a bar falls -- *123.4 bpm, with the first beat of a bar at 0:00.42* -- and
asks before using it. **Use it** sets the song's tempo and **First beat**,
and the click, the count-in and the bar lines on the timeline follow.

It works on recordings with a steady beat, which is nearly every backing
track. A track that speeds up, slows down or breathes gets the tempo it
mostly keeps, and the window says so (*a faint beat: check it against the
click*). Half or double is the one thing no detector can settle -- a
backbeat at 168 is also a slow 84 -- so if the band counts it twice or half
as fast, **x2** and **/2** fix the tempo in one press. If the accent lands on
the wrong beat of the bar -- the click says *one* where the band says
*three* -- **< beat** and **beat >** move the first beat a beat at a time.
**First beat** can also be typed. MIDI files bring their own tempo and need
none of this.

A song's files are read in when the song is selected, so they can jump and
loop without a gap; the line beside **Play** says *loading* until they are
ready, and a Play pressed before then starts the moment they are.

### Sections and vamps

Under **SECTIONS** the track is drawn along time: each audio file as a
waveform, each MIDI file as its notes, with a ruler in minutes and seconds
and, when the song's tempo is known (a MIDI file brings its own; an audio
file uses the song's tempo), bar numbers too. The sections sit above it as
named blocks, each running to the next; whatever comes before the first is a
section of its own, *Start*.

- **Click** the timeline to put the green cue there; **Play** (or Space)
  plays the track from the cue, so you can listen for the spot. **Stop** is
  the same button.
- **Double-click** where a section starts to make one, and name it. Or press
  **Mark section here** while the track plays, at the moment the chorus
  lands; stopped, it marks the cue.
- **Drag** a section's flag to move it. It snaps to the nearest bar line
  when it is close; hold Alt to drag freely. The arrow keys nudge a selected
  section a tenth of a second (Shift: a second).
- **Double-click** a section to rename it. **Right-click** it for **Vamp**,
  **Go** and **Remove**; Delete removes the selected one. The list under the
  timeline shows the same sections with their times, to type one exactly.
- **Ctrl+wheel** zooms around the mouse, the wheel scrolls, **-** / **+** /
  **Fit** do the same from the buttons.

#### What a section does when the track enters it

Each section can carry a cue sheet: right-click it on the timeline (or press
its **Actions...** button in the list) and **+ Add...** offers, in the setup's
own words:

- **Put a keyboard on a program** -- Upper to *012 Big Strings* when the
  chorus arrives, so your hands never leave the keys.
- **Mute** or **unmute a part of the MIDI files** -- the piano part plays
  itself in the verse and is yours in the chorus.
- **Bypass an effect** or **turn an effect on** -- the delay only in the solo.
- **Send MIDI to a device** -- a program change, controller or note to a
  lighting desk or a hardware synth: scene 5 when the bridge starts.

They fire the moment the track enters the section: at Play, at a jump, or by
playing into it. A vamp going round does not fire them again. **Try** on
each one does it now, to hear or see it; the section shows how many it has.
Removing a section removes its sheet.

A **Vamp** section repeats until it is released: the intro goes round while
the singer talks to the audience, then goes on into the verse at the end of
the pass you are in. **Go** goes to a section when the current one ends, so
the band stays in time; while the track plays, clicking a section on the
timeline does the same. While the track is stopped, Go sets where Play starts.
Removing a section never touches the music.

### Playing it

Play begins where the sound begins: silence at the head of a file is never
played, so a track exported with an empty bar in front starts the moment you
press Play. A lead-in that is not silent -- a quiet count-in recorded into
the file, a breath -- is yours to cut: right-click the timeline where the
song should begin and choose **Start the song here**, or type the time under
**Starts at** (blank goes back to the first sound). Sections before that
point are still there to go to, and the cue shows where Play will start.

Selecting a song makes its track ready but never starts it: starting is always
on purpose. Then any of these starts it, and the same again stops it with a
short fade:

- **Play** at the top of the songs panel (it reads **Stop** while playing),
  which also shows where the track is (*0:42 / 3:51*);
- the Play bar at the top of the tablet's Set list tab;
- a footswitch: **Footswitches... ▸ Backing track: start / stop**, then press
  the pedal. It works on any keyboard and channel, like the song pedals.

**Next** beside Play does the job of one pedal: during a vamp it lets the
vamp go on (it reads **End vamp** then); otherwise it queues the next section.
The tablet shows the sections as buttons under its Play bar -- tap one to go
there when the current section ends -- with **End vamp ▸** during a vamp, and
a footswitch can do the same: **Footswitches... ▸ Backing track: next section
/ release vamp**. The panel shows the section you are in and the one queued
(*Verse 1:12 / 3:40 > Chorus*).

Moving to another song stops the track that is playing. Without a backing
track playing, plugins are told the music is always running, from bar one at
the current tempo. When a backing track starts, the position jumps to its bar
one and follows it, so a tempo-synced delay or arpeggiator lands on the
track's beat -- set the song's tempo to the track's.

## Reading the stage display

**Stage** in the toolbar puts a large readout at the top of the window: for each
input, the program number and the program's name, big enough to read from a
stand while you play. When you pick programs by number on the keyboard, this is
your confirmation that the right sound loaded. A program still loading is marked
*...loading*, which explains a keyboard that is briefly silent, and an empty
program is greyed. An audio input shows its level meter under the program
name.

## The tablet

**Tablet** in the toolbar starts a small web app on your network. Nothing is
installed on the tablet: it is a web page, and a laptop or phone on the same
network does the same job. It has two tabs: **Set list** (the running order
and the backing track) and **Programs** (every program on every keyboard, with
what each keyboard is on now at the top). **TAP TEMPO** and **PANIC** are in
the header.

![The tablet set list: the song you are on in large type, the running order, and a search](images/tablet-setlist.png)

StageRig shows the address and code to get here:

![The Tablet control dialog](images/tablet-control.png)

The fast way in is the square code: point the tablet's camera at it and it
opens StageRig directly. **Options ▸ Print the program list (for the
stand)...** puts the same code on the printed sheet. To type it instead, enter
the address once, then the six-character code. The tablet remembers the code,
and it does not change when StageRig restarts, so a home-screen shortcut keeps
working.

![Entering the code on the tablet](images/tablet-code.png)

Tablet control stays on across restarts, so a tablet on a stand keeps working
without anyone going back to the laptop.

A change from the tablet is a real change to the setup, exactly like the same
change on the laptop, and it is saved to your setup straight away.

Anyone on your network who has the code can change your sounds, which is the
right level of care for a stage tool and no more: do not expect it to be safe on
an untrusted network.

`?tab=songs` or `?tab=programs` on the address opens the page on that tab,
for a home-screen shortcut straight to it.

### Set list

The tablet opens on the set list: the current song large, with what each
keyboard is playing and your stage notes, the rest of the running order
compact. Tap any song to jump to it, or use **← PREV SONG** and **NEXT SONG
→**. When there is more than one set list, a row of buttons at the top picks
the active one.

Songs that are not in the current set list are listed underneath, under
**SONGS NOT IN THIS SET LIST** -- a song you have captured but not yet ordered
is still one you may want to play. Tapping one loads it without changing the
set list or where the footswitches step from.

The **Find a song** box matches the name, the stage notes, and the programs --
so "piano" or "002" finds a song you cannot quite name.

With a song selected (its track ready but not started), the Play bar at the
top carries **Play** / **Stop** and where the track is, under a **BACKING
TRACK** caption, with the song's sections under it: tap a section to go there
when the current one ends; a vamping section reads *↻ vamp*, and **End vamp
▸** lets it through. **Click** turns the song's metronome on or off (see
*Backing tracks*). **Parts of the track** lists the MIDI files' parts --
*Piano: Acoustic Grand Piano · General MIDI*, *Strings: Upper: Big Strings*
-- each with **Mute** (for the part you will play yourself tonight) and, when a
plugin in the song's own program plays it, **GUI** to open that plugin's
window on the tablet like any other. What plays each part is chosen on the
laptop, in the song's Backing track window; the tablet reads it.

### Programs

**Programs** shows every program on every keyboard, with a row of group
buttons to filter by (**Ungrouped** for programs with no group):

![The tablet showing the programs of both keyboards, grouped by kind](images/tablet-app.png)

The current program for each input is highlighted, and the big number at the top
of each block is what a keyboard would send as a Program Change. Tapping a
program switches to it immediately, and the page follows along if you change
programs at the laptop instead.

### Showing a plugin's own window

**GUI** at the right of each input's header opens the current program's
plugin window on the tablet, mirrored from the computer. It is level with the
program name, so it needs no scrolling. If the program is a split or a layer
with more than one instrument, **GUI** asks which one you meant. Programs with
no plugins have no button at all.

![IK Multimedia's Hammond B-3X mirrored to the tablet: the real drawbars, the Leslie and the vibrato, drawn by the plugin itself](images/tablet-plugin-gui.png)

That is the plugin itself, not a drawing of it: pull those drawbars, kick the
Leslie from slow to fast, open a menu, load a different patch. What you see is
the plugin's own window, so what it shows is always true, and there is nothing
to set up for each plugin.

This needs `x11vnc`, `websockify`, `xdotool` and `novnc` installed; StageRig
says so if any are missing. Works on X11 and Wayland desktops. If the tablet
shows a black rectangle, install `xwayland` and log in again. Nothing extra to
open in a firewall.

The tab is named after the program, so several open plugin windows are told
apart at a glance, and pressing **GUI** again for the same instrument brings its
existing tab forward rather than opening another.

A touch presses and holds for as long as your finger does, so a key on the
plugin's own keyboard sounds while you hold it, and a drawbar or knob drags.
One finger at a time, though: the mirror is a mouse, as far as the plugin
knows, so a second finger is ignored. Chords come from the keyboard.

**+** and **-** zoom a step at a time, and **Fit to screen** scales the whole
window onto the screen. **Move view** hands the touches to the browser so you
can pinch and drag the plugin around; press it again to play. **Full screen**
hides the browser's address bar. iPhones and iPads do not offer that; on those,
add StageRig to the home screen (below) and open plugin windows from there.

**The plugin window has to be uncovered on the computer.** The tablet's taps
arrive on the computer's screen as real mouse clicks, and a click goes to
whatever window is on top at that spot. StageRig brings the plugin's window to
the front each time you press **GUI**; if something then covers it -- the
computer's own browser is the classic case -- the clicks land on that window
instead, which looks like the plugin ignoring you. Move it aside.

It is for setting up and adjusting rather than playing: a window built for a
mouse, so expect to tap precisely and to miss sometimes.

### If the tablet cannot connect

The most common cause is a firewall on the computer. The symptom is
confusing, because the laptop's own browser opens the address perfectly well
while the tablet times out: only traffic from other machines is blocked.
StageRig warns about this when it can detect it. To allow the port from your
own networks only, and not from the internet:

```
sudo ufw allow from 192.168.0.0/16 to any port 7777 proto tcp
sudo ufw allow from 10.0.0.0/8 to any port 7777 proto tcp
sudo ufw allow from 172.16.0.0/12 to any port 7777 proto tcp
```

Those three ranges cover your home wifi, a venue's network and StageRig's own
hotspot. With a different firewall the principle is the same: open TCP 7777
for private networks.

### When the venue has no usable wifi

Guest networks often stop devices from seeing each other, and plenty of stages
have no wifi at all. **Create a wifi network** in the Tablet dialog makes this
computer serve its own network instead, so the tablet joins the laptop directly
and needs nothing from the venue.

![Serving our own wifi network](images/tablet-control-hotspot.png)

The first time, StageRig asks which wifi adapter to use and remembers the
answer, along with the network name and password. The tablet joins it like any
other wifi, and still has internet through whatever this computer is connected
to. Pick a name and password you are happy to reuse, because the tablet then
reconnects on its own at the next gig.

A computer with one wifi adapter leaves its current network while the hotspot
runs, which is normally what you want on stage. A cheap USB wifi adapter gives
you a second radio to keep both; the adapter list marks which is which. **Stop
the wifi network** in the same dialog returns this computer to its normal
network.

### Adding it to the home screen

On Android, use **Add to Home Screen** from Chrome's menu; on an iPhone or
iPad, **Share** then **Add to Home Screen** in Safari. The page then opens
fullscreen with its own icon, like an app, which is worth doing for the screen
space alone: an address bar costs a fifth of the height on a small screen, and
plugin windows want all of it. Browsers offer to install a page by themselves
only on an `https` address, and StageRig serves plain `http` on your own
network, so the menu route is the one to use.

## Tempo

Plugins with a tempo-synced delay, tremolo or arpeggiator ask StageRig what
the tempo is. The toolbar shows it, and **Tap** sets it: tap four times in
time and it follows. One tempo serves the whole rig, because a band plays a
song at a tempo rather than an instrument at one, and it is saved with the
setup. A song can carry its tempo, so loading the song sets it.

When you already know the tempo, type it: click the number beside **Tap** and it
becomes a text box. Type, press Return, and that is the tempo; Escape leaves it
alone. Anything outside 20 to 300 is pulled to the nearer end rather than
refused, and fractions are kept, so 97.5 stays 97.5.

Right-click **Tap** to set a pedal or button that taps it. Choose **Learn:
press the pedal or button...** and press it: that is easier than looking up
what your keyboard sends, and it works even if something is already assigned.
**Type a controller number...** is still there if you prefer it, and
**Footswitches... ▸ Tap tempo** in the songs panel learns the same pedal.

A footswitch is the usual choice; most send controller 64 or controller 80.
That controller works on any input and in any program, so it taps the tempo
whatever you happen to be playing, and only the press counts, not the release.

The tablet shows the tempo and **TAP TEMPO** in its header, so you can set it
from the music stand. Tapping on the tablet is as accurate as on the laptop.

Taps more than two seconds apart start a fresh count, so you can stop, think,
and start again. Only the last eight taps count, so speeding up is followed
rather than averaged away.

Without a backing track playing, plugins are told the music is always running,
from bar one at the current tempo. When a backing track starts, the position
jumps to its bar one and follows it. If a plugin's delay still does not sync,
it may simply never ask the host for the tempo; set it in the plugin.

### Looping a passage

**Loop** beside Footswitches... loops what you play on the selected keyboard: press it
and play; press it again and the passage goes round on its own, trimmed to
whole bars at the rig's tempo (the nearest; at least one), while you play
over it. Press again to stop it, again to start it from the top; right-click
for **Clear**. The button reads *Rec...*, then *2 bars*, then *Loop >*.

A footswitch is the way to do it with your hands on the keys: **Footswitches
▸ Loop: record / play / stop**, and a second pedal for **Loop: clear**. The
pedal loops the keyboard it is pressed on. Press it on the *one*: the loop
starts the moment you press, and ends on the nearest bar line to your second
press, so a bar's passage played a little long or short still comes round
in time. The loop plays into whatever program the keyboard is on, so a
program change changes its sound.

### Recording the gig

**Options ▸ Record the gig** writes every output to its own WAV file --
*Main.wav*, *In-ears.wav* -- exactly as it left for the desk, after the
master effects and volume, and everything the keyboards sent to
*keyboards.mid*, a track per input, timed to the clock. The status bar reads
**REC** with the time; **Options ▸ Stop recording** finishes the files. A
take is a folder named by date and time inside `MySetup.stagerig.recordings/`
beside the setup, so it travels with it like the charts. Listen back to a
rehearsal, or keep the night that went well.

### Ableton Link

**Options ▸ Ableton Link** shares the tempo and the beat over the network
with anything else that has Link: Live or Bitwig on another laptop, a loop
app on a phone, a DJ's software, a drum machine that speaks it. Nothing to
connect: they find each other on the wifi (StageRig's own hotspot works
too), and the tempo readout shows *Link · 2* for two others. From then on
the rig's tempo and theirs are one number, from whichever side set it last:
tap yours and they follow, let the drummer tap theirs and your click,
arpeggiators, delays and MIDI clock follow. **Play** on the backing track
waits for the next shared bar line, count-in included, so its bar one is
the band's. **Link also starts and stops the others** makes Play and Stop
go both ways; off, each side keeps its own transport. A venue network that
keeps devices apart gives *Link* with no number, and nothing else changes.

Link is Ableton's, open source, and is built in -- see *Licence* in the
README.

### MIDI clock to hardware

**Options ▸ Send MIDI clock to** lists the MIDI devices on the computer; tick
one and StageRig sends it MIDI clock -- 24 ticks a beat at the rig's tempo --
so a drum machine, a hardware synth's arpeggiator or a delay pedal keeps time
with everything else. A tapped tempo reaches it at once. When the backing
track starts, the device gets **Start**, so its bar one is the track's; when
the track stops, **Stop**. Several devices can be ticked. The choice is saved
with the setup.

## Printing the program list

**Options ▸ Print the program list (for the stand)...** writes a sheet of
which program number plays which sound, and opens it in your browser; print it
from there. Only programs you have filled in are listed, with each program's
number, its name and the instruments in it, grouped by input with the MIDI
device and channel, and by group within the input. Tape it to the keyboard and
you never have to remember that the Rhodes is 001.

If tablet control is running when you print, the sheet also carries the square
code from the Tablet dialog, so the page on the keyboard doubles as the way onto
the tablet.

![The code as printed](images/qr-example.png)

## The on-screen keyboard

**Keyboard** in the toolbar opens an on-screen keyboard at the bottom of the
window with velocity, sustain, mod wheel, pitch bend and a free controller
number and value. It plays into the selected input on that input's channel,
through exactly the same path as a real keyboard, so it works for auditioning
sounds, checking splits and doing MIDI learn without a controller connected.

The keys follow the instrument you click: **click an instrument** in the
program and the keys play that one, on its channel. A button beside *Playing
into* names the channel and the instrument (*ch 5 · Upper: Big Strings*) and
lists the others. This is how you hear one part of a Backing track program, or
one instrument of a keyboard on *All channels* whose program gives each
channel its own sound. An input with a channel of its own plays on it and
shows no choice.

Click the keys with the mouse, or focus the keyboard and use the computer
keyboard: `A S D F G H J K` for white keys, `W E T Y U` for sharps, `Z` and `X`
to shift octave.

## Audio settings and latency

The **buffer size** is how much audio the computer prepares at a time, in
samples. A smaller buffer means less delay between a key and the sound, and
less room for a slow plugin; 128 samples is 2.7 ms at 48 kHz and a good live
default.

**Audio...** has two parts. The top is StageRig's own **LATENCY** section:

- **Buffer size** -- the buffer StageRig asks for. 128 is the live default; 64
  if the machine is quiet and has realtime scheduling (see below).
- **Make the whole system use this buffer size while StageRig runs** -- forces
  the whole audio system to that buffer size at start and restores the previous
  value on quit. Turn it on when another app has forced a big buffer and
  StageRig cannot get a small one.
- A status line shows the device, the buffer size actually delivered, the
  resulting latency, whether realtime scheduling is available, and warnings.

The lower part is the device chooser: device type, output device and
channels, sample rate and, for audio inputs, the input device and which of its
channels to open. Only open channels are offered to an audio input.

**For the lowest latency:**

- Use the **JACK** device type. On a PipeWire system this plugs StageRig
  straight into the sound system with no resampling and no extra buffering.
  The ALSA type goes through a compatibility layer.
- Give yourself **realtime scheduling** -- permission for the audio to run
  ahead of everything else on the machine. Run `ulimit -r`; if it prints 0,
  add yourself to the `pipewire` group with `sudo usermod -aG pipewire $USER`
  and log out and back in. Small buffers without it cause dropouts.
- **Close other audio applications.** A DAW such as Bitwig forces the whole
  system to its own buffer size every few seconds; StageRig then shows *clock
  held by another app* in the status bar and cannot get its own buffer size.

**A USB interface on the ALSA type** is driven by StageRig directly. Two
settings there sound like a broken cable, and the status line warns about
them: a buffer below 128 samples (256 is safe, 128 often works), and an input
device different from the output device (two clocks drift and crackle -- use
the same interface for both, or untick the input channels).

*late N* in the status bar counts buffers where a plugin did not answer in
time. A few during loading are normal. A number that climbs while you play
means the buffer size is too small for the plugins in that program.

## Saving your setup

A setup is one `.stagerig.json` file holding every input, program, plugin and
mapping, including each plugin's own state, and your songs and set lists.

- **Save** / **Save As...** write the file. The last file reopens next start.
  New saves use `.stagerig.json`; a `.performer.json` from before the rename
  opens as before and keeps its name.
- Plugin state is captured whenever you leave a program, close a plugin's
  window, unload a program or save, so tweaks made inside a plugin's own window
  persist.
- StageRig autosaves about once a minute and on quit, to
  `~/.config/StageRig/autosave.stagerig.json`. If the autosave is newer than
  the setup you open, the status bar says so. Press **Save** between songs,
  not mid-song.

A setup file is plain text, so a copy is a backup.

## Playing live

A short checklist:

- Turn on **Options ▸ Preload all programs** and give StageRig time to load
  everything before the first song. The status bar stops showing loading
  activity when it is done.
- Check that the status bar shows the buffer size you expect and no *late*
  count.
- Leave **Let held notes ring through a program change** on and set the
  **Tail** long enough that program changes do not cut your release.
- Learn where **PANIC** is.
- Keep one input per keyboard in your setup, with program numbers matching the
  patch numbers on your keyboard, so you never touch the computer.

## How long loading takes

Programs load several plugins at once, so a rig is ready in about the time its
slowest plugin takes: Hammond B-3X takes about seven seconds to start under
Wine whatever else is happening, so a program using it is ready in about seven
seconds, and a rig of eight plugins is ready in about the same. Reopening a
plugin's window is instant.

How many load at once is half the machine's threads, between four and twelve;
`parallelLoads` in the settings file overrides it (see *Files StageRig uses*).

## Plugin formats

StageRig hosts **VST3**, **CLAP**, **LV2** and **LADSPA**. Windows VST3s and
CLAPs bridged with yabridge appear as ordinary plugins of that format.

**CLAP** plugins are scanned from `~/.clap`, `/usr/lib/clap` and
`/usr/local/lib/clap`, and load like any other format. A `.clap` file can
hold several plugins -- Surge XT ships the synth and its effects rack in one
file -- so each entry in the Plugins window is one plugin, not one file.

Stepped parameters -- a waveform selector, say -- snap to their nearest step, so
the value a plugin reports back may differ slightly from what was sent while
naming the same setting. That is normal and not a sign of a lost setting.

## Good free plugins to start with

StageRig installs no plugins of its own beyond the General MIDI player, but
Linux has free ones good enough to gig with, and the distributions package
them. One line gets a working rig's worth; press **Scan LV2** (and **Scan
VST3**, **Scan CLAP** where a plugin ships them) afterwards:

```sh
# Debian / Ubuntu / Mint
sudo apt install surge-xt setbfree zynaddsubfx-lv2 avldrums.lv2 \
                 dragonfly-reverb lsp-plugins-lv2 x42-plugins calf-plugins \
                 guitarix-lv2 zam-plugins
# Fedora
sudo dnf install setBfree zynaddsubfx lsp-plugins-lv2 lv2-calf-plugins guitarix zam-plugins
```

Instruments:

- **Surge XT** -- a serious synth: pads, leads, basses, keys, with its own
  effects. The first thing to install. (Fedora: from the Surge site.)
- **setBfree** -- a tonewheel organ with a Leslie, and a convincing one.
- **ZynAddSubFX** -- pads, strings, bells, choirs; thousands of presets.
- **AVL Drumkits** -- acoustic drums that sound like drums, and load fast.
- The built-in **General MIDI player** covers the piano, the bass and
  everything else until you have better.

Effects:

- **Dragonfly Reverb** -- hall, room and plate reverbs; the one to put on the
  master effects.
- **LSP Plugins** -- a full studio: compressors, EQs, limiters, gates,
  de-essers. Where to look for any dynamics or EQ job.
- **x42** -- meters, a stereo tool, a limiter, an EQ, auto-tune.
- **Calf** -- chorus, vintage delay, phaser, a tube-ish saturator, and an
  organ and mono synth of their own.
- **Guitarix** -- amps, cabinets and pedals for a guitar on an audio input.
- **ZamAudio** -- ZamComp, ZamEQ2, ZamTube: simple and good.

Not packaged, worth fetching from their own sites, all native Linux: **Vital**
(free tier) and **Odin 2** (synths), **Dexed** (the DX7), u-he's free
**Tyrell N6**, **Podolski** and **Zebralette**, **sfizz** with the
**Salamander Grand Piano** (a real piano, in SFZ), **Neural Amp Modeler**
(amp captures), **Airwindows Consolidated** (hundreds of effects in one
plugin), **Cardinal** (a modular). Windows-only plugins go through yabridge;
see *Windows plugins*.

## Windows plugins

Windows VST3 and CLAP plugins bridged with [yabridge](https://github.com/robbert-vdh/yabridge)
appear as ordinary plugins of that format. Scan the folders yabridge puts them
in, usually `~/.vst3/yabridge` for VST3s and `~/.clap/yabridge` for CLAPs
(**Scan folder...** in the Plugins window).

Setting the Wine side up by hand is the fiddly part, and
[vstenv](https://github.com/dguedry/vstenv) exists to do it for you: it
installs Wine, the vendors' manager applications and the products they
deliver, and bridges everything into `~/.vst3/yabridge` for StageRig to scan.
Nothing in StageRig depends on it -- any yabridge setup works. If you set Wine
up yourself, StageRig uses the same Wine your desktop session does, so a
plugin that runs from a terminal also runs from the launcher.

While a vendor's installer replaces a plugin, the bridged file is briefly
missing. StageRig reports *yabridge link points to a missing file* rather than
switching the plugin off; scan again once the install has finished.

## When something goes wrong

| What you see | What it means | What to do |
|---|---|---|
| An instrument's row is red, with **Reload** instead of **Window** | That plugin crashed. Only that plugin is affected. | Press **Reload**; it restarts with the saved state. |
| A plugin shows *Deactivated* in the Plugins window | It hung or crashed twice while being scanned. | Select it and press **Remove selected** to give it another chance. |
| *late N* climbing while playing | Plugins are not keeping up with the buffer. | Raise the buffer size in **Audio...**, or check realtime scheduling. |
| *clock held by another app* | Another audio application forces the system's buffer size. | Close it, or set its buffer size to match. |
| Static or crackle on a USB interface under the ALSA type | The buffer is below 128 samples, or the input is on a different device from the output. | Set 256 (or 128) in **Audio...**, and the same interface for input and output; the status line says which it is. |
| A few seconds of static about once a minute | Autosave reading a plugin's state. | Press **Save** between songs rather than mid-song. |
| No sound, plugin loaded | Instrument turned off, key or velocity range excludes what you play, gain at minimum, or wrong MIDI channel. | Check the instrument's controls; the on-screen keyboard rules out the controller. |
| The keyboard is silent after plugging it in | StageRig has not seen the device yet. | **Options ▸ Rescan MIDI devices**, then check the input's **MIDI device** and **Channel**. |
| Program changes from the keyboard do nothing | They arrive on a channel the input is not watching, or **Respond to Program Change** is off. | Set **Arrive on** to *Any channel*; tick **Respond to Program Change**. |
| A note hangs | A plugin missed a note-off. | Press **PANIC**. |
| The parameter list seems endless | A plugin that lists its MIDI controllers per channel. | Type in the search box; see *MIDI mappings*. |
| An empty plugin window that will not respond | A plugin made only for a DAW's audio-editing extension. | Use the plugin's ordinary version. |
| The tablet times out while the laptop's browser opens the page | The computer's firewall. | See *If the tablet cannot connect*. |
| The tablet shows a black rectangle instead of the plugin | The desktop session has no X display for the mirror. | Install `xwayland` and log in again. |
| The tablet's taps land somewhere else | Another window covers the plugin's window on the computer. | Move it aside; see *Showing a plugin's own window*. |
| A Spitfire plugin's window (BBC Symphony Orchestra, LABS, Originals) locks up or crashes the plugin the first time it opens | It draws with OpenGL, which faults under Wine. | In the Wine prefix, open `drive_c/users/<you>/AppData/Roaming/Spitfire Audio/Settings/<plugin>.settings`, change `defaultOpenGL` from `1` to `0`, then press **Reload**. It looks the same. |

## Files StageRig uses

| Path | What it is |
|---|---|
| `~/.config/StageRig/StageRig.settings` | Window, audio device, scan folders, known plugins, tuning options. |
| `~/.config/StageRig/autosave.stagerig.json` | The rolling autosave. |
| `~/.config/StageRig/mapping-templates.json` | Your saved per-plugin mapping templates. |
| `~/.config/StageRig/icons/` | Cached plugin icons. |
| `~/.config/StageRig/RecentlyCrashedPluginsList` | Plugins that crashed while scanning. |
| your `*.stagerig.json` | Your setup. Keep it wherever you like. |
| `<your setup>.charts/` beside it | The songs' charts and backing-track files, copied in when you add them, so the setup and its files travel together. An edited MIDI file keeps its first version here as `<name>.orig.mid`. |

The General MIDI player is FluidSynth (*libfluidsynth3*, Fedora's
*fluidsynth-libs*) with the soundfont your distribution installs
(*fluid-soundfont-gm*); **Options ▸ General MIDI sounds (soundfont file)...**
chooses another, such as GeneralUser GS.

Advanced options can be set in `StageRig.settings`: `parallelLoads` (how many
plugins load at once; default half the machine's threads, 4 to 12),
`parallelBridgedLoads`, `bridgedColdStartSolo`, `pipewireQuantum` (the buffer
size) and `pipewireTakeover`.

## Keyboard and mouse reference

| Action | Where |
|---|---|
| Double-click a gain, pan, transpose or curve bar | Resets it to its neutral value. |
| Drag a range slider's handles | Sets the low and high ends of a key or velocity range. |
| Type in the parameter picker | Filters thousands of parameters; arrows and Page keys move, Return picks, Escape cancels. |
| `A S D F G H J K` / `W E T Y U` | White and black keys on the on-screen keyboard. |
| `Z` / `X` | Shift the on-screen keyboard down or up an octave. |
| Double-click a part's level in the Backing track window | Back to 0 dB: the part as the file has it. |
| The piano roll (**Edit notes...**) | Double-click adds a note (on a note: deletes it); drag its right edge to resize; Delete removes the selection; arrows move it a step (Shift: an octave); Ctrl+wheel zooms; Space plays; `Ctrl+Z` / `Ctrl+Shift+Z` undo and redo. |
| The chart window | Arrow keys or Page Up / Page Down turn a PDF's pages; Ctrl+wheel zooms; `Ctrl+S` saves a typed chart. |
| The timeline in the Backing track window | Click sets the cue (Play starts there); double-click starts a section; drag a flag moves it (snaps to bars; Alt: free); double-click a section renames it; right-click: vamp, go, remove; Delete removes the selected one; arrows nudge it; Ctrl+wheel zooms; Space plays. |
| Click a section's name or time in the list under the timeline | Edits it exactly. |
| Escape | Closes a dialog. |

## Appendix: If you know Ableton Live

The pieces map, but the centre of gravity is different. Live is built
around tracks; StageRig is built around the keyboard in front of you and
the sound it is on, the way a stage piano's patches are.

| In Live | In StageRig | The difference |
|---|---|---|
| A **Set** (the file) | the **setup** file, `*.stagerig.json` | the whole rig: inputs, programs, songs, set lists, plugin states |
| A MIDI **track** with *MIDI From* a device and channel | an **input**: a keyboard, or one channel of it | an input is not a sound; it is where the notes come from, and it holds 128 programs |
| The track's **device chain** (Instrument Rack with chains, key and velocity zones, effects) | a **program**: instruments with key and velocity ranges, each with its own MIDI chain and effects, and the program's effects after them | a program is one sound of your show; a Program Change from the keyboard switches to another, and only that keyboard's sound changes |
| Switching sounds: arm another track, or a Rack's chain selector | select another program (keys, pedal, click, tablet) | nothing to arm; each keyboard changes independently |
| An audio track with effects | an **audio input** whose programs are effects | the vocal's reverb steps with a footswitch like any program |
| *Audio To* | the input's **output**, a pair of interface channels with master effects and a volume | |
| A **scene** per song, or a Set per song | a **song**: a program per input, a tempo, notes, charts, a backing track | one place that says what every keyboard is on for this number |
| Locators in the arrangement; a looped clip | **sections** on the backing track; a **vamp** | jumps land at the end of the current section, in time |
| The arrangement's audio and MIDI clips | the song's **audio files** and **MIDI files**, each MIDI part playing on an instrument you pick | the MIDI files' instruments live in a program on the *Backing track* input, which the song selects |
| MIDI Map mode | **MIDI mappings**, with Learn, in the program | per program, so a pedal does different things in different sounds |
| A **set list** in Live needs a plugin or a template | a **set list**: the songs in order, stepped from a pedal or the tablet | built in |
