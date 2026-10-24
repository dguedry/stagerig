#include "SongsPanel.h"
#include "Charts.h"
#include "GmSynth.h"
#include "GmMixer.h"
#include "MidiEditor.h"
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

    songsList.setModel (&songsModel);
    setList.setModel (&setModel);
    for (auto* b : { &songsList, &setList })
    {
        addAndMakeVisible (b);
        b->setRowHeight (34);          // two lines: the name, and what it loads
        b->setColour (ListBox::backgroundColourId, bgRow);
    }

    addAndMakeVisible (addBtn);
    addBtn.setTooltip ("A new song, ready to capture what is loaded now");
    addBtn.onClick = [this] { addSong(); };

    addAndMakeVisible (removeBtn);
    removeBtn.setTooltip ("Delete the selected song, and take it out of every set that used it");
    removeBtn.onClick = [this] { removeSong(); };

    addAndMakeVisible (captureBtn);
    captureBtn.setColour (TextButton::buttonColourId, accentDim);
    captureBtn.setTooltip ("Save what every keyboard is playing right now, and the tempo, into the selected "
                           "song -- this is how you change which sounds a song loads: select it, change the "
                           "programs, then press this");
    captureBtn.onClick = [this] { captureIntoSelectedSong(); };

    addAndMakeVisible (editBtn);
    editBtn.setTooltip ("Name, stage notes and tempo for this song");
    editBtn.onClick = [this] { editSelectedSong(); };

    addAndMakeVisible (chartBtn);
    chartBtn.setTooltip ("What this song brings with it: sheet music for the tablet (a PDF, an image, a text "
                         "chart or a ChordPro file) and a backing track: audio files and MIDI files. Or drop files on the song.");
    chartBtn.onClick = [this] { showChartMenu(); };

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
    toSetBtn.setTooltip ("Put the selected song at the end of the set");
    toSetBtn.onClick = [this] { addToSet(); };

    addAndMakeVisible (fromSetBtn);
    fromSetBtn.setTooltip ("Take this song out of the set. The song itself is kept.");
    fromSetBtn.onClick = [this] { removeFromSet(); };

    addAndMakeVisible (songUpBtn);
    songUpBtn.setTooltip ("Move this song up the list. Sets keep playing the same order.");
    songUpBtn.onClick = [this] { moveSongInList (-1); };
    addAndMakeVisible (songDownBtn);
    songDownBtn.setTooltip ("Move this song down the list");
    songDownBtn.onClick = [this] { moveSongInList (1); };

    addAndMakeVisible (upBtn);
    upBtn.onClick = [this] { moveInSet (-1); };
    addAndMakeVisible (downBtn);
    downBtn.onClick = [this] { moveInSet (1); };

    addAndMakeVisible (setBtn);
    setBtn.setTooltip ("Choose which set you are playing, or make a new one");
    setBtn.onClick = [this] { showSetMenu(); };

    addAndMakeVisible (footBtn);
    footBtn.setTooltip ("Pedals that step to the next and previous song");
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
    setBtn.setBounds (top.removeFromLeft (top.getWidth() / 2).reduced (1, 0));
    footBtn.setBounds (top.reduced (1, 0));
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
    const int third = songButtons.getWidth() / 3;
    captureBtn.setBounds (songButtons.removeFromLeft (third).reduced (1, 0));
    editBtn.setBounds (songButtons.removeFromLeft (third).reduced (1, 0));
    chartBtn.setBounds (songButtons.reduced (1, 0));

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

    songsHeader.setText (songs.empty() ? "SONGS  (a program per keyboard, and a tempo)"
                                      : "SONGS  (" + String ((int) songs.size()) + ")",
                         dontSendNotification);

    if (set >= 0 && set < (int) sets.size())
    {
        const auto& st = sets[(size_t) set];
        setHeader.setText ("SET: " + st.name.toUpperCase() + "  ("
                             + String ((int) st.songs.size()) + ")", dontSendNotification);
    }
    else
    {
        setHeader.setText (sets.empty() ? "SET  (none yet)" : "SET  (none chosen)", dontSendNotification);
    }

    const bool haveSong = selectedSong >= 0 && selectedSong < (int) songs.size();
    const bool haveSet  = set >= 0 && set < (int) sets.size();
    removeBtn.setEnabled (haveSong);
    captureBtn.setEnabled (haveSong);
    editBtn.setEnabled (haveSong);
    chartBtn.setEnabled (haveSong);
    toSetBtn.setEnabled (haveSong && haveSet);
    fromSetBtn.setEnabled (haveSet && selectedInSet >= 0);
    songUpBtn.setEnabled (haveSong && selectedSong > 0);
    songDownBtn.setEnabled (haveSong && selectedSong < (int) songs.size() - 1);
    upBtn.setEnabled (haveSet && selectedInSet > 0);
    downBtn.setEnabled (haveSet && selectedInSet >= 0
                        && selectedInSet < (int) sets[(size_t) set].songs.size() - 1);

    songsList.updateContent();
    setList.updateContent();
    songsList.repaint();
    setList.repaint();
}

void SongsPanel::timerCallback()
{
    /* Follow the set moving under us. Only the highlight and the headers can
       change without a click here, so this is cheap. */
    static int lastSet = -2, lastPos = -2, lastSongs = -1, lastSets = -1;
    const int set = engine.getCurrentSet(), pos = engine.getCurrentSongInSet();
    const int nSongs = (int) engine.getSongs().size(), nSets = (int) engine.getSets().size();
    if (set != lastSet || pos != lastPos || nSongs != lastSongs || nSets != lastSets)
    {
        lastSet = set; lastPos = pos; lastSongs = nSongs; lastSets = nSets;
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
    nextBtn.setButtonText (vamping ? "Go on" : "Next");
    nextBtn.setColour (TextButton::buttonColourId, vamping ? Colour (0xff8a5a12) : getLookAndFeel().findColour (TextButton::buttonColourId));
    playBtn.setColour (TextButton::buttonColourId, engine.isTrackRunning() ? accentDim : getLookAndFeel().findColour (TextButton::buttonColourId));

    String text;
    if (! has)
        text = "No backing track";
    else if (! engine.isTrackReady())
        text = songs[(size_t) song].name + "  -- loading " + String (roundToInt (engine.getTrackLoadProgress() * 100.0f)) + "%";
    else if (engine.isTrackCountingIn())
        text = songs[(size_t) song].name + "  -- counting in";
    else
    {
        text = mmss (engine.getTrackPosition()) + " / " + mmss (engine.getTrackLength());
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
    if (! isSet) return (int) owner.engine.getSongs().size();
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
        songIndex = row;
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
        if (prog < 0) continue;              // this song leaves that keyboard alone
        const auto& pd = inputs[(size_t) i].programs[(size_t) prog];
        parts.add (String (prog).paddedLeft ('0', 3)
                     + (pd.name.isNotEmpty() ? " " + pd.name : ""));
    }
    if (! sg.charts.empty())
        parts.add (String (CharPointer_UTF8 ("\xe2\x99\xaa")) + (sg.charts.size() > 1 ? " charts" : " chart"));
    g.setColour (playing ? Colours::white.withAlpha (0.8f) : textDim);
    g.setFont (FontOptions (10.5f));
    g.drawText (parts.isEmpty() ? "(nothing captured)" : parts.joinIntoString ("   "),
                r, Justification::centredLeft, true);
}

void SongsPanel::ListProxy::listBoxItemClicked (int row, const MouseEvent&)
{
    /* One click loads it. That is what a tap does on the tablet and what a
       click does in the programs list, and a set list whose songs need
       double-clicking would be the odd one out -- on stage, twice is a miss.

       It also selects, so Capture, Rename and Add to set act on what you just
       clicked without a second step. */
    if (isSet) owner.selectedInSet = row;
    else       owner.selectedSong  = row;
    play (row);
}

void SongsPanel::ListProxy::play (int row)
{
    /* In the set that means "start here", which also moves the position a
       footswitch will step from; in the song list it is just "load this",
       without touching the set. */
    if (isSet) owner.engine.selectSongInSet (row);
    else       owner.engine.selectSong (row);
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
        refresh();
        songsList.selectRow (selectedSong);
    }), false);
}

void SongsPanel::removeSong()
{
    const auto& songs = engine.getSongs();
    if (selectedSong < 0 || selectedSong >= (int) songs.size()) return;

    const auto name = songs[(size_t) selectedSong].name;
    auto* w = new AlertWindow ("Delete song", "Delete \"" + name + "\"?\n\n"
                              "It will also be taken out of every set that uses it.",
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
                                   "want and press Capture.\n\nThe name, and anything you want to "
                                   "read on the stand:",
                               MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", sg.name);
    w->addTextEditor ("notes", sg.notes);
    w->addTextEditor ("tempo", sg.tempoBpm > 0.0 ? String (sg.tempoBpm, 1) : String());
    w->getTextEditor ("notes")->setTextToShowWhenEmpty ("stage notes: key, count-in, a reminder", textDim);
    w->getTextEditor ("tempo")->setTextToShowWhenEmpty ("tempo, or blank to keep the current one", textDim);
    w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
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
    songsList.selectRow (selectedSong);
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
    auto* w = new AlertWindow ("New set", "What is this set called?", MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", "Set " + String ((int) engine.getSets().size() + 1));
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
        m.addSectionHeader ("Play");
        for (int i = 0; i < (int) sets.size(); ++i)
            m.addItem (100 + i, sets[(size_t) i].name, true, i == current);
        m.addSeparator();
    }
    m.addItem (1, "New set...");
    if (current >= 0)
    {
        m.addItem (2, "Rename this set...");
        m.addItem (4, "Move up",   current > 0);
        m.addItem (5, "Move down", current < (int) sets.size() - 1);
        m.addItem (3, "Delete this set");
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
            auto* w = new AlertWindow ("Set", "Name", MessageBoxIconType::NoIcon);
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
            engine.removeSet (set);
            selectedInSet = -1;
            refresh();
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
    m.addItem (1, "Attach a chart...");
    m.addItem (2, "Backing track..." + (sg.stems.empty() ? String() : "  (" + String ((int) sg.stems.size()) + (sg.stems.size() == 1 ? " file)" : " files)")));
    if (! sg.charts.empty())
    {
        m.addSectionHeader ("Attached");
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
        else if (r == 2)
        {
            showTracksDialog (idx);
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
        song = songsList.getRowContainingPosition (x - songsList.getX(), y - songsList.getY());
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

        addAndMakeVisible (countInLabel);
        countInLabel.setColour (Label::textColourId, textDim);
        addAndMakeVisible (countInBox);
        countInBox.addItem ("No count-in", 1);
        for (int b = 1; b <= 8; ++b) countInBox.addItem (String (b) + (b == 1 ? " beat" : " beats"), b + 1);
        countInBox.onChange = [this] { applyCountIn(); };
        addAndMakeVisible (countInOutLabel);
        countInOutLabel.setColour (Label::textColourId, textDim);
        addAndMakeVisible (countInOutBox);
        countInOutBox.onChange = [this] { applyCountIn(); };

        addAndMakeVisible (sectionsHeader);
        sectionsHeader.setColour (Label::textColourId, accent);
        sectionsHeader.setFont (FontOptions (13.0f, Font::bold));
        addAndMakeVisible (addSectionBtn);
        addSectionBtn.setTooltip ("A new section where the track is now if it is playing, otherwise after the last one. "
                                  "Type the time to move it: 1:23.5");
        addSectionBtn.onClick = [this] { addSection(); };

        addAndMakeVisible (hint);
        hint.setColour (Label::textColourId, textDim);
        hint.setFont (FontOptions (12.0f));
        hint.setJustificationType (Justification::topLeft);
        hint.setText ("Audio files play on an output. A MIDI file plays parts, like another player in the band: "
                      "each part plays on what you choose -- a General MIDI instrument, a keyboard's sounds, a plugin, "
                      "a MIDI device. Everything starts together with Play in the songs panel, the tablet, or the "
                      "start/stop footswitch. Put a click or cue file on your in-ear output. The count-in clicks at the song's tempo.\n"
                      "Sections start at their time and run to the next. A vamp section repeats until released "
                      "(Next, the tablet, or the section footswitch); asking for a section takes you there when "
                      "the current one ends.", dontSendNotification);

        rebuild();
        startTimerHz (2);
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

        sectionRows.clear();
        for (int i = 0; i < (int) sg.markers.size(); ++i)
        {
            auto row = std::make_unique<SectionRow> (*this, i, sg.markers[(size_t) i]);
            addAndMakeVisible (row.get());
            sectionRows.push_back (std::move (row));
        }

        fillOutputs (countInOutBox, sg.countInOutput);
        countInBox.setSelectedId (sg.countInBeats + 1, dontSendNotification);
        countInOutBox.setEnabled (sg.countInBeats > 0);
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
        for (auto& row : sectionRows) row->setBounds (r.removeFromTop (28));
        r.removeFromTop (4);
        addSectionBtn.setBounds (r.removeFromTop (26).removeFromLeft (180));
        r.removeFromTop (12);
        auto c = r.removeFromTop (24);
        countInLabel.setBounds (c.removeFromLeft (70));
        countInBox.setBounds (c.removeFromLeft (130));
        c.removeFromLeft (12);
        countInOutLabel.setBounds (c.removeFromLeft (30));
        countInOutBox.setBounds (c.removeFromLeft (180));
        r.removeFromTop (10);
        hint.setBounds (r);
    }

private:
    struct Row : public Component
    {
        Row (TracksComponent& o, int i, const StemDef& d) : owner (o), index (i), def (d)
        {
            addAndMakeVisible (name);
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
                refreshParts();
            }
            else
            {
                addAndMakeVisible (output);
                owner.fillOutputs (output, d.output);
                output.onChange = [this] { def.output = output.getSelectedId() - 1; owner.engine.setSongStem (owner.songIndex, index, def); };

                addAndMakeVisible (gain);
                gain.setRange (-60.0, 12.0, 0.5);
                gain.setSliderStyle (Slider::LinearBar);
                gain.setTextValueSuffix (" dB");
                gain.setDoubleClickReturnValue (true, 0.0);
                gain.setValue (d.gainDb, dontSendNotification);
                gain.onValueChange = [this] { def.gainDb = (float) gain.getValue(); owner.engine.setSongStem (owner.songIndex, index, def); };
            }
            addAndMakeVisible (remove);
            remove.setTooltip ("Take this file off the song. It stays in the setup's folder.");
            remove.onClick = [this] { owner.engine.removeSongStem (owner.songIndex, index); MessageManager::callAsync ([o = &owner] { o->rebuild(); }); };
        }

        int preferredHeight() const { return 30 + (int) parts.size() * 28 + (parts.empty() && def.isMidi() ? 24 : 0); }

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
            if (parts.empty()) empty.setBounds (all.removeFromTop (24).withTrimmedLeft (24));
            for (auto& p : parts)
            {
                auto line = all.removeFromTop (28).reduced (0, 3);
                line.removeFromLeft (24);
                line.removeFromRight (34);                 // under the X
                p->mute.setBounds (line.removeFromRight (28));  line.removeFromRight (6);
                p->level.setBounds (line.removeFromRight (100)); line.removeFromRight (6);
                p->plays.setBounds (line.removeFromRight (jmin (300, line.getWidth() / 2))); line.removeFromRight (6);
                p->label.setBounds (line);
            }
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
            TextButton plays, mute { "M" };
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
            for (auto* c : std::initializer_list<Component*> { &p->label, &p->plays, &p->level, &p->mute })
                addAndMakeVisible (c);
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
                if (sd == nullptr) { p->mute.setToggleState (false, dontSendNotification); continue; }
                if (sd->isGm())
                {
                    const auto& gc = sd->gmChannels[(size_t) p->channel - 1];
                    p->level.setDoubleClickReturnValue (true, 0.0);
                    p->level.setValue (gc.volume >= 0 ? ccToDb (gc.volume, fileCc (p->channel)) : 0.0f, dontSendNotification);
                    p->mute.setToggleState (gc.muted, dontSendNotification);
                }
                else
                {
                    p->level.setDoubleClickReturnValue (true, 0.0);
                    p->level.setValue (sd->gainDb, dontSendNotification);
                    p->mute.setToggleState (! sd->enabled, dontSendNotification);
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

        void setMuted (int ch, bool muted)
        {
            int sl = -1;
            const auto* sd = instrumentFor (ch, &sl);
            if (sd == nullptr) return;
            if (sd->isGm())
            {
                auto gc = sd->gmChannels[(size_t) ch - 1];
                gc.muted = muted;
                owner.engine.setGmChannel (track(), program(), sl, ch, gc);
            }
            else
                owner.engine.setSlotEnabled (track(), program(), sl, ! muted);
        }

        /** What plays this part: a General MIDI instrument, a keyboard's sounds,
            a plugin, a MIDI device, something already in the song's program, or nothing. */
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
            const auto& inputs = engine.getSetup().inputs;
            for (int i = 0; i < (int) inputs.size(); ++i)
                if (! inputs[(size_t) i].isAudio() && ! inputs[(size_t) i].isTrack())
                    m.addItem (2000 + i, inputs[(size_t) i].name + "'s sounds  (whatever it is playing)", true,
                               cur != nullptr && cur->isInput() && cur->targetInput == i);
            Array<PluginDescription> types;
            for (auto& d : engine.getPluginHost().getKnownPlugins().getTypes()) if (d.isInstrument) types.add (d);
            {
                PopupMenu plugins;
                StringArray makers;
                for (auto& d : types) makers.addIfNotAlreadyThere (d.manufacturerName.isNotEmpty() ? d.manufacturerName : "Other");
                makers.sortNatural();
                for (auto& maker : makers)
                {
                    PopupMenu sub;
                    for (int k = 0; k < types.size(); ++k)
                        if ((types[k].manufacturerName.isNotEmpty() ? types[k].manufacturerName : "Other") == maker)
                            sub.addItem (3000 + k, types[k].name + "  [" + types[k].pluginFormatName + "]");
                    if (makers.size() == 1) plugins = sub; else plugins.addSubMenu (maker, sub);
                }
                if (types.isEmpty()) plugins.addItem (2999, "No instrument plugins known yet: scan under Plugins...", false);
                m.addSubMenu ("A plugin instrument", plugins);
            }
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

            m.showMenuAsync (PopupMenu::Options().withTargetComponent (partFor (ch)->plays), [this, ch, t, p, types, ports] (int r)
            {
                auto& e = owner.engine;
                if (r < 1) return;
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
                else if (r >= 2000 && r < 3000)
                {
                    const int target = r - 2000;
                    const auto& slots = e.getSetup().inputs[(size_t) t].programs[(size_t) p].slots;
                    int sl = -1;
                    for (int k = 0; k < (int) slots.size(); ++k) if (slots[(size_t) k].isInput() && slots[(size_t) k].targetInput == target) sl = k;
                    if (sl < 0 && e.addInputSlot (t, p, target)) sl = slotsNow() - 1;
                    if (sl >= 0) e.assignPart (t, p, ch, sl);
                }
                else if (r >= 3000 && r < 3000 + types.size())
                {
                    String err;
                    if (e.addSlot (t, p, types[r - 3000], err)) e.assignPart (t, p, ch, slotsNow() - 1);
                }
                else if (r >= 4000 && r < 4000 + ports.size())
                {
                    if (e.addExternalSlot (t, p, ports[r - 4000])) e.assignPart (t, p, ch, slotsNow() - 1);
                }
                refreshParts();
            });
        }

        TracksComponent& owner;
        int index;
        StemDef def;
        MidiDoc doc;
        Label name, empty;
        ComboBox output;
        TextButton notes { "Notes..." };
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
            go.setTooltip ("Go here: at the end of the current section if the track is playing, otherwise where Play starts");
            go.onClick = [this] { owner.goToMarker (index); };

            addAndMakeVisible (remove);
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
            vamp.setBounds (r.removeFromRight (70));     r.removeFromRight (6);
            time.setBounds (r.removeFromRight (90));     r.removeFromRight (6);
            name.setBounds (r);
        }
        TracksComponent& owner;
        int index;
        Label name, time;
        ToggleButton vamp { "Vamp" };
        TextButton go { "Go" }, remove { "X" };
    };

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

    void addSection()
    {
        auto m = markers();
        double at = 0.0;
        if (engine.getTrackSong() == songIndex && engine.isTrackRunning())
            at = engine.getTrackPosition();
        else if (! m.empty())
        {
            for (const auto& x : m) at = jmax (at, x.seconds);
            at += 10.0;
        }
        m.push_back ({ "Section " + String ((int) m.size() + 1), at, false });
        std::sort (m.begin(), m.end(), [] (const MarkerDef& a, const MarkerDef& b) { return a.seconds < b.seconds; });
        engine.setSongMarkers (songIndex, m);
        rebuild();
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
        engine.setSongCountIn (songIndex, beats, jmax (0, countInOutBox.getSelectedId() - 1));
        countInOutBox.setEnabled (beats > 0);
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
    }

    Engine& engine;
    SongsPanel& panel;
    const int songIndex;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<std::unique_ptr<SectionRow>> sectionRows;
    Label sectionsHeader { {}, "SECTIONS" };
    TextButton addSectionBtn { "+ Add section" };
    TextButton addBtn { "+ Add audio or MIDI..." };
    Label countInLabel { {}, "Count-in" }, countInOutLabel { {}, "on" }, hint;
    ComboBox countInBox, countInOutBox;
    std::unique_ptr<FileChooser> chooser;
};

void SongsPanel::showTracksDialog (int songIndex)
{
    const auto& songs = engine.getSongs();
    if (songIndex < 0 || songIndex >= (int) songs.size()) return;
    auto* content = new TracksComponent (engine, *this, songIndex);
    content->setSize (820, 640);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "Backing track: " + songs[(size_t) songIndex].name;
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

} // namespace perf
