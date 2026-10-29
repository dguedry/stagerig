#include "SongsPanel.h"
#include "Confirm.h"
#include "Charts.h"
#include "MidiFx.h"
#include "GmSynth.h"
#include "GmMixer.h"
#include "MidiEditor.h"
#include "TrackTimeline.h"
#include "TempoDetect.h"
#include "ChartViewer.h"
#include "SectionActions.h"
#include <thread>
#include "PatchDialog.h"
#include <cmath>
#include <map>

namespace perf
{

using namespace juce;

namespace
{
    const Colour bgPanel   { 0xff1b1d22 };
    const Colour bgRow     { 0xff23262d };
    const Colour accent    { 0xff4f9dff };
    const Colour accentDim { 0xff2b4f80 };
    const Colour textDim   { 0xff9aa0ab };
}

//==============================================================================
SongsPanel::SongsPanel (Engine& e) : engine (e)
{
    for (auto* l : { &songsHeader, &setHeader })
    {
        addAndMakeVisible (l);
        l->setColour (Label::textColourId, textDim);
        l->setFont (FontOptions (11.0f));
    }

    /* Find a song by name, notes, or what it loads: a list of hundreds is no
       use to scroll on a stand. Escape clears it. */
    addAndMakeVisible (findBox);
    findBox.setTextToShowWhenEmpty ("Find a song  (name, notes, sound)", textDim);
    findBox.setFont (FontOptions (12.0f));
    findBox.setColour (TextEditor::backgroundColourId, bgRow);
    findBox.onTextChange = [this] { refresh(); };
    findBox.onEscapeKey = [this] { findBox.clear(); refresh(); };

    songsList.setModel (&songsModel);
    setList.setModel (&setModel);
    for (auto* b : { &songsList, &setList })
    {
        addAndMakeVisible (b);
        b->setRowHeight (34);          // two lines: the name, and what it loads
        b->setColour (ListBox::backgroundColourId, bgRow);
    }

    addAndMakeVisible (addBtn);
    addBtn.setTooltip ("A new song that stores what is loaded now");
    addBtn.onClick = [this] { addSong(); };

    addAndMakeVisible (removeBtn);
    removeBtn.setTooltip ("Delete the selected song, and take it out of every set list that used it");
    removeBtn.onClick = [this] { removeSong(); };

    addAndMakeVisible (captureBtn);
    captureBtn.setColour (TextButton::buttonColourId, accentDim);
    captureBtn.setTooltip ("Capture stores in this song what each keyboard is playing now, plus the tempo and transpose. "
                           "This is how you change which sounds a song loads: select it, change the programs, then press this.");
    captureBtn.onClick = [this] { captureIntoSelectedSong(); };

    addAndMakeVisible (editBtn);
    editBtn.setTooltip ("Name, stage notes and tempo for this song");
    editBtn.onClick = [this] { editSelectedSong(); };

    addAndMakeVisible (chartBtn);
    chartBtn.setTooltip ("The song's sheet music, for the stand: open one, write a chord chart, or attach a PDF, an image, "
                         "a text chart or a ChordPro file. Or drop a file on the song.");
    chartBtn.onClick = [this] { showChartMenu(); };
    addAndMakeVisible (trackBtn);
    trackBtn.setTooltip ("The song's backing track: its audio and MIDI files, sections, count-in, click and tempo. Or drop files on the song.");
    trackBtn.onClick = [this] { if (selectedSong >= 0) showTracksDialog (selectedSong); };

    addAndMakeVisible (loopBtn);
    loopBtn.setTooltip ("Loop a passage on the selected keyboard: press to record, press again to play it round (whole bars at the rig's tempo), "
                        "press to stop, press to play again. Right-click clears. A footswitch can do the same (Footswitches...).");
    loopBtn.onClick = [this]
    {
        const int input = selectedInputProvider ? selectedInputProvider() : 0;
        engine.loopPedal (input);
        refreshLoopButton();
    };
    addAndMakeVisible (playBtn);
    playBtn.setTooltip ("Start or stop the backing track of the song in play, after its count-in. "
                        "A footswitch can do the same (Footswitches...).");
    playBtn.onClick = [this] { engine.toggleTrack(); refreshTransport(); };
    addAndMakeVisible (nextBtn);
    nextBtn.setTooltip ("Releases a vamp at the end of its pass, or queues the next section of the backing track. "
                        "A footswitch can do the same (Footswitches...).");
    nextBtn.onClick = [this] { engine.advanceTrack(); refreshTransport(); };
    addAndMakeVisible (trackLabel);
    trackLabel.setFont (FontOptions (12.0f));
    trackLabel.setColour (Label::textColourId, textDim);

    addAndMakeVisible (toSetBtn);
    toSetBtn.setTooltip ("Put the selected song at the end of the set list");
    toSetBtn.onClick = [this] { addToSet(); };

    addAndMakeVisible (fromSetBtn);
    fromSetBtn.setTooltip ("Take this song out of the set list. The song itself is kept.");
    fromSetBtn.onClick = [this] { removeFromSet(); };

    addAndMakeVisible (songUpBtn);
    songUpBtn.setTooltip ("Move this song up the list. Set lists keep their own order.");
    songUpBtn.onClick = [this] { moveSongInList (-1); };
    addAndMakeVisible (songDownBtn);
    songDownBtn.setTooltip ("Move this song down the list");
    songDownBtn.onClick = [this] { moveSongInList (1); };

    addAndMakeVisible (upBtn);
    upBtn.setTooltip ("Move this song earlier in the set list");
    upBtn.onClick = [this] { moveInSet (-1); };
    addAndMakeVisible (downBtn);
    downBtn.setTooltip ("Move this song later in the set list");
    downBtn.onClick = [this] { moveInSet (1); };

    addAndMakeVisible (setBtn);
    setBtn.setTooltip ("Choose which set list you are playing, or make a new one");
    setBtn.onClick = [this] { showSetMenu(); };

    addAndMakeVisible (footBtn);
    footBtn.setTooltip ("Pedals that step through the set list and start, stop or advance the backing track");
    footBtn.onClick = [this] { showFootswitchMenu(); };

    /* The set can move without anyone touching this panel -- a footswitch, the
       tablet, a program change. Polling rather than listening keeps this panel
       out of the Engine's listener list, which is already long, and ten times a
       second is plenty for a list of songs. */
    startTimerHz (10);
    refresh();
}

SongsPanel::~SongsPanel()
{
    songsList.setModel (nullptr);
    setList.setModel (nullptr);
}

//==============================================================================
void SongsPanel::paint (Graphics& g)
{
    g.setColour (bgPanel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
}

void SongsPanel::resized()
{
    auto r = getLocalBounds().reduced (6);

    auto top = r.removeFromTop (24);
    const int third = top.getWidth() / 3;
    setBtn.setBounds (top.removeFromLeft (third + 10).reduced (1, 0));
    footBtn.setBounds (top.removeFromLeft (third + 24).reduced (1, 0));
    loopBtn.setBounds (top.reduced (1, 0));
    r.removeFromTop (4);
    auto transport = r.removeFromTop (24);
    playBtn.setBounds (transport.removeFromLeft (52).reduced (1, 0));
    nextBtn.setBounds (transport.removeFromLeft (48).reduced (1, 0));
    transport.removeFromLeft (4);
    trackLabel.setBounds (transport);
    r.removeFromTop (6);

    // The set list is what you play from, so it gets the larger half.
    auto songsArea = r.removeFromTop (r.getHeight() * 45 / 100);
    r.removeFromTop (6);

    songsHeader.setBounds (songsArea.removeFromTop (14));
    findBox.setBounds (songsArea.removeFromTop (22));
    songsArea.removeFromTop (3);
    auto songFiles = songsArea.removeFromBottom (26);
    songsArea.removeFromBottom (2);
    auto songButtons = songsArea.removeFromBottom (26);
    songsArea.removeFromBottom (4);
    auto songMoveCol = songsArea.removeFromRight (28);
    songUpBtn.setBounds (songMoveCol.removeFromTop (26).reduced (1));
    songMoveCol.removeFromTop (2);
    songDownBtn.setBounds (songMoveCol.removeFromTop (26).reduced (1));
    songsArea.removeFromRight (4);
    songsList.setBounds (songsArea);

    addBtn.setBounds (songButtons.removeFromLeft (28).reduced (1, 0));
    removeBtn.setBounds (songButtons.removeFromLeft (28).reduced (1, 0));
    captureBtn.setBounds (songButtons.removeFromLeft (songButtons.getWidth() / 2).reduced (1, 0));
    editBtn.setBounds (songButtons.reduced (1, 0));
    chartBtn.setBounds (songFiles.removeFromLeft (songFiles.getWidth() * 2 / 5).reduced (1, 0));
    trackBtn.setBounds (songFiles.reduced (1, 0));

    setHeader.setBounds (r.removeFromTop (14));
    auto setButtons = r.removeFromBottom (26);
    r.removeFromBottom (4);
    auto moveCol = r.removeFromRight (28);
    upBtn.setBounds (moveCol.removeFromTop (26).reduced (1));
    moveCol.removeFromTop (2);
    downBtn.setBounds (moveCol.removeFromTop (26).reduced (1));
    r.removeFromRight (4);
    setList.setBounds (r);

    toSetBtn.setBounds (setButtons.removeFromLeft (setButtons.getWidth() * 60 / 100).reduced (1, 0));
    fromSetBtn.setBounds (setButtons.reduced (1, 0));
}

//==============================================================================
void SongsPanel::refresh()
{
    const auto& songs = engine.getSongs();
    const auto& sets  = engine.getSets();
    const int set = engine.getCurrentSet();

    const auto query = findBox.getText().trim();
    shownSongs.clear();
    for (int i = 0; i < (int) songs.size(); ++i)
        if (songMatches (songs[(size_t) i], query)) shownSongs.push_back (i);
    songsHeader.setText (songs.empty() ? "SONGS  (none yet: + stores what is loaded now as a song)"
                       : query.isEmpty() ? "SONGS  (" + String ((int) songs.size()) + ")"
                                         : "SONGS  (" + String ((int) shownSongs.size()) + " of " + String ((int) songs.size()) + " match)",
                         dontSendNotification);

    if (set >= 0 && set < (int) sets.size())
    {
        const auto& st = sets[(size_t) set];
        setHeader.setText ("SET LIST: " + st.name.toUpperCase() + "  ("
                             + String ((int) st.songs.size()) + ")", dontSendNotification);
    }
    else
    {
        setHeader.setText (sets.empty() ? "SET LIST  (none yet: Set list... > New set list...)" : "SET LIST  (none chosen: pick one under Set list...)", dontSendNotification);
    }

    const bool haveSong = selectedSong >= 0 && selectedSong < (int) songs.size();
    const bool haveSet  = set >= 0 && set < (int) sets.size();
    removeBtn.setEnabled (haveSong);
    captureBtn.setEnabled (haveSong);
    editBtn.setEnabled (haveSong);
    chartBtn.setEnabled (haveSong);
    trackBtn.setEnabled (haveSong);
    toSetBtn.setEnabled (haveSong && haveSet);
    toSetBtn.setTooltip (haveSet ? "Put the selected song at the end of the set list" : "Make or choose a set list first (Set list...)");
    fromSetBtn.setEnabled (haveSet && selectedInSet >= 0);
    songUpBtn.setEnabled (haveSong && selectedSong > 0);
    songDownBtn.setEnabled (haveSong && selectedSong < (int) songs.size() - 1);
    upBtn.setEnabled (haveSet && selectedInSet > 0);
    downBtn.setEnabled (haveSet && selectedInSet >= 0
                        && selectedInSet < (int) sets[(size_t) set].songs.size() - 1);

    songsList.updateContent();
    setList.updateContent();
    // The selection follows the song, not the row, as the find box changes what the rows are.
    if (const int row = rowOfSong (selectedSong); row != songsList.getSelectedRow())
    {
        if (row >= 0) songsList.selectRow (row, true);
        else songsList.deselectAllRows();
    }
    songsList.repaint();
    setList.repaint();
}

int SongsPanel::rowOfSong (int songIndex) const
{
    for (int r = 0; r < (int) shownSongs.size(); ++r)
        if (shownSongs[(size_t) r] == songIndex) return r;
    return -1;
}

bool SongsPanel::songMatches (const SongDef& sg, const String& query) const
{
    if (query.isEmpty()) return true;
    if (sg.name.containsIgnoreCase (query) || sg.notes.containsIgnoreCase (query)) return true;
    // What it loads, too: "kontakt" or "002" is how you find a song you cannot name.
    const auto& inputs = engine.getSetup().inputs;
    for (int i = 0; i < (int) inputs.size(); ++i)
    {
        const int prog = sg.programFor (i);
        if (prog < 0 || prog >= (int) inputs[(size_t) i].programs.size()) continue;
        if (String (prog).paddedLeft ('0', 3).contains (query)) return true;
        if (inputs[(size_t) i].programs[(size_t) prog].name.containsIgnoreCase (query)) return true;
    }
    return false;
}

void SongsPanel::refreshLoopButton()
{
    const int input = selectedInputProvider ? selectedInputProvider() : 0;
    const auto st = engine.getLoopState (input);
    const int bars = engine.getLoopBars (input);
    loopBtn.setButtonText (st == Engine::LoopState::recording ? "Rec..." : st == Engine::LoopState::playing ? String (bars) + (bars == 1 ? " bar" : " bars")
                         : st == Engine::LoopState::stopped ? "Loop >" : "Loop");
    loopBtn.setColour (TextButton::buttonColourId, st == Engine::LoopState::recording ? Colour (0xffa3282d)
                                                 : st == Engine::LoopState::playing ? accentDim : getLookAndFeel().findColour (TextButton::buttonColourId));
}

void SongsPanel::showLoopMenu()
{
    const int input = selectedInputProvider ? selectedInputProvider() : 0;
    PopupMenu m;
    m.addItem (1, "Stop the loop", engine.getLoopState (input) == Engine::LoopState::playing);
    m.addItem (2, "Clear the loop", engine.getLoopState (input) != Engine::LoopState::empty);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (loopBtn), [this, input] (int r)
    {
        if (r == 1) engine.loopStop (input);
        else if (r == 2) engine.loopClear (input);
        refreshLoopButton();
    });
}

void SongsPanel::timerCallback()
{
    refreshLoopButton();
    /* Follow the set moving under us. Only the highlight and the headers can
       change without a click here, so this is cheap. */
    static int lastSet = -2, lastPos = -2, lastSongs = -1, lastSets = -1, lastPlaying = -2;
    const int set = engine.getCurrentSet(), pos = engine.getCurrentSongInSet(), playing = engine.currentSongIndex();
    const int nSongs = (int) engine.getSongs().size(), nSets = (int) engine.getSets().size();
    if (set != lastSet || pos != lastPos || nSongs != lastSongs || nSets != lastSets || playing != lastPlaying)
    {
        lastSet = set; lastPos = pos; lastSongs = nSongs; lastSets = nSets; lastPlaying = playing;
        refresh();
    }
    refreshTransport();
}

void SongsPanel::refreshTransport()
{
    auto mmss = [] (double s) { const int t = (int) s; return String (t / 60) + ":" + String (t % 60).paddedLeft ('0', 2); };
    const int song = engine.getTrackSong();
    const auto& songs = engine.getSongs();
    const bool has = engine.hasTrack() && song >= 0 && song < (int) songs.size();

    playBtn.setEnabled (has);
    playBtn.setButtonText (engine.isTrackRunning() ? "Stop" : "Play");
    const bool vamping = engine.isTrackVamping();
    nextBtn.setEnabled (has && (engine.getTrackSectionCount() > 1));
    nextBtn.setButtonText (vamping ? "End vamp" : "Next");
    nextBtn.setColour (TextButton::buttonColourId, vamping ? Colour (0xff8a5a12) : getLookAndFeel().findColour (TextButton::buttonColourId));
    playBtn.setColour (TextButton::buttonColourId, engine.isTrackRunning() ? accentDim : getLookAndFeel().findColour (TextButton::buttonColourId));

    String text;
    if (! has)
        text = "No backing track (Backing track... under the songs)";
    else if (! engine.isTrackReady())
        text = songs[(size_t) song].name + "  -- loading " + String (roundToInt (engine.getTrackLoadProgress() * 100.0f)) + "%";
    else if (engine.isTrackCountingIn())
        text = songs[(size_t) song].name + "  -- counting in";
    else
    {
        // A metronome with no files has no length worth showing.
        text = songs[(size_t) song].stems.empty() ? "click  " + mmss (engine.getTrackPosition())
                                                   : mmss (engine.getTrackPosition()) + " / " + mmss (engine.getTrackLength());
        if (engine.getTrackSectionCount() > 1)
        {
            text = engine.getTrackSectionName (engine.getTrackSection()) + (vamping ? " (vamp)" : "") + "  " + text;
            if (const int q = engine.getQueuedTrackSection(); q >= 0)
                text << "  > " << engine.getTrackSectionName (q);
        }
        else
            text = songs[(size_t) song].name + "  " + text;
    }
    if (trackLabel.getText() != text) trackLabel.setText (text, dontSendNotification);
}

//==============================================================================
int SongsPanel::getNumRows() { return 0; }
void SongsPanel::paintListBoxItem (int, Graphics&, int, int, bool) {}
void SongsPanel::listBoxItemClicked (int, const MouseEvent&) {}
void SongsPanel::listBoxItemDoubleClicked (int, const MouseEvent&) {}

int SongsPanel::ListProxy::getNumRows()
{
    if (! isSet) return (int) owner.shownSongs.size();
    const int set = owner.engine.getCurrentSet();
    const auto& sets = owner.engine.getSets();
    return set >= 0 && set < (int) sets.size() ? (int) sets[(size_t) set].songs.size() : 0;
}

void SongsPanel::ListProxy::paintListBoxItem (int row, Graphics& g, int w, int h, bool selected)
{
    const auto& songs = owner.engine.getSongs();

    int songIndex = -1;
    bool playing = false;
    String prefix;

    if (! isSet)
    {
        songIndex = owner.songAtRow (row);
        playing = songIndex >= 0 && songIndex == owner.engine.currentSongIndex();
    }
    else
    {
        songIndex = owner.songAtSetPosition (row);
        playing = row == owner.engine.getCurrentSongInSet();
        prefix = String (row + 1) + ".  ";
    }
    if (songIndex < 0 || songIndex >= (int) songs.size()) return;
    const auto& sg = songs[(size_t) songIndex];

    // The song being played is the one that has to be findable at a glance.
    g.fillAll (playing ? accentDim : (selected ? Colour (0xff2c3038) : bgRow));

    auto r = Rectangle<int> (0, 0, w, h).reduced (6, 0);


    /* Two lines: the name, and which program each keyboard plays. The second
       line is the answer to "what is in this song" -- without it the panel
       looks like a list of labels rather than a list of rigs. */
    auto nameRow = r.removeFromTop (r.getHeight() / 2 + 1);

    /* The tempo sits beside the name, not beside the programs: the second line
       needs the whole width, and a truncated "001 ..." tells you nothing. */
    if (sg.tempoBpm > 0.0)
    {
        g.setColour (playing ? Colours::white.withAlpha (0.85f) : textDim);
        g.setFont (FontOptions (11.0f));
        g.drawText (String (roundToInt (sg.tempoBpm)) + " bpm",
                    nameRow.removeFromRight (52), Justification::centredRight);
    }

    g.setColour (playing ? Colours::white : Colours::white.withAlpha (0.92f));
    g.setFont (FontOptions (13.0f, playing ? Font::bold : Font::plain));
    g.drawText (prefix + (sg.name.isNotEmpty() ? sg.name : "(unnamed)"),
                nameRow, Justification::centredLeft, true);

    const auto& inputs = owner.engine.getSetup().inputs;
    StringArray parts;
    for (int i = 0; i < (int) inputs.size(); ++i)
    {
        const int prog = sg.programFor (i);
        if (prog < 0 || inputs[(size_t) i].isTrack()) continue;     // this song leaves that keyboard alone; the track's program is the song itself
        const auto& pd = inputs[(size_t) i].programs[(size_t) prog];
        parts.add (String (prog).paddedLeft ('0', 3)
                     + (pd.name.isNotEmpty() ? " " + pd.name : ""));
    }
    if (! sg.charts.empty())
        parts.add (String (CharPointer_UTF8 ("\xe2\x99\xaa")) + (sg.charts.size() > 1 ? " charts" : " chart"));
    g.setColour (playing ? Colours::white.withAlpha (0.8f) : textDim);
    g.setFont (FontOptions (10.5f));
    g.drawText (parts.isEmpty() ? "(no sounds stored yet: press Capture)" : parts.joinIntoString ("   "),
                r, Justification::centredLeft, true);
}

void SongsPanel::ListProxy::listBoxItemClicked (int row, const MouseEvent&)
{
    /* One click loads it. That is what a tap does on the tablet and what a
       click does in the programs list, and a set list whose songs need
       double-clicking would be the odd one out -- on stage, twice is a miss.

       It also selects, so Capture, Rename and Add to set act on what you just
       clicked without a second step. */
    if (isSet)
    {
        owner.selectedInSet = row;
        const auto& sets = owner.engine.getSets();
        const int set = owner.engine.getCurrentSet();
        if (set >= 0 && set < (int) sets.size() && row >= 0 && row < (int) sets[(size_t) set].songs.size())
            owner.selectedSong = sets[(size_t) set].songs[(size_t) row];
    }
    else owner.selectedSong = owner.songAtRow (row);
    play (row);
}

void SongsPanel::ListProxy::play (int row)
{
    /* In the set that means "start here", which also moves the position a
       footswitch will step from; in the song list it is just "load this",
       without touching the set. */
    if (isSet) owner.engine.selectSongInSet (row);
    else       owner.engine.selectSong (owner.songAtRow (row));
    owner.refresh();
}

void SongsPanel::ListProxy::listBoxItemDoubleClicked (int row, const MouseEvent&) { play (row); }
void SongsPanel::ListProxy::returnKeyPressed (int row)                            { play (row); }

int SongsPanel::songAtSetPosition (int position) const
{
    const int set = engine.getCurrentSet();
    const auto& sets = engine.getSets();
    if (set < 0 || set >= (int) sets.size()) return -1;
    const auto& v = sets[(size_t) set].songs;
    return position >= 0 && position < (int) v.size() ? v[(size_t) position] : -1;
}

//==============================================================================
String SongsPanel::describeCurrent() const
{
    /* What is loaded right now, in the same words the song row will use. Shown
       while naming so it is obvious what is about to be stored -- the answer to
       "what is a song?" is easier to see than to read. */
    const auto& inputs = engine.getSetup().inputs;
    StringArray parts;
    for (const auto& in : inputs)
    {
        if (in.isTrack()) continue;
        const auto& prog = in.programs[(size_t) in.currentProgram];
        parts.add (in.name + "  " + String (in.currentProgram).paddedLeft ('0', 3)
                     + "  " + (prog.name.isNotEmpty() ? prog.name : "(empty)"));
    }
    parts.add ("Tempo  " + String (engine.getTempoBpm(), 1) + " bpm");
    return parts.joinIntoString ("\n");
}

void SongsPanel::addSong()
{
    /* Ask for the name when the song is made. It used to be named silently
       after whatever Upper was playing -- so every song was called "B3" until
       you found the Rename button, which is not a thing anyone should have to
       find. */
    String suggested;
    const auto& inputs = engine.getSetup().inputs;
    if (! inputs.empty())
    {
        const auto& prog = inputs[0].programs[(size_t) inputs[0].currentProgram];
        if (prog.name.isNotEmpty()) suggested = prog.name;
    }

    auto* w = new AlertWindow ("New song",
                               "The song will remember what is loaded now:\n\n"
                                 + describeCurrent()
                                 + "\n\nChanging a program later changes every song that uses it.",
                               MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", suggested);
    w->getTextEditor ("name")->setTextToShowWhenEmpty ("song name", textDim);
    w->addTextEditor ("notes", {});
    w->getTextEditor ("notes")->setTextToShowWhenEmpty ("stage notes: key, count-in, a reminder", textDim);
    w->addButton ("Create", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0) return;

        auto name = w->getTextEditorContents ("name").trim();
        if (name.isEmpty()) name = "Song " + String ((int) engine.getSongs().size() + 1);

        selectedSong = engine.addSong (name);
        engine.captureSong (selectedSong);      // a new song remembers what is up now
        const auto notes = w->getTextEditorContents ("notes").trim();
        if (notes.isNotEmpty()) engine.setSongNotes (selectedSong, notes);
        findBox.clear();                        // the new song is shown whatever was being looked for
        refresh();
    }), false);
}

void SongsPanel::removeSong()
{
    const auto& songs = engine.getSongs();
    if (selectedSong < 0 || selectedSong >= (int) songs.size()) return;

    const auto name = songs[(size_t) selectedSong].name;
    auto* w = new AlertWindow ("Delete song", "Delete \"" + name + "\"?\n\n"
                              "It will also be taken out of every set list that uses it.",
                              MessageBoxIconType::QuestionIcon);
    w->addButton ("Delete", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w, idx = selectedSong] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0) return;
        engine.removeSong (idx);
        selectedSong = -1;
        selectedInSet = -1;
        refresh();
    }), false);
}

void SongsPanel::captureIntoSelectedSong()
{
    if (selectedSong < 0 || selectedSong >= (int) engine.getSongs().size()) return;
    engine.captureSong (selectedSong);
    refresh();
}

void SongsPanel::editSelectedSong()
{
    const auto& songs = engine.getSongs();
    if (selectedSong < 0 || selectedSong >= (int) songs.size()) return;
    const auto& sg = songs[(size_t) selectedSong];

    /* What this song currently loads, so the dialog is not a dead end for
       someone who opened it looking for the sounds. They are not editable
       here on purpose: a song stores whatever the rig is set to, so the way
       to change them is to set the rig up and press Capture, and a second way
       of saying it would be a second thing to keep in step. */
    const auto& inputs = engine.getSetup().inputs;
    StringArray loads;
    for (int i = 0; i < (int) inputs.size(); ++i)
    {
        const int prog = sg.programFor (i);
        const auto& in = inputs[(size_t) i];
        if (in.isTrack()) continue;
        if (prog < 0 || prog >= (int) in.programs.size())
        {
            loads.add (in.name + ": left alone");
            continue;
        }
        const auto& pd = in.programs[(size_t) prog];
        loads.add (in.name + ": " + String (prog).paddedLeft ('0', 3)
                     + (pd.name.isNotEmpty() ? " " + pd.name : ""));
    }

    const String whatItLoads =
        loads.isEmpty() ? String ("This song has no sounds stored yet.")
                        : ("Loads " + loads.joinIntoString (", ") + ".");

    auto* w = new AlertWindow ("Edit song",
                               whatItLoads
                                 + "\n\nTo change those, close this, set each keyboard to the sound you "
                                   "want and press Capture.\n\nThe name, and stage notes: anything you want to "
                                   "read on the stand.",
                               MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", sg.name, "Name");
    w->addTextEditor ("notes", sg.notes, "Stage notes");
    w->addTextEditor ("tempo", sg.tempoBpm > 0.0 ? String (sg.tempoBpm, 1) : String(), "Tempo (bpm; blank keeps whatever the tempo is)");
    w->addTextEditor ("transpose", sg.transpose != 0 ? String (sg.transpose) : String(), "Transpose the keyboards (semitones; blank or 0 for none)");
    w->getTextEditor ("name")->setTextToShowWhenEmpty ("song name", textDim);
    w->getTextEditor ("notes")->setTextToShowWhenEmpty ("key, count-in, a reminder", textDim);
    w->getTextEditor ("tempo")->setTextToShowWhenEmpty ("blank = leave the tempo alone", textDim);
    w->getTextEditor ("transpose")->setTextToShowWhenEmpty ("0", textDim);
    StringArray roots { "None  (each Scale and Run keeps its own key)" }, scales;
    for (int i = 0; i < 12; ++i) roots.add (MidiFx::noteNames[i]);
    for (int i = 0; i < MidiFx::numScaleModes(); ++i) scales.add (MidiFx::scaleModes[i]);
    w->addComboBox ("key", roots, "Key (the Scale and Run effects play in it; a chord chart that names its key is shown in it)");
    w->addComboBox ("scale", scales, "Scale");
    w->getComboBoxComponent ("key")->setSelectedItemIndex (jlimit (0, 12, sg.keyRoot + 1), dontSendNotification);
    w->getComboBoxComponent ("scale")->setSelectedItemIndex (jlimit (0, MidiFx::numScaleModes() - 1, sg.keyMode), dontSendNotification);
    w->addButton ("Save", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w, idx = selectedSong] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0) return;
        engine.setSongName (idx, w->getTextEditorContents ("name"));
        engine.setSongNotes (idx, w->getTextEditorContents ("notes"));

        // Blank means "leave the tempo alone", which is not the same as 0 bpm.
        const auto typed = w->getTextEditorContents ("tempo").trim();
        engine.setSongTempo (idx, typed.isEmpty() ? 0.0 : typed.getDoubleValue());
        engine.setSongTranspose (idx, w->getTextEditorContents ("transpose").trim().getIntValue());
        engine.setSongKey (idx, w->getComboBoxComponent ("key")->getSelectedItemIndex() - 1, w->getComboBoxComponent ("scale")->getSelectedItemIndex());
        refresh();
    }), false);
}

//==============================================================================
void SongsPanel::addToSet()
{
    const int set = engine.getCurrentSet();
    const auto& sets = engine.getSets();
    if (set < 0 || set >= (int) sets.size()) return;
    if (selectedSong < 0 || selectedSong >= (int) engine.getSongs().size()) return;

    auto songs = sets[(size_t) set].songs;
    songs.push_back (selectedSong);
    engine.setSetSongs (set, songs);
    selectedInSet = (int) songs.size() - 1;
    refresh();
    setList.selectRow (selectedInSet);
}

void SongsPanel::removeFromSet()
{
    const int set = engine.getCurrentSet();
    const auto& sets = engine.getSets();
    if (set < 0 || set >= (int) sets.size()) return;

    auto songs = sets[(size_t) set].songs;
    if (selectedInSet < 0 || selectedInSet >= (int) songs.size()) return;

    songs.erase (songs.begin() + selectedInSet);
    engine.setSetSongs (set, songs);
    selectedInSet = jmin (selectedInSet, (int) songs.size() - 1);
    refresh();
}

void SongsPanel::moveSongInList (int delta)
{
    if (selectedSong < 0 || selectedSong >= (int) engine.getSongs().size()) return;
    selectedSong = engine.moveSong (selectedSong, delta);
    refresh();
}

void SongsPanel::moveInSet (int delta)
{
    const int set = engine.getCurrentSet();
    const auto& sets = engine.getSets();
    if (set < 0 || set >= (int) sets.size()) return;

    auto songs = sets[(size_t) set].songs;
    const int to = selectedInSet + delta;
    if (selectedInSet < 0 || to < 0 || to >= (int) songs.size()) return;

    std::swap (songs[(size_t) selectedInSet], songs[(size_t) to]);
    engine.setSetSongs (set, songs);
    selectedInSet = to;
    refresh();
    setList.selectRow (to);
}

//==============================================================================
void SongsPanel::newSet()
{
    auto* w = new AlertWindow ("New set list", "Name for this set list:", MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", "Set list " + String ((int) engine.getSets().size() + 1));
    w->addButton ("Create", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0) return;
        const int idx = engine.addSet (w->getTextEditorContents ("name"));
        engine.selectSet (idx);          // a set you just made is the one you are working on
        selectedInSet = -1;
        refresh();
    }), false);
}

void SongsPanel::showSetMenu()
{
    PopupMenu m;
    const auto& sets = engine.getSets();
    const int current = engine.getCurrentSet();

    if (! sets.empty())
    {
        m.addSectionHeader ("Set lists");
        for (int i = 0; i < (int) sets.size(); ++i)
            m.addItem (100 + i, sets[(size_t) i].name, true, i == current);
        m.addSeparator();
    }
    m.addItem (1, "New set list...");
    if (current >= 0)
    {
        m.addItem (2, "Rename this set list...");
        m.addItem (4, "Move up",   current > 0);
        m.addItem (5, "Move down", current < (int) sets.size() - 1);
        m.addItem (3, "Delete this set list");
    }

    m.showMenuAsync (PopupMenu::Options().withTargetComponent (setBtn), [this] (int choice)
    {
        if (choice == 0) return;
        if (choice >= 100)
        {
            engine.selectSet (choice - 100);
            selectedInSet = -1;
            refresh();
            return;
        }
        if (choice == 1) { newSet(); return; }

        const int set = engine.getCurrentSet();
        if (set < 0) return;

        if (choice == 2)
        {
            auto* w = new AlertWindow ("Rename set list", "New name:", MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", engine.getSets()[(size_t) set].name);
            w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
            w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
            w->enterModalState (true, ModalCallbackFunction::create ([this, w, set] (int r)
            {
                std::unique_ptr<AlertWindow> owned (w);
                if (r == 0) return;
                engine.setSetName (set, w->getTextEditorContents ("name"));
                refresh();
            }), false);
        }
        else if (choice == 3)
        {
            const auto& sets = engine.getSets();
            const String name = set >= 0 && set < (int) sets.size() ? sets[(size_t) set].name : String();
            confirm ("Delete set list", "Delete the set list \"" + name + "\"?\n\nThe songs themselves are kept.", "Delete", [this, set]
            {
                engine.removeSet (set);
                selectedInSet = -1;
                refresh();
            });
        }
        else if (choice == 4 || choice == 5)
        {
            engine.moveSet (set, choice == 4 ? -1 : 1);
            refresh();
        }
    });
}

void SongsPanel::showFootswitchMenu()
{
    PopupMenu m;
    const int nextCC = engine.getNextSongCC(), prevCC = engine.getPrevSongCC(), playCC = engine.getTransportCC();

    m.addSectionHeader ("Next song");
    m.addItem (1, "Learn: press the pedal...");
    if (nextCC > 0) m.addItem (2, "Stop using CC " + String (nextCC));

    m.addSectionHeader ("Previous song");
    m.addItem (3, "Learn: press the pedal...");
    if (prevCC > 0) m.addItem (4, "Stop using CC " + String (prevCC));

    m.addSectionHeader ("Backing track: start / stop");
    m.addItem (5, "Learn: press the pedal...");
    if (playCC > 0) m.addItem (6, "Stop using CC " + String (playCC));

    m.addSectionHeader ("Backing track: next section / release vamp");
    m.addItem (7, "Learn: press the pedal...");
    if (engine.getSectionCC() > 0) m.addItem (8, "Stop using CC " + String (engine.getSectionCC()));

    m.addSectionHeader ("Tap tempo");
    m.addItem (9, "Learn: press the pedal...");
    if (engine.getTapTempoCC() > 0) m.addItem (10, "Stop using CC " + String (engine.getTapTempoCC()));

    m.addSectionHeader ("Loop: record / play / stop  (on the keyboard the pedal is on)");
    m.addItem (11, "Learn: press the pedal...");
    if (engine.getLooperCC() > 0) m.addItem (12, "Stop using CC " + String (engine.getLooperCC()));

    m.addSectionHeader ("Loop: clear");
    m.addItem (13, "Learn: press the pedal...");
    if (engine.getLooperClearCC() > 0) m.addItem (14, "Stop using CC " + String (engine.getLooperClearCC()));

    m.showMenuAsync (PopupMenu::Options().withTargetComponent (footBtn), [this] (int choice)
    {
        switch (choice)
        {
            case 1: engine.armSongLearn (true);  break;
            case 2: engine.setNextSongCC (0);    break;
            case 3: engine.armSongLearn (false); break;
            case 4: engine.setPrevSongCC (0);    break;
            case 5: engine.armTransportLearn();  break;
            case 6: engine.setTransportCC (0);   break;
            case 7: engine.armSectionLearn();    break;
            case 8: engine.setSectionCC (0);     break;
            case 9: engine.armTapTempoLearn (true); break;
            case 10: engine.setTapTempoCC (0);   break;
            case 11: engine.armLooperLearn (false); break;
            case 12: engine.setLooperCC (0);     break;
            case 13: engine.armLooperLearn (true); break;
            case 14: engine.setLooperClearCC (0); break;
            default: break;
        }
        refresh();
    });
}

//==============================================================================
void SongsPanel::showChartMenu()
{
    const auto& songs = engine.getSongs();
    if (selectedSong < 0 || selectedSong >= (int) songs.size()) return;
    const auto& sg = songs[(size_t) selectedSong];

    PopupMenu m;
    for (int i = 0; i < (int) sg.charts.size(); ++i)
        m.addItem (200 + i, "Open " + sg.charts[(size_t) i].displayName() + "...");
    if (! sg.charts.empty()) m.addSeparator();
    m.addItem (3, "New chord chart...");
    m.addItem (1, "Attach a file...  (PDF, image, text, ChordPro)");
    if (! sg.charts.empty())
    {
        m.addSeparator();
        for (int i = 0; i < (int) sg.charts.size(); ++i)
            m.addItem (100 + i, "Remove " + sg.charts[(size_t) i].displayName());
    }

    m.showMenuAsync (PopupMenu::Options().withTargetComponent (chartBtn), [this, idx = selectedSong] (int r)
    {
        if (r == 1)
        {
            chooser = std::make_unique<FileChooser> ("Attach a chart",
                                                     File::getSpecialLocation (File::userHomeDirectory),
                                                     chartFilePatterns());
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles
                                    | FileBrowserComponent::canSelectMultipleItems,
                                  [this, idx] (const FileChooser& fc)
            {
                for (const auto& f : fc.getResults()) attachChartFile (f, idx);
            });
        }
        else if (r == 3)
        {
            newChordChart (idx);
        }
        else if (r >= 200)
        {
            showChartViewer (engine, idx, r - 200);
        }
        else if (r >= 100)
        {
            /* The entry goes; the file stays in the folder. Another song may
               use it, and a chart is the kind of thing you want back. */
            engine.removeSongChart (idx, r - 100);
            refresh();
        }
    });
}

void SongsPanel::newChordChart (int songIndex)
{
    /* A typed chart starts from a few lines that show the ChordPro way, in
       the setup's own folder so it travels with the setup; the viewer opens
       on it with the editor beside the page. */
    const auto& songs = engine.getSongs();
    if (songIndex < 0 || songIndex >= (int) songs.size()) return;
    const auto folder = engine.getChartsFolder();
    if (folder == File()) return;
    folder.createDirectory();
    const auto& sg = songs[(size_t) songIndex];
    const File dest = folder.getNonexistentChildFile (File::createLegalFileName (sg.name.isNotEmpty() ? sg.name : "Chart"), ".cho", false);
    // Its key, so the chart can follow the song's: a chart that names no key is shown as written.
    const String key = sg.hasKey() ? noteNameFor (sg.keyRoot, MidiFx::keyPrefersFlats (sg.keyRoot, sg.keyMode)) + (MidiFx::keyIsMinor (sg.keyMode) ? "m" : "") : String ("C");
    const String text = "{title: " + sg.name + "}\n{key: " + key + "}\n{subtitle: tempo, who starts}\n\n"
                        "{comment: Intro}\n[C]    [G]    [Am]    [F]\n\n"
                        "{comment: Verse}\n[C]Chords go in square brackets [G]before the syllable they [Am]fall on, like [F]this.\n"
                        "A line without brackets is just words.\n";
    if (! dest.replaceWithText (text)) return;
    engine.addSongChart (songIndex, { dest.getFileName(), {} });
    refresh();
    showChartViewer (engine, songIndex, (int) engine.getSongs()[(size_t) songIndex].charts.size() - 1);
}

void SongsPanel::attachChartFile (const File& f, int songIndex)
{
    if (songIndex < 0 || songIndex >= (int) engine.getSongs().size()) return;
    if (! f.existsAsFile() || ! isChartFile (f)) return;

    const auto folder = engine.getChartsFolder();
    if (folder == File()) return;
    folder.createDirectory();

    /* Copied in rather than referred to, so the setup and its charts travel
       together. A name already in use gets a number -- unless it is the same
       file again, which just gets listed. */
    File dest = folder.getChildFile (f.getFileName());
    if (dest.existsAsFile() && ! dest.hasIdenticalContentTo (f))
        dest = folder.getNonexistentChildFile (f.getFileNameWithoutExtension(), f.getFileExtension(), true);
    if (! dest.existsAsFile() && ! f.copyFileTo (dest)) return;

    engine.addSongChart (songIndex, { dest.getFileName(), {} });
    if (chartKindFor (dest) == "pdf")
        prerenderPdfPagesAsync (dest);      // pages appear on the tablet as they are made
    refresh();
}

bool SongsPanel::isInterestedInFileDrag (const StringArray& files)
{
    for (const auto& p : files)
        if (isChartFile (File (p)) || isTrackFile (File (p))) return true;
    return false;
}

void SongsPanel::filesDropped (const StringArray& files, int x, int y)
{
    /* Dropped on a song: that song. Dropped on the set: the song at that
       position. Anywhere else: the selected song. */
    const auto& songs = engine.getSongs();
    int song = -1;
    if (songsList.getBounds().contains (x, y))
        song = songAtRow (songsList.getRowContainingPosition (x - songsList.getX(), y - songsList.getY()));
    else if (setList.getBounds().contains (x, y))
    {
        const int pos = setList.getRowContainingPosition (x - setList.getX(), y - setList.getY());
        const int set = engine.getCurrentSet();
        const auto& sets = engine.getSets();
        if (set >= 0 && set < (int) sets.size() && pos >= 0 && pos < (int) sets[(size_t) set].songs.size())
            song = sets[(size_t) set].songs[(size_t) pos];
    }
    if (song < 0 || song >= (int) songs.size()) song = selectedSong;
    if (song < 0 || song >= (int) songs.size()) return;

    for (const auto& p : files)
    {
        const File f (p);
        if (isTrackFile (f)) addTrackFile (f, song);
        else                 attachChartFile (f, song);
    }
}

//==============================================================================
void SongsPanel::addTrackFile (const File& f, int songIndex)
{
    if (songIndex < 0 || songIndex >= (int) engine.getSongs().size()) return;
    if (! f.existsAsFile() || ! isTrackFile (f)) return;
    const auto folder = engine.getChartsFolder();
    if (folder == File()) return;
    folder.createDirectory();

    // Copied beside the setup, like a chart, so the two travel together.
    File dest = folder.getChildFile (f.getFileName());
    if (dest.existsAsFile() && ! dest.hasIdenticalContentTo (f))
        dest = folder.getNonexistentChildFile (f.getFileNameWithoutExtension(), f.getFileExtension(), true);
    if (! dest.existsAsFile() && ! f.copyFileTo (dest)) return;

    StemDef d;
    d.file = dest.getFileName();
    engine.addSongStem (songIndex, d);
    // The song you are building is the one Play should play.
    if (engine.getTrackSong() != songIndex) engine.selectSong (songIndex);
    refresh();
}

/** A song's backing track: its stems, each with an output and a level, and
    the count-in. A window of its own, because it is set up once per song. */
class TracksComponent : public Component,
                        private Timer
{
public:
    TracksComponent (Engine& e, SongsPanel& p, int song) : engine (e), panel (p), songIndex (song)
    {
        addAndMakeVisible (addBtn);
        addBtn.setTooltip ("Audio files (click, cue, stems) play on an output; a MIDI file plays parts on your instruments");
        addBtn.onClick = [this]
        {
            chooser = std::make_unique<FileChooser> ("Add audio or MIDI files to the backing track",
                                                     File::getSpecialLocation (File::userHomeDirectory),
                                                     trackFilePatterns());
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles
                                    | FileBrowserComponent::canSelectMultipleItems,
                                  [this] (const FileChooser& fc)
            {
                for (const auto& f : fc.getResults()) panel.addTrackFile (f, songIndex);
                rebuild();
            });
        };

        addAndMakeVisible (tempoLabel);
        tempoLabel.setColour (Label::textColourId, textDim);
        addAndMakeVisible (tempoText);
        tempoText.setTooltip ("The tempo the click, the count-in and the bar lines use. Edit... on the song sets it by hand.");
        addAndMakeVisible (findTempoBtn);
        findTempoBtn.setTooltip ("Listens to the song's longest audio file and finds its tempo and where its first beat falls. "
                                 "For a recording with a steady beat; a track that speeds up or slows down gets the tempo it mostly keeps.");
        findTempoBtn.onClick = [this] { findTempo(); };
        addAndMakeVisible (fileBpmLabel);
        fileBpmLabel.setColour (Label::textColourId, textDim);
        addAndMakeVisible (fileBpmText);
        fileBpmText.setEditable (true);
        fileBpmText.setJustificationType (Justification::centredRight);
        fileBpmText.setTooltip ("The tempo the audio files were made at (Find the tempo fills it in). With the song's own tempo set to "
                                "something else, the files are stretched to play at the song's tempo. Blank: unknown, so they play as they are.");
        fileBpmText.onTextChange = [this]
        {
            engine.setSongTrackBpm (songIndex, fileBpmText.getText().trim().getDoubleValue());
            refreshTempoRow();
        };
        addAndMakeVisible (transposeTrackBtn);
        transposeTrackBtn.setTooltip ("The song's transpose (Edit... on the song) also moves the files' pitch, and the MIDI files' notes "
                                      "(drums aside): one setting takes the whole band down a step. The files are re-read when it changes.");
        transposeTrackBtn.onClick = [this] { engine.setSongTransposeTrack (songIndex, transposeTrackBtn.getToggleState()); refreshTempoRow(); };
        addAndMakeVisible (halfBtn);
        addAndMakeVisible (doubleBtn);
        halfBtn.setTooltip ("Half the tempo: when the band counts it slower than the detector did");
        doubleBtn.setTooltip ("Double the tempo: when the band counts it faster than the detector did");
        halfBtn.onClick = [this] { scaleTempo (0.5); };
        doubleBtn.onClick = [this] { scaleTempo (2.0); };
        addAndMakeVisible (firstBeatLabel);
        firstBeatLabel.setColour (Label::textColourId, textDim);
        addAndMakeVisible (firstBeatText);
        firstBeatText.setEditable (true);
        firstBeatText.setJustificationType (Justification::centredRight);
        firstBeatText.setTooltip ("Where beat 1 of a bar first falls in the files (minutes:seconds). 0:00.00 when the recording starts on the beat. "
                                  "The click, the count-in and the bar lines line up with it.");
        firstBeatText.onTextChange = [this]
        {
            engine.setSongFirstBeat (songIndex, SectionRow::parseTime (firstBeatText.getText()));
            refreshTempoRow();
        };
        addAndMakeVisible (beatBackBtn);
        addAndMakeVisible (beatFwdBtn);
        beatBackBtn.setTooltip ("Move the first beat one beat earlier: when the accent falls on the wrong beat of the bar");
        beatFwdBtn.setTooltip ("Move the first beat one beat later: when the accent falls on the wrong beat of the bar");
        beatBackBtn.onClick = [this] { nudgeFirstBeat (-1); };
        beatFwdBtn.onClick = [this] { nudgeFirstBeat (1); };

        addAndMakeVisible (countInLabel);
        countInLabel.setColour (Label::textColourId, textDim);
        addAndMakeVisible (countInBox);
        countInBox.addItem ("No count-in", 1);
        for (int b = 1; b <= 8; ++b) countInBox.addItem (String (b) + (b == 1 ? " beat" : " beats"), b + 1);
        countInBox.setTooltip ("Clicks before the track starts, at the song's tempo");
        countInBox.onChange = [this] { applyCountIn(); };
        addAndMakeVisible (clickLabel);
        clickLabel.setColour (Label::textColourId, textDim);
        addAndMakeVisible (clickBox);
        clickBox.addItem ("No click", 1);
        clickBox.addItem ("Click through the song", 2);
        clickBox.setTooltip ("A metronome through the whole track at the song's tempo, on the same output as the count-in. "
                             "With no files, Play is a plain metronome.");
        clickBox.onChange = [this] { applyCountIn(); };
        addAndMakeVisible (barBox);
        for (int b = 2; b <= 7; ++b) barBox.addItem (String (b) + " beats per bar", b);
        barBox.setTooltip ("The click is accented on beat 1 of every bar");
        barBox.onChange = [this] { applyCountIn(); };
        addAndMakeVisible (startLabel);
        startLabel.setColour (Label::textColourId, textDim);
        addAndMakeVisible (startText);
        startText.setEditable (true);
        startText.setJustificationType (Justification::centredRight);
        startText.setTooltip ("Where Play from the top begins (minutes:seconds). Blank: at the first sound, skipping silence at the head of the files. "
                              "Set it past a lead-in you do not want to hear, such as a count-in recorded in the file; or right-click the timeline there.");
        startText.onTextChange = [this]
        {
            engine.setSongStart (songIndex, startText.getText().trim().isEmpty() ? 0.0 : SectionRow::parseTime (startText.getText()));
            refreshTempoRow();
        };
        addAndMakeVisible (countInOutLabel);
        countInOutLabel.setColour (Label::textColourId, textDim);
        addAndMakeVisible (countInOutBox);
        countInOutBox.setTooltip ("Which output hears the count-in and the click (your in-ears)");
        countInOutBox.onChange = [this] { applyCountIn(); };

        addAndMakeVisible (sectionsHeader);
        sectionsHeader.setColour (Label::textColourId, accent);
        sectionsHeader.setFont (FontOptions (13.0f, Font::bold));

        timeline = std::make_unique<TrackTimeline> (engine, songIndex);
        addAndMakeVisible (*timeline);
        timeline->onMarkersChanged = [this] { rebuild(); };
        timeline->onEditActions = [this] (int marker) { editActions (marker); };
        timeline->onHint = [this] (const String& t) { timelineHint.setText (t.isNotEmpty() ? t : kTimelineHint, dontSendNotification); };
        addAndMakeVisible (timelineHint);
        timelineHint.setText (kTimelineHint, dontSendNotification);
        timelineHint.setColour (Label::textColourId, textDim);
        timelineHint.setFont (FontOptions (11.5f));
        timelineHint.setMinimumHorizontalScale (0.8f);

        addAndMakeVisible (playBtn);
        playBtn.setTooltip ("Play the backing track from the green cue (click the timeline to move it). Space does the same.");
        playBtn.onClick = [this] { timeline->togglePlay(); };
        addAndMakeVisible (addSectionBtn);
        addSectionBtn.setTooltip ("A new section where the track is playing now, or at the green cue while it is stopped. "
                                  "Or double-click the timeline where the section should start.");
        addSectionBtn.onClick = [this] { timeline->markHere(); };
        addAndMakeVisible (zoomOutBtn);
        addAndMakeVisible (zoomInBtn);
        addAndMakeVisible (fitBtn);
        zoomOutBtn.setTooltip ("Zoom out (or Ctrl+wheel on the timeline)");
        zoomInBtn.setTooltip ("Zoom in (or Ctrl+wheel on the timeline)");
        fitBtn.setTooltip ("Show the whole song");
        zoomOutBtn.onClick = [this] { timeline->zoomBy (1.0f / 1.5f); };
        zoomInBtn.onClick = [this] { timeline->zoomBy (1.5f); };
        fitBtn.onClick = [this] { timeline->zoomToFit(); };

        addAndMakeVisible (hint);
        hint.setColour (Label::textColourId, textDim);
        hint.setFont (FontOptions (12.0f));
        hint.setJustificationType (Justification::topLeft);
        hint.setText ("Audio files play on an output; put a click or cue file on your in-ear output. "
                      "A MIDI file plays parts: click a part's sound to choose what plays it. "
                      "Play (songs panel, tablet or footswitch) starts everything together, after the count-in; "
                      "the click is a metronome through the song for the in-ears.\n"
                      "Sections: double-click the timeline where one starts, or press Mark section here while the track plays. "
                      "Each runs to the next. A vamp section repeats until released (Next, the tablet, or the section footswitch); "
                      "asking for a section takes you there when the current one ends.", dontSendNotification);

        rebuild();
        startTimerHz (5);
    }

    ~TracksComponent() override
    {
        if (detectStop != nullptr) detectStop->store (true);
        if (detectThread.joinable()) detectThread.join();
    }

    void rebuild()
    {
        rows.clear();
        const auto& songs = engine.getSongs();
        if (songIndex < 0 || songIndex >= (int) songs.size()) return;
        const auto& sg = songs[(size_t) songIndex];

        for (int i = 0; i < (int) sg.stems.size(); ++i)
        {
            auto row = std::make_unique<Row> (*this, i, sg.stems[(size_t) i]);
            addAndMakeVisible (row.get());
            rows.push_back (std::move (row));
        }

        if (timeline != nullptr) timeline->refresh();
        sectionRows.clear();
        for (int i = 0; i < (int) sg.markers.size(); ++i)
        {
            auto row = std::make_unique<SectionRow> (*this, i, sg.markers[(size_t) i]);
            addAndMakeVisible (row.get());
            sectionRows.push_back (std::move (row));
        }

        refreshTempoRow();
        fillOutputs (countInOutBox, sg.countInOutput);
        countInBox.setSelectedId (sg.countInBeats + 1, dontSendNotification);
        clickBox.setSelectedId (sg.click ? 2 : 1, dontSendNotification);
        barBox.setSelectedId (jlimit (2, 7, sg.beatsPerBar), dontSendNotification);
        barBox.setEnabled (sg.click);
        countInOutBox.setEnabled (sg.countInBeats > 0 || sg.click);
        resized();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        for (auto& row : rows) row->setBounds (r.removeFromTop (row->preferredHeight()));
        r.removeFromTop (6);
        addBtn.setBounds (r.removeFromTop (26).removeFromLeft (180));
        r.removeFromTop (12);
        sectionsHeader.setBounds (r.removeFromTop (20));
        if (timeline != nullptr)
        {
            // The timeline takes whatever height the rest leaves: a bigger window shows more of the music.
            const int below = 4 + 26 + 6 + 28 * (int) sectionRows.size() + 12 + 24 + 6 + 24 + 6 + 24 + 10 + 60;
            timeline->setBounds (r.removeFromTop (jmax (timeline->preferredHeight(), r.getHeight() - below)));
        }
        r.removeFromTop (4);
        {
            auto b = r.removeFromTop (26);
            playBtn.setBounds (b.removeFromLeft (70));          b.removeFromLeft (6);
            addSectionBtn.setBounds (b.removeFromLeft (150));   b.removeFromLeft (14);
            zoomOutBtn.setBounds (b.removeFromLeft (30));       b.removeFromLeft (4);
            zoomInBtn.setBounds (b.removeFromLeft (30));        b.removeFromLeft (4);
            fitBtn.setBounds (b.removeFromLeft (40));           b.removeFromLeft (10);
            timelineHint.setBounds (b);
        }
        r.removeFromTop (6);
        for (auto& row : sectionRows) row->setBounds (r.removeFromTop (28));
        r.removeFromTop (12);
        auto c = r.removeFromTop (24);
        tempoLabel.setBounds (c.removeFromLeft (50));
        tempoText.setBounds (c.removeFromLeft (190));
        findTempoBtn.setBounds (c.removeFromLeft (170));   c.removeFromLeft (4);
        halfBtn.setBounds (c.removeFromLeft (30));          c.removeFromLeft (4);
        doubleBtn.setBounds (c.removeFromLeft (30));        c.removeFromLeft (10);
        fileBpmLabel.setBounds (c.removeFromLeft (50));
        fileBpmText.setBounds (c.removeFromLeft (52));      c.removeFromLeft (10);
        firstBeatLabel.setBounds (c.removeFromLeft (60));
        firstBeatText.setBounds (c.removeFromLeft (64));   c.removeFromLeft (4);
        beatBackBtn.setBounds (c.removeFromLeft (50));     c.removeFromLeft (4);
        beatFwdBtn.setBounds (c.removeFromLeft (50));
        r.removeFromTop (6);
        c = r.removeFromTop (24);
        countInLabel.setBounds (c.removeFromLeft (70));
        countInBox.setBounds (c.removeFromLeft (130));
        c.removeFromLeft (16);
        clickLabel.setBounds (c.removeFromLeft (40));
        clickBox.setBounds (c.removeFromLeft (190));
        c.removeFromLeft (8);
        barBox.setBounds (c.removeFromLeft (150));
        r.removeFromTop (6);
        c = r.removeFromTop (24);
        countInOutLabel.setBounds (c.removeFromLeft (70));
        countInOutBox.setBounds (c.removeFromLeft (240));
        c.removeFromLeft (16);
        startLabel.setBounds (c.removeFromLeft (66));
        startText.setBounds (c.removeFromLeft (90));
        c.removeFromLeft (16);
        transposeTrackBtn.setBounds (c.removeFromLeft (250));
        r.removeFromTop (10);
        hint.setBounds (r.removeFromTop (60));
    }

private:
    struct Row : public Component
    {
        Row (TracksComponent& o, int i, const StemDef& d) : owner (o), index (i), def (d)
        {
            addAndMakeVisible (name);
            addChildComponent (partsCap);
            partsCap.setText ("Each part of the file: what plays it, its level (0 dB = as the file has it), and M to mute it", dontSendNotification);
            partsCap.setFont (FontOptions (11.0f));
            partsCap.setColour (Label::textColourId, textDim);
            name.setText (d.displayName(), dontSendNotification);
            name.setEditable (true);
            name.setTooltip (d.file);
            name.onTextChange = [this] { def.name = name.getText().trim(); owner.engine.setSongStem (owner.songIndex, index, def); };

            if (d.isMidi())
            {
                /* A MIDI file is another player in the band: it plays parts,
                   and each part plays on an instrument. The parts are listed
                   here with what plays them; the notes have a window of their own. */
                addAndMakeVisible (notes);
                notes.setTooltip ("The notes: a piano roll to move, add and delete them, and change velocities");
                notes.onClick = [this] { owner.showChannelRoutes (index); };
                String err;
                if (doc.load (owner.engine.getChartsFolder().getChildFile (d.file), err))
                    for (int ch = 1; ch <= 16; ++ch)
                        if (doc.channelUsed (ch)) addPart (ch);
                if (parts.empty())
                {
                    addAndMakeVisible (empty);
                    empty.setColour (Label::textColourId, err.isNotEmpty() ? Colours::orange : textDim);
                    empty.setText (err.isNotEmpty() ? err : String ("No notes in this file"), dontSendNotification);
                }
                else if (const auto why = GmSynth::unavailableReason (owner.engine.getGmSoundfont()); why.isNotEmpty())
                {
                    addAndMakeVisible (empty);
                    empty.setColour (Label::textColourId, Colours::orange);
                    empty.setText ("No General MIDI sounds: " + why + ". Give each part one of your own instruments.", dontSendNotification);
                }
                refreshParts();
            }
            else
            {
                addAndMakeVisible (output);
                output.setTooltip ("Which output this file plays on");
                owner.fillOutputs (output, d.output);
                output.onChange = [this] { def.output = output.getSelectedId() - 1; owner.engine.setSongStem (owner.songIndex, index, def); };

                addAndMakeVisible (gain);
                gain.setRange (-60.0, 12.0, 0.5);
                gain.setSliderStyle (Slider::LinearBar);
                gain.setTextValueSuffix (" dB");
                gain.setDoubleClickReturnValue (true, 0.0);
                gain.setTooltip ("This file's level. Double-click for 0 dB");
                gain.setValue (d.gainDb, dontSendNotification);
                gain.onValueChange = [this] { def.gainDb = (float) gain.getValue(); owner.engine.setSongStem (owner.songIndex, index, def); };
            }
            addAndMakeVisible (remove);
            remove.setTooltip ("Take this file off the song. It stays in the setup's folder.");
            remove.onClick = [this] { owner.engine.removeSongStem (owner.songIndex, index); MessageManager::callAsync ([o = &owner] { o->rebuild(); }); };
        }

        int preferredHeight() const { return 30 + (parts.empty() ? 0 : 18) + (int) parts.size() * 28 + (def.isMidi() && empty.isVisible() ? 24 : 0); }

        void resized() override
        {
            auto all = getLocalBounds();
            auto r = all.removeFromTop (30).reduced (0, 2);
            remove.setBounds (r.removeFromRight (28));     r.removeFromRight (6);
            if (def.isMidi())
                notes.setBounds (r.removeFromRight (100));
            else
            {
                gain.setBounds (r.removeFromRight (110));
                r.removeFromRight (6);
                output.setBounds (r.removeFromRight (180));
            }
            r.removeFromRight (6);
            name.setBounds (r);
            if (parts.empty() && empty.isVisible()) empty.setBounds (all.removeFromTop (24).withTrimmedLeft (24));
            partsCap.setVisible (! parts.empty());
            if (! parts.empty()) partsCap.setBounds (all.removeFromTop (18).withTrimmedLeft (24));
            for (auto& p : parts)
            {
                auto line = all.removeFromTop (28).reduced (0, 3);
                line.removeFromLeft (24);
                p->mute.setBounds (line.removeFromRight (28));  line.removeFromRight (6);
                p->level.setBounds (line.removeFromRight (100)); line.removeFromRight (6);
                p->open.setBounds (line.removeFromRight (64));  line.removeFromRight (6);
                p->plays.setBounds (line.removeFromRight (jmin (300, line.getWidth() / 2))); line.removeFromRight (6);
                p->label.setBounds (line);
            }
            if (! parts.empty() && empty.isVisible()) empty.setBounds (all.removeFromTop (24).withTrimmedLeft (24));
        }

        void paint (Graphics& g) override
        {
            if (parts.empty()) return;
            g.setColour (textDim.withAlpha (0.35f));
            g.drawVerticalLine (12, 30.0f, (float) getHeight() - 4.0f);
        }

        // --- the parts -----------------------------------------------------------
        struct Part
        {
            int channel = 1;
            Label label;
            TextButton plays, mute { "M" }, open;     // open: the plugin's window, or a MIDI device's patch
            Slider level;
        };

        void addPart (int ch)
        {
            auto p = std::make_unique<Part>();
            p->channel = ch;
            const auto partName = doc.partName (ch);
            p->label.setText ("Ch " + String (ch) + "   " + (partName.isNotEmpty() ? partName : (ch == 10 ? String ("Drums") : String (GmSynth::programName (doc.firstProgram (ch))))), dontSendNotification);
            p->label.setFont (FontOptions (13.0f));
            p->plays.setTooltip ("What plays this part");
            p->plays.onClick = [this, ch] { choosePlays (ch); };
            p->level.setSliderStyle (Slider::LinearBar);
            p->level.setRange (-60.0, 12.0, 0.5);
            p->level.setTextValueSuffix (" dB");
            p->level.setTooltip ("This part's level. 0 dB is as the file has it; double-click to go back there");
            p->level.onValueChange = [this, ch] { setLevel (ch, (float) partFor (ch)->level.getValue()); };
            p->mute.setClickingTogglesState (true);
            p->mute.setColour (TextButton::buttonOnColourId, Colours::orangered);
            p->mute.setTooltip ("Mute this part");
            p->mute.onClick = [this, ch] { setMuted (ch, partFor (ch)->mute.getToggleState()); };
            p->open.onClick = [this, ch] { openPart (ch); };
            for (auto* c : std::initializer_list<Component*> { &p->label, &p->plays, &p->level, &p->mute })
                addAndMakeVisible (c);
            addChildComponent (p->open);
            parts.push_back (std::move (p));
        }

        Part* partFor (int ch) { for (auto& p : parts) if (p->channel == ch) return p.get(); return nullptr; }

        int track() const      { return owner.engine.getTrackInput(); }
        int program() const    { const auto& songs = owner.engine.getSongs(); return owner.songIndex < (int) songs.size() ? songs[(size_t) owner.songIndex].programFor (track()) : -1; }
        const ProgramDef* programDef() const
        {
            const auto& inputs = owner.engine.getSetup().inputs;
            const int t = track(), p = program();
            return t >= 0 && t < (int) inputs.size() && p >= 0 ? &inputs[(size_t) t].programs[(size_t) p] : nullptr;
        }
        const SlotDef* instrumentFor (int ch, int* indexOut = nullptr) const
        {
            auto* pd = programDef();
            if (pd == nullptr) return nullptr;
            const int sl = owner.engine.partInstrument (track(), program(), ch);
            if (indexOut != nullptr) *indexOut = sl;
            return sl >= 0 ? &pd->slots[(size_t) sl] : nullptr;
        }

        /** A GM part's level is relative to what the file asks for: 0 dB is
            the file's own CC 7 (100 when it sets none), by a fader's law. */
        int fileCc (int ch) const      { const int v = doc.firstController (ch, 7); return v > 0 ? v : 100; }
        static float ccToDb (int cc, int base) { return cc <= 0 ? -60.0f : jmax (-60.0f, 40.0f * std::log10 ((float) cc / (float) base)); }
        static int   dbToCc (float dB, int base) { return jlimit (0, 127, (int) std::lround ((float) base * std::pow (10.0f, dB / 40.0f))); }

        void refreshParts()
        {
            for (auto& p : parts)
            {
                const auto* sd = instrumentFor (p->channel);
                p->plays.setButtonText (partInstrumentName (owner.engine, track(), program(), p->channel, doc.firstProgram (p->channel)));
                const bool loading = sd != nullptr && ! sd->isGm() && owner.engine.isPluginLoading (track(), program(), owner.engine.partInstrument (track(), program(), p->channel));
                p->plays.setColour (TextButton::textColourOffId, sd == nullptr || loading ? textDim : Colours::white);
                p->level.setEnabled (sd != nullptr);
                p->mute.setEnabled (sd != nullptr);
                // A plugin has a window to open; a MIDI device has a patch to send.
                p->open.setVisible (sd != nullptr && (sd->isPlugin() || sd->isExternal()));
                if (sd != nullptr && sd->isPlugin()) { p->open.setButtonText (loading ? "Loading" : "Window"); p->open.setEnabled (! loading); }
                if (sd != nullptr && sd->isExternal()) { p->open.setButtonText ("Patch recall..."); p->open.setEnabled (true); }
                if (sd == nullptr) { p->mute.setToggleState (false, dontSendNotification); continue; }
                if (sd->isGm())
                {
                    const auto& gc = sd->gmChannels[(size_t) p->channel - 1];
                    p->level.setDoubleClickReturnValue (true, 0.0);
                    p->level.setValue (gc.volume >= 0 ? ccToDb (gc.volume, fileCc (p->channel)) : 0.0f, dontSendNotification);
                    p->mute.setToggleState (owner.engine.isPartMuted (track(), program(), p->channel), dontSendNotification);
                }
                else
                {
                    p->level.setDoubleClickReturnValue (true, 0.0);
                    p->level.setValue (sd->gainDb, dontSendNotification);
                    p->mute.setToggleState (owner.engine.isPartMuted (track(), program(), p->channel), dontSendNotification);
                }
            }
        }

        void setLevel (int ch, float dB)
        {
            int sl = -1;
            const auto* sd = instrumentFor (ch, &sl);
            if (sd == nullptr) return;
            if (sd->isGm())
            {
                auto gc = sd->gmChannels[(size_t) ch - 1];
                gc.volume = std::abs (dB) < 0.01f ? -1 : dbToCc (dB, fileCc (ch));      // at 0 dB the file has it
                owner.engine.setGmChannel (track(), program(), sl, ch, gc);
            }
            else
                owner.engine.setSlotGainDb (track(), program(), sl, dB);
        }

        void setMuted (int ch, bool muted)   { owner.engine.setPartMuted (track(), program(), ch, muted); }

        void openPart (int ch)
        {
            int sl = -1;
            const auto* sd = instrumentFor (ch, &sl);
            if (sd == nullptr) return;
            auto& engine = owner.engine;
            if (sd->isPlugin())
            {
                auto* plugin = engine.getPlugin (track(), program(), sl);
                if (plugin == nullptr || ! plugin->isAlive()) return;
                const auto& songs = engine.getSongs();
                plugin->showEditor ((owner.songIndex < (int) songs.size() ? songs[(size_t) owner.songIndex].name + " / " : String()) + sd->displayName());
            }
            else if (sd->isExternal())
            {
                const int t = track(), p = program();
                Component::SafePointer<Row> self (this);
                showPatchRecallDialog (sd->midiOutName, sd->bankMsb, sd->bankLsb, sd->patch, [self, &engine, t, p, sl] (int msb, int lsb, int patch)
                {
                    engine.setSlotPatch (t, p, sl, msb, lsb, patch);
                    if (self != nullptr) self->refreshParts();
                });
            }
        }

        /** What plays this part: a General MIDI instrument, one of your
            programs (by keyboard, then by name), a MIDI device straight from
            its port, something already in the song's program, or nothing.
            Your programs are where your sounds are -- plugins, hardware,
            splits and layers, all set up -- so the menu offers those rather
            than a plugin from scratch; a device is offered for the things
            that are not a program, lights above all. */
        void choosePlays (int ch)
        {
            auto& engine = owner.engine;
            const int p = engine.ensureTrackProgram (owner.songIndex);
            const int t = engine.getTrackInput();
            if (p < 0 || t < 0) return;
            const auto& prog = engine.getSetup().inputs[(size_t) t].programs[(size_t) p];
            const int current = engine.partInstrument (t, p, ch);
            const auto* cur = current >= 0 ? &prog.slots[(size_t) current] : nullptr;
            const int fileProgram = doc.firstProgram (ch);

            PopupMenu m;
            const bool gmNow = cur != nullptr && cur->isGm();
            const int gmChoice = gmNow ? cur->gmChannels[(size_t) ch - 1].program : -2;
            m.addItem (2, (ch == 10 ? gmKitName (fileProgram) : String (GmSynth::programName (fileProgram))) + "   (what the file asks for, General MIDI)",
                       GmSynth::isAvailable(), gmChoice == -1);
            {
                PopupMenu gm;
                if (ch == 10) { addGmKitsToMenu (gm, 1000, gmChoice); gm.addSeparator(); }
                addGmInstrumentsToMenu (gm, 1000, gmChoice);
                m.addSubMenu (ch == 10 ? "Another General MIDI kit or instrument" : "Another General MIDI instrument", gm, GmSynth::isAvailable(), nullptr, gmChoice >= 0);
            }
            m.addSeparator();
            /* Each keyboard's programs, as its program pages have them
               (grouped when they are), then whatever it happens to be playing. */
            const auto& inputs = engine.getSetup().inputs;
            const bool keysNow = cur != nullptr && cur->isInput();
            for (int i = 0; i < (int) inputs.size(); ++i)
            {
                const auto& in = inputs[(size_t) i];
                if (in.isAudio() || in.isTrack()) continue;
                PopupMenu programs;
                String group;
                bool any = false;
                for (int k = 0; k < (int) in.programs.size(); ++k)
                {
                    const auto& pd = in.programs[(size_t) k];
                    if (pd.isEmpty() && pd.name.isEmpty()) continue;
                    if (pd.group.isNotEmpty() && pd.group != group) { programs.addSectionHeader (pd.group); group = pd.group; }
                    programs.addItem (10000 + i * 128 + k, String (k).paddedLeft ('0', 3) + "  " + (pd.name.isNotEmpty() ? pd.name : String ("(unnamed)")),
                                      true, keysNow && cur->targetInput == i && cur->targetProgram == k);
                    any = true;
                }
                if (! any) programs.addItem (9999, "No programs yet: select that keyboard under INPUTS and name a program in the Programs list", false);
                programs.addSeparator();
                programs.addItem (2000 + i, "Whatever " + in.name + " is playing", true, keysNow && cur->targetInput == i && cur->targetProgram < 0);
                m.addSubMenu (in.name + "'s programs", programs, true, nullptr, keysNow && cur->targetInput == i);
            }
            // A MIDI device straight from the port: a hardware synth or module, a lighting controller.
            Array<MidiDeviceInfo> ports;
            for (const auto& d : MidiOutput::getAvailableDevices()) if (! d.name.containsIgnoreCase ("Midi Through")) ports.add (d);
            {
                PopupMenu devices;
                for (int k = 0; k < ports.size(); ++k) devices.addItem (4000 + k, ports[k].name);
                if (ports.isEmpty()) devices.addItem (3999, "No MIDI interface connected", false);
                m.addSubMenu ("A MIDI device (hardware synth, lights)", devices);
            }
            bool header = false;
            for (int sl = 0; sl < (int) prog.slots.size(); ++sl)
            {
                const auto& sd = prog.slots[(size_t) sl];
                if (sd.isGm() || sd.isInput()) continue;
                if (! header) { m.addSectionHeader ("Already in this song"); header = true; }
                m.addItem (100 + sl, sd.displayName(), true, sl == current);
            }
            m.addSeparator();
            m.addItem (1, "Nothing", true, cur == nullptr);

            Component::SafePointer<Row> self (this);
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (partFor (ch)->plays), [self, ch, t, p, ports] (int r)
            {
                if (self == nullptr || r < 1) return;
                auto* thisRow = self.getComponent();
                auto& e = thisRow->owner.engine;
                auto slotsNow = [&] { return (int) e.getSetup().inputs[(size_t) t].programs[(size_t) p].slots.size(); };
                auto gmSlot = [&]
                {
                    const auto& slots = e.getSetup().inputs[(size_t) t].programs[(size_t) p].slots;
                    for (int sl = 0; sl < (int) slots.size(); ++sl) if (slots[(size_t) sl].isGm()) return sl;
                    e.addGmSlot (t, p);
                    return slotsNow() - 1;
                };
                if (r == 1) e.assignPart (t, p, ch, -1);
                else if (r == 2 || (r >= 1000 && r < 1128))
                {
                    const int g = gmSlot();
                    e.assignPart (t, p, ch, g);
                    auto gc = e.getSetup().inputs[(size_t) t].programs[(size_t) p].slots[(size_t) g].gmChannels[(size_t) ch - 1];
                    gc.program = r == 2 ? -1 : r - 1000;
                    e.setGmChannel (t, p, g, ch, gc);
                }
                else if (r >= 100 && r < 1000) e.assignPart (t, p, ch, r - 100);
                else if ((r >= 2000 && r < 3000) || r >= 10000)
                {
                    // A keyboard's program, or whatever it is playing: one slot per choice, shared by the parts that make it.
                    const int target = r >= 10000 ? (r - 10000) / 128 : r - 2000;
                    const int targetProgram = r >= 10000 ? (r - 10000) % 128 : -1;
                    const auto& slots = e.getSetup().inputs[(size_t) t].programs[(size_t) p].slots;
                    int sl = -1;
                    for (int k = 0; k < (int) slots.size(); ++k)
                        if (slots[(size_t) k].isInput() && slots[(size_t) k].targetInput == target && slots[(size_t) k].targetProgram == targetProgram) sl = k;
                    if (sl < 0 && e.addInputSlot (t, p, target, targetProgram)) sl = slotsNow() - 1;
                    if (sl >= 0) e.assignPart (t, p, ch, sl);
                }
                else if (r >= 4000 && r < 4000 + ports.size())
                {
                    if (e.addExternalSlot (t, p, ports[r - 4000])) e.assignPart (t, p, ch, slotsNow() - 1);
                }
                thisRow->refreshParts();
            });
        }

        TracksComponent& owner;
        int index;
        StemDef def;
        MidiDoc doc;
        Label name, empty, partsCap;
        ComboBox output;
        TextButton notes { "Edit notes..." };
        Slider gain;
        TextButton remove { "X" };
        std::vector<std::unique_ptr<Part>> parts;
    };

    struct SectionRow : public Component
    {
        SectionRow (TracksComponent& o, int i, const MarkerDef& m) : owner (o), index (i)
        {
            addAndMakeVisible (name);
            name.setText (m.name, dontSendNotification);
            name.setEditable (true);
            name.onTextChange = [this] { owner.updateMarker (index, [this] (MarkerDef& d) { d.name = name.getText().trim(); }); };

            addAndMakeVisible (time);
            time.setText (formatTime (m.seconds), dontSendNotification);
            time.setEditable (true);
            time.setJustificationType (Justification::centredRight);
            time.setTooltip ("Where the section starts: seconds, or minutes:seconds");
            time.onTextChange = [this]
            {
                const double secs = parseTime (time.getText());
                owner.updateMarker (index, [secs] (MarkerDef& d) { d.seconds = secs; });
                time.setText (formatTime (secs), dontSendNotification);
            };

            addAndMakeVisible (vamp);
            vamp.setToggleState (m.vamp, dontSendNotification);
            vamp.setTooltip ("Repeat this section until released");
            vamp.onClick = [this] { const bool v = vamp.getToggleState(); owner.updateMarker (index, [v] (MarkerDef& d) { d.vamp = v; }); };

            addAndMakeVisible (go);
            go.setTooltip ("Go here: at the end of the current section if the backing track is playing, otherwise where Play starts");
            go.onClick = [this] { owner.goToMarker (index); };

            addAndMakeVisible (actions);
            actions.setButtonText (m.actions.empty() ? "Actions..." : String ((int) m.actions.size()) + (m.actions.size() == 1 ? " action..." : " actions..."));
            actions.setTooltip ("What happens when the track enters this section: a keyboard's program, a part muted, an effect bypassed, a cue to the lights");
            actions.onClick = [this] { owner.editActions (index); };

            addAndMakeVisible (remove);
            remove.setTooltip ("Remove this section marker (the music is untouched)");
            remove.onClick = [this] { owner.removeMarker (index); };
        }
        static String formatTime (double s)
        {
            const int m = (int) (s / 60.0);
            return String (m) + ":" + String (s - m * 60.0, 1).paddedLeft ('0', 4);
        }
        static double parseTime (const String& t)
        {
            if (t.containsChar (':'))
                return jmax (0.0, t.upToFirstOccurrenceOf (":", false, false).getDoubleValue() * 60.0
                                    + t.fromFirstOccurrenceOf (":", false, false).getDoubleValue());
            return jmax (0.0, t.getDoubleValue());
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (0, 2);
            remove.setBounds (r.removeFromRight (28));   r.removeFromRight (6);
            go.setBounds (r.removeFromRight (44));       r.removeFromRight (6);
            actions.setBounds (r.removeFromRight (96));  r.removeFromRight (6);
            vamp.setBounds (r.removeFromRight (70));     r.removeFromRight (6);
            time.setBounds (r.removeFromRight (90));     r.removeFromRight (6);
            name.setBounds (r);
        }
        TracksComponent& owner;
        int index;
        Label name, time;
        ToggleButton vamp { "Vamp" };
        TextButton go { "Go" }, actions, remove { "X" };
    };

    void editActions (int markerIndex)
    {
        SafePointer<TracksComponent> self (this);
        showSectionActions (engine, songIndex, markerIndex, [self] { if (self != nullptr) self->rebuild(); });
    }

    std::vector<MarkerDef> markers() const
    {
        const auto& songs = engine.getSongs();
        return songIndex >= 0 && songIndex < (int) songs.size() ? songs[(size_t) songIndex].markers : std::vector<MarkerDef>();
    }

    void updateMarker (int index, std::function<void (MarkerDef&)> change)
    {
        auto m = markers();
        if (index < 0 || index >= (int) m.size()) return;
        change (m[(size_t) index]);
        engine.setSongMarkers (songIndex, m);
    }

    void removeMarker (int index)
    {
        auto m = markers();
        if (index < 0 || index >= (int) m.size()) return;
        m.erase (m.begin() + index);
        engine.setSongMarkers (songIndex, m);
        MessageManager::callAsync ([this] { rebuild(); });
    }

    void goToMarker (int index)
    {
        if (engine.getTrackSong() != songIndex) return;
        const auto m = markers();
        if (index < 0 || index >= (int) m.size()) return;
        // The player's sections may begin with an implicit one before the first marker: find it by time.
        for (int s = 0; s < engine.getTrackSectionCount(); ++s)
            if (std::abs (engine.getTrackSectionStart (s) - m[(size_t) index].seconds) < 0.001)
                { engine.queueTrackSection (s); return; }
    }

    void showChannelRoutes (int stemIndex)
    {
        SafePointer<TracksComponent> self (this);
        showMidiEditor (engine, songIndex, stemIndex, [self] { if (self != nullptr) self->rebuild(); });
    }

    void fillInputs (ComboBox& box, int selected)
    {
        box.clear (dontSendNotification);
        const auto& ins = engine.getSetup().inputs;
        for (int i = 0; i < (int) ins.size(); ++i)
            box.addItem ("-> " + ins[(size_t) i].name, i + 1);
        box.setSelectedId (jlimit (0, jmax (0, (int) ins.size() - 1), selected) + 1, dontSendNotification);
    }

    void fillOutputs (ComboBox& box, int selected)
    {
        box.clear (dontSendNotification);
        const auto& outs = engine.getSetup().outputs;
        for (int o = 0; o < (int) outs.size(); ++o)
            box.addItem (outs[(size_t) o].name + "  (" + String (outs[(size_t) o].left + 1)
                           + (outs[(size_t) o].right >= 0 ? "+" + String (outs[(size_t) o].right + 1) : String()) + ")", o + 1);
        box.setSelectedId (jlimit (0, jmax (0, (int) outs.size() - 1), selected) + 1, dontSendNotification);
    }

    void applyCountIn()
    {
        const int beats = jmax (0, countInBox.getSelectedId() - 1);
        const bool click = clickBox.getSelectedId() == 2;
        engine.setSongCountIn (songIndex, beats, jmax (0, countInOutBox.getSelectedId() - 1));
        engine.setSongClick (songIndex, click, jmax (2, barBox.getSelectedId()));
        barBox.setEnabled (click);
        countInOutBox.setEnabled (beats > 0 || click);
    }

    double songBeatSeconds() const
    {
        const auto& songs = engine.getSongs();
        const double bpm = songIndex < (int) songs.size() && songs[(size_t) songIndex].tempoBpm > 0.0 ? songs[(size_t) songIndex].tempoBpm
                                                                                                     : engine.getTempoBpm();
        return 60.0 / jmax (20.0, bpm);
    }

    void refreshTempoRow()
    {
        const auto& songs = engine.getSongs();
        if (songIndex < 0 || songIndex >= (int) songs.size()) return;
        const auto& sg = songs[(size_t) songIndex];
        const bool stretched = sg.trackBpm > 0.0 && sg.tempoBpm > 0.0 && std::abs (sg.trackBpm - sg.tempoBpm) > 0.005;
        tempoText.setText (sg.tempoBpm > 0.0 ? String (sg.tempoBpm, 1) + " bpm, the song's own"
                                                 + (stretched ? "  (files stretched " + String (roundToInt ((sg.tempoBpm / sg.trackBpm - 1.0) * 100.0)) + "%)" : String())
                                             : String (engine.getTempoBpm(), 1) + " bpm now (the song sets none)", dontSendNotification);
        tempoText.setColour (Label::textColourId, sg.tempoBpm > 0.0 ? Colours::white : textDim);
        tempoText.setTooltip (stretched ? "The files were made at " + String (sg.trackBpm, 1) + " bpm and are stretched to play at the song's "
                                          + String (sg.tempoBpm, 1) + ". Edit... on the song sets its tempo; Files at sets theirs."
                                        : "The tempo the click, the count-in and the bar lines use. Edit... on the song sets it by hand.");
        fileBpmText.setText (sg.trackBpm > 0.0 ? String (sg.trackBpm, 1) : String ("?"), dontSendNotification);
        fileBpmText.setColour (Label::textColourId, sg.trackBpm > 0.0 ? Colours::white : textDim);
        transposeTrackBtn.setToggleState (sg.transposeTrack, dontSendNotification);
        transposeTrackBtn.setButtonText (sg.transposeTrack && sg.transpose != 0
                                           ? "Transpose the files with the song  (" + String (sg.transpose > 0 ? "+" : "") + String (sg.transpose) + " st now)"
                                           : "Transpose the files with the song");
        const int m = (int) (sg.firstBeat / 60.0);
        firstBeatText.setText (String (m) + ":" + String (sg.firstBeat - m * 60.0, 2).paddedLeft ('0', 5), dontSendNotification);
        if (sg.startSeconds > 0.0)
        {
            const int sm = (int) (sg.startSeconds / 60.0);
            startText.setText (String (sm) + ":" + String (sg.startSeconds - sm * 60.0, 2).paddedLeft ('0', 5), dontSendNotification);
            startText.setColour (Label::textColourId, Colours::white);
        }
        else
        {
            startText.setText ("first sound", dontSendNotification);
            startText.setColour (Label::textColourId, textDim);
        }
        if (timeline != nullptr) timeline->repaint();
    }

    void scaleTempo (double factor)
    {
        const auto& songs = engine.getSongs();
        if (songIndex < 0 || songIndex >= (int) songs.size()) return;
        const double bpm = songs[(size_t) songIndex].tempoBpm > 0.0 ? songs[(size_t) songIndex].tempoBpm : engine.getTempoBpm();
        // Both numbers: the detector heard the octave wrong, not the stretch.
        const auto& sg = songs[(size_t) songIndex];
        engine.setSongTempoAndFiles (songIndex, std::round (bpm * factor * 100.0) / 100.0,
                                     sg.trackBpm > 0.0 ? std::round (sg.trackBpm * factor * 100.0) / 100.0 : 0.0, sg.firstBeat);
        refreshTempoRow();
    }

    void nudgeFirstBeat (int beats)
    {
        const auto& songs = engine.getSongs();
        if (songIndex < 0 || songIndex >= (int) songs.size()) return;
        const auto& sg = songs[(size_t) songIndex];
        const double beat = songBeatSeconds();
        double fb = sg.firstBeat + beats * beat;
        if (fb < 0.0) fb += jmax (1, sg.beatsPerBar) * beat;      // the same downbeat, a bar later
        engine.setSongFirstBeat (songIndex, jmax (0.0, fb));
        refreshTempoRow();
    }

    void findTempo()
    {
        const auto& songs = engine.getSongs();
        if (songIndex < 0 || songIndex >= (int) songs.size()) return;
        const auto& sg = songs[(size_t) songIndex];
        File best;
        for (const auto& st : sg.stems)
            if (! st.isMidi())
                if (const File f = engine.getChartsFolder().getChildFile (st.file); f.getSize() > best.getSize())
                    best = f;
        if (best == File())
        {
            tempoText.setText ("Add an audio file first; MIDI files bring their own tempo.", dontSendNotification);
            return;
        }
        if (detectThread.joinable()) return;
        findTempoBtn.setEnabled (false);
        tempoText.setText ("Listening to " + best.getFileName() + "...", dontSendNotification);
        detectStop = std::make_shared<std::atomic<bool>> (false);
        SafePointer<TracksComponent> self (this);
        detectThread = std::thread ([self, best, bpb = sg.beatsPerBar, stop = detectStop]
        {
            AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<AudioFormatReader> reader (fm.createReaderFor (best));
            TempoEstimate est;
            if (reader == nullptr) est.problem = best.getFileName() + " could not be read.";
            else est = detectTempo (*reader, bpb, [stop] { return stop->load(); });
            MessageManager::callAsync ([self, est, name = best.getFileName()] { if (self != nullptr) self->tempoFound (est, name); });
        });
    }

    void tempoFound (const TempoEstimate& est, const String& fileName)
    {
        if (detectThread.joinable()) detectThread.join();
        findTempoBtn.setEnabled (true);
        if (! est.ok) { tempoText.setText (est.problem, dontSendNotification); return; }
        const double bpm = std::round (est.bpm * 100.0) / 100.0;     // two decimals: a tenth of a bpm drifts a beat over a long song
        const int m = (int) (est.firstBeat / 60.0);
        const String at = String (m) + ":" + String (est.firstBeat - m * 60.0, 2).paddedLeft ('0', 5);
        const String how = est.confidence > 0.6 ? "a clear, steady beat" : est.confidence > 0.3 ? "a fair guess: check it against the click" : "a faint beat: check it against the click";
        refreshTempoRow();
        SafePointer<TracksComponent> self (this);
        perf::confirm ("Tempo found",
                       fileName + " is at " + String (bpm, 1) + " bpm (" + how + "), with the first beat of a bar at " + at + ".\n\n"
                       "Use it for this song? Its tempo, click, count-in and the bar lines on the timeline will follow it. "
                       "If the band counts it twice or half as fast, press x2 or /2 afterwards; if the accent lands on the "
                       "wrong beat, move the first beat a beat with the buttons beside it.",
                       "Use it", [self, bpm, fb = est.firstBeat]
        {
            if (self == nullptr) return;
            self->engine.setSongTempoAndFiles (self->songIndex, bpm, bpm, fb);
            self->refreshTempoRow();
        });
    }

    void timerCallback() override
    {
        // Rows follow the song if it was changed elsewhere -- a stem dropped on it in the songs panel.
        const auto& songs = engine.getSongs();
        if (songIndex >= 0 && songIndex < (int) songs.size()
            && (songs[(size_t) songIndex].stems.size() != rows.size() || songs[(size_t) songIndex].markers.size() != sectionRows.size()))
            rebuild();
        else
            for (auto& row : rows) if (row->def.isMidi()) row->refreshParts();     // a plugin finished loading, a part was changed elsewhere
        playBtn.setButtonText (timeline != nullptr && timeline->isPlaying() ? "Stop" : "Play");
    }

    Engine& engine;
    SongsPanel& panel;
    const int songIndex;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<std::unique_ptr<SectionRow>> sectionRows;
    Label sectionsHeader { {}, "SECTIONS" };
    std::unique_ptr<TrackTimeline> timeline;
    Label timelineHint;
    static constexpr const char* kTimelineHint = "Click the timeline to set where Play starts. Double-click to start a section there; drag a flag to move it; right-click it for more.";
    TextButton playBtn { "Play" }, addSectionBtn { "Mark section here" }, zoomOutBtn { "-" }, zoomInBtn { "+" }, fitBtn { "Fit" };
    TextButton addBtn { "+ Add audio or MIDI..." };
    Label countInLabel { {}, "Count-in" }, clickLabel { {}, "Click" }, countInOutLabel { {}, "Heard on" }, hint;
    ComboBox countInBox, clickBox, barBox, countInOutBox;
    Label tempoLabel { {}, "Tempo" }, tempoText, fileBpmLabel { {}, "Files at" }, fileBpmText, firstBeatLabel { {}, "First beat" }, firstBeatText,
          startLabel { {}, "Starts at" }, startText;
    ToggleButton transposeTrackBtn { "Transpose the files with the song" };
    TextButton findTempoBtn { "Find the tempo in the audio..." }, halfBtn { "/2" }, doubleBtn { "x2" }, beatBackBtn { "< beat" }, beatFwdBtn { "beat >" };
    std::thread detectThread;
    std::shared_ptr<std::atomic<bool>> detectStop;
    std::unique_ptr<FileChooser> chooser;
};

namespace
{
    /** The Backing track window: a document window with the usual title-bar
        buttons, so it can be maximised to work on a long song. Escape closes it. */
    class TracksWindow : public DocumentWindow
    {
    public:
        TracksWindow (const String& title, TracksComponent* content)
            : DocumentWindow (title, bgPanel, DocumentWindow::allButtons)
        {
            setContentOwned (content, true);
            setUsingNativeTitleBar (true);
            setResizable (true, true);
            setResizeLimits (700, 500, 10000, 10000);
            centreWithSize (jmin (getWidth(), getParentMonitorArea().getWidth() - 80), jmin (getHeight(), getParentMonitorArea().getHeight() - 80));
            setVisible (true);
            toFront (true);
        }
        void closeButtonPressed() override
        {
            setVisible (false);
            MessageManager::callAsync ([w = this] { delete w; });
        }
        bool keyPressed (const KeyPress& k) override
        {
            if (k == KeyPress::escapeKey) { closeButtonPressed(); return true; }
            return DocumentWindow::keyPressed (k);
        }
    };
}

void SongsPanel::showTracksDialog (int songIndex)
{
    const auto& songs = engine.getSongs();
    if (songIndex < 0 || songIndex >= (int) songs.size()) return;
    auto* content = new TracksComponent (engine, *this, songIndex);
    content->setSize (900, 760);
    new TracksWindow ("Backing track: " + songs[(size_t) songIndex].name, content);     // deletes itself when closed
}

} // namespace perf
