#include "MidiEditor.h"
#include "GmSynth.h"
#include "GmMixer.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace juce;

namespace perf
{

namespace
{
    const Colour bgPanel   { 0xff1b1d22 };
    const Colour bgRow     { 0xff23262d };
    const Colour accent    { 0xff4f9dff };
    const Colour textDim   { 0xff9aa0ab };
    const Colour selColour { 0xffffb347 };

    bool isNoteOn (const MidiMessage& m)  { return m.isNoteOn() && m.getVelocity() > 0; }
}

//==============================================================================
// MidiDoc
//==============================================================================
bool MidiDoc::load (const File& f, String& error)
{
    MidiFile mf;
    FileInputStream in (f);
    if (! f.existsAsFile() || ! in.openedOk() || ! mf.readFrom (in))
    {
        error = f.existsAsFile() ? "not a MIDI file StageRig can read" : "file missing";
        return false;
    }
    const int tf = mf.getTimeFormat();
    if (tf <= 0)
    {
        error = "this file counts time in SMPTE frames, which the editor cannot edit";
        return false;
    }
    ticksPerQuarter = tf;
    notes.clear();
    others.clear();

    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        MidiMessageSequence seq (*mf.getTrack (t));
        seq.updateMatchedPairs();
        MidiMessageSequence rest;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            auto* e = seq.getEventPointer (i);
            const auto& m = e->message;
            if (isNoteOn (m))
            {
                Note n;
                n.track = t;
                n.channel = m.getChannel();
                n.key = m.getNoteNumber();
                n.velocity = m.getVelocity();
                n.start = (int64) m.getTimeStamp();
                n.end = e->noteOffObject != nullptr ? (int64) e->noteOffObject->message.getTimeStamp() : n.start + ticksPerQuarter / 4;
                if (n.end <= n.start) n.end = n.start + 1;
                notes.push_back (n);
            }
            else if (! m.isNoteOff() && ! m.isNoteOn())
                rest.addEvent (m);
            if (m.isTimeSignatureMetaEvent() && m.getTimeStamp() <= 0.0)
                m.getTimeSignatureInfo (timeSigNumerator, timeSigDenominator);
        }
        others.push_back (std::move (rest));
    }
    if (others.empty()) others.emplace_back();
    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    buildTempoMap();
    return true;
}

bool MidiDoc::save (const File& f, String& error) const
{
    MidiFile out;
    out.setTicksPerQuarterNote (ticksPerQuarter);
    for (int t = 0; t < (int) others.size(); ++t)
    {
        MidiMessageSequence seq (others[(size_t) t]);
        // Note-offs ahead of note-ons at the same tick, so a repeated key is let go before it sounds again.
        for (auto& n : notes)
            if (n.track == t)
            {
                auto off = MidiMessage::noteOff (n.channel, n.key);
                off.setTimeStamp ((double) n.end);
                seq.addEvent (off);
            }
        for (auto& n : notes)
            if (n.track == t)
            {
                auto on = MidiMessage::noteOn (n.channel, n.key, (uint8) jlimit (1, 127, n.velocity));
                on.setTimeStamp ((double) n.start);
                seq.addEvent (on);
            }
        seq.sort();
        seq.updateMatchedPairs();
        out.addTrack (seq);
    }

    // The first version is kept, so nothing the file came with is ever lost to an edit.
    const auto backup = f.getSiblingFile (f.getFileNameWithoutExtension() + ".orig" + f.getFileExtension());
    if (f.existsAsFile() && ! backup.existsAsFile()) f.copyFileTo (backup);

    TemporaryFile tmp (f);
    {
        FileOutputStream os (tmp.getFile());
        if (! os.openedOk() || ! out.writeTo (os, 1)) { error = "could not write " + f.getFileName(); return false; }
    }
    if (! tmp.overwriteTargetFileWithTemporary()) { error = "could not replace " + f.getFileName(); return false; }
    return true;
}

void MidiDoc::buildTempoMap()
{
    std::vector<std::pair<int64, double>> changes;     // tick, microseconds per quarter
    for (auto& seq : others)
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            if (m.isTempoMetaEvent())
                changes.push_back ({ (int64) m.getTimeStamp(), m.getTempoSecondsPerQuarterNote() * 1.0e6 });
        }
    std::stable_sort (changes.begin(), changes.end(), [] (auto& a, auto& b) { return a.first < b.first; });
    tempos.clear();
    tempos.push_back ({ 0, 0.0, 0.5 / ticksPerQuarter });
    for (auto& [tick, usPerQ] : changes)
    {
        auto& last = tempos.back();
        const double sec = last.seconds + (double) (tick - last.tick) * last.secondsPerTick;
        const double spt = usPerQ / 1.0e6 / ticksPerQuarter;
        if (tick == last.tick) { last.secondsPerTick = spt; continue; }
        tempos.push_back ({ tick, sec, spt });
    }
}

double MidiDoc::tickToSeconds (int64 tick) const
{
    const Tempo* t = &tempos.front();
    for (auto& x : tempos) if (x.tick <= tick) t = &x;
    return t->seconds + (double) (tick - t->tick) * t->secondsPerTick;
}

int64 MidiDoc::secondsToTick (double s) const
{
    const Tempo* t = &tempos.front();
    for (auto& x : tempos) if (x.seconds <= s) t = &x;
    return t->tick + (int64) ((s - t->seconds) / t->secondsPerTick);
}

int64 MidiDoc::lengthTicks() const
{
    int64 len = 0;
    for (auto& n : notes) len = jmax (len, n.end);
    for (auto& seq : others) len = jmax (len, (int64) seq.getEndTime());
    return len;
}

int MidiDoc::firstProgram (int channel) const
{
    double best = 1.0e18;
    int p = 0;
    for (auto& seq : others)
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            if (m.isProgramChange() && m.getChannel() == channel && m.getTimeStamp() < best) { best = m.getTimeStamp(); p = m.getProgramChangeNumber(); }
        }
    return p;
}

int MidiDoc::firstController (int channel, int cc) const
{
    double best = 1.0e18;
    int v = -1;
    for (auto& seq : others)
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            if (m.isController() && m.getControllerNumber() == cc && m.getChannel() == channel && m.getTimeStamp() < best) { best = m.getTimeStamp(); v = m.getControllerValue(); }
        }
    return v;
}

String MidiDoc::partName (int channel) const
{
    const int t = trackFor (channel);
    if (t < 0 || t >= (int) others.size()) return {};
    for (int i = 0; i < others[(size_t) t].getNumEvents(); ++i)
        if (const auto& m = others[(size_t) t].getEventPointer (i)->message; m.isTrackNameEvent())
            return m.getTextFromTextMetaEvent().trim();
    return {};
}

bool MidiDoc::channelUsed (int channel) const
{
    for (auto& n : notes) if (n.channel == channel) return true;
    return false;
}

int MidiDoc::trackFor (int channel) const
{
    std::map<int, int> count;
    for (auto& n : notes) if (n.channel == channel) ++count[n.track];
    int best = -1, most = 0;
    for (auto& [t, c] : count) if (c > most) { best = t; most = c; }
    if (best >= 0) return best;
    // A track whose controllers or program changes are on that channel, else the last one.
    for (int t = 0; t < (int) others.size(); ++t)
        for (int i = 0; i < others[(size_t) t].getNumEvents(); ++i)
            if (others[(size_t) t].getEventPointer (i)->message.getChannel() == channel) return t;
    return jmax (0, (int) others.size() - 1);
}

//==============================================================================
// The editor
//==============================================================================
class MidiEditorComponent;

/** One part: its channel, name, sound and mix, and its notes across the whole song. */
class LaneRow : public Component
{
public:
    static constexpr int height = 36, headerWidth = 500;

    LaneRow (MidiEditorComponent& e, int ch);
    void refresh();
    void paint (Graphics&) override;
    void resized() override;
    void mouseDown (const MouseEvent&) override;

    const int channel;

private:
    MidiEditorComponent& editor;
    Label chLabel, name;
    Label plays;                      // what plays it, set in the song's Backing track window
};

/** The piano roll for one part, with the other parts faint behind it. */
class RollView : public Component,
                 private ScrollBar::Listener
{
public:
    explicit RollView (MidiEditorComponent&);

    void setChannel (int ch)              { channel = ch; selection.clear(); if (placed) scrollToNotes(); repaint(); }
    int getChannel() const                { return channel; }
    void setSnap (int64 t)                { snap = jmax ((int64) 1, t); }
    void zoomBy (float factor, int aroundX = -1);
    void scrollToTick (int64 t);
    void scrollToNotes();
    void updateScrollBars();
    void setPlayhead (int64 t)            { if (t != playhead) { playhead = t; repaint(); } }
    void setCue (int64 t)                 { cue = t; repaint(); }
    void followPlayhead();
    void selectAll();
    void deleteSelection();
    void moveSelection (int64 dt, int dk);
    std::set<int> selection;              // indices into doc.notes

    void paint (Graphics&) override;
    void resized() override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    void mouseDoubleClick (const MouseEvent&) override;
    void mouseMove (const MouseEvent&) override;
    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails&) override;
    bool keyPressed (const KeyPress&) override;

private:
    void scrollBarMoved (ScrollBar*, double) override;
    Rectangle<int> grid() const       { return { kKeys, kRuler, getWidth() - kKeys - kBar, getHeight() - kRuler - kVel - kBar }; }
    Rectangle<int> velArea() const    { return { kKeys, getHeight() - kVel - kBar, getWidth() - kKeys - kBar, kVel }; }
    float xOf (int64 t) const         { return (float) grid().getX() + (float) (t - scrollTick) * pxPerTick; }
    int64 tickAt (float x) const      { return scrollTick + (int64) ((x - (float) grid().getX()) / pxPerTick); }
    float yOf (int key) const         { return (float) grid().getY() + (float) (topKey - key) * kKeyH; }
    int keyAt (float y) const         { return topKey - (int) std::floor ((y - (float) grid().getY()) / kKeyH); }
    int64 snapped (int64 t) const     { return (int64) std::llround ((double) t / (double) snap) * snap; }
    int noteAt (Point<float>, bool& onEdge) const;
    void commit (const std::vector<MidiDoc::Note>& before);

    static constexpr int kKeys = 46, kRuler = 22, kVel = 60, kBar = 12;
    static constexpr float kKeyH = 11.0f;

    MidiEditorComponent& editor;
    MidiDoc& doc;
    int channel = 1;
    int64 snap = 240, scrollTick = 0, playhead = -1, cue = 0, lastLength = 0;
    float pxPerTick = 0.1f;
    int topKey = 84;
    bool placed = false;
    ScrollBar hbar { false }, vbar { true };

    enum class Drag { none, move, resize, marquee, velocity, cue };
    Drag drag = Drag::none;
    std::vector<MidiDoc::Note> dragBefore;
    Point<float> dragStart;
    int dragNote = -1;
    Rectangle<float> marquee;
};

class MidiEditorComponent : public Component,
                            private Timer
{
public:
    MidiEditorComponent (Engine& e, int song, int stem, std::function<void()> onChange)
        : engine (e), songIndex (song), stemIndex (stem), changed (std::move (onChange)), roll (*this)
    {
        def = engine.getSongs()[(size_t) songIndex].stems[(size_t) stemIndex];
        file = engine.getChartsFolder().getChildFile (def.file);
        loaded = doc.load (file, loadError);
        trackInput = engine.getTrackInput();
        trackProgram = engine.getSongs()[(size_t) songIndex].programFor (trackInput);

        for (auto* b : { &playBtn, &saveBtn, &undoBtn, &redoBtn, &zoomIn, &zoomOut, &addPart })
            addAndMakeVisible (b);
        addAndMakeVisible (snapBox);
        addAndMakeVisible (status);
        addAndMakeVisible (lanesView);
        addAndMakeVisible (roll);
        status.setColour (Label::textColourId, textDim);
        lanesView.setViewedComponent (&lanes, false);
        lanesView.setScrollBarsShown (true, false);

        playBtn.onClick = [this] { togglePlay(); };
        saveBtn.onClick = [this] { save(); };
        undoBtn.onClick = [this] { undo.undo(); afterEdit(); };
        redoBtn.onClick = [this] { undo.redo(); afterEdit(); };
        zoomIn.onClick  = [this] { roll.zoomBy (1.5f); };
        zoomOut.onClick = [this] { roll.zoomBy (1.0f / 1.5f); };
        addPart.onClick = [this] { showAddPart(); };
        addPart.setTooltip ("Start a new part on a channel the file does not use yet");
        playBtn.setTooltip ("Play the song's track from the cue (click the ruler to move it). What plays is the saved file: Save to hear your edits.");
        saveBtn.setTooltip ("Write the notes back to the file. The first version is kept beside it as .orig.mid");

        const int q = doc.ticksPerQuarter;
        snapBox.addItem ("Snap: bar", 1);
        snapBox.addItem ("Snap: 1/4", 2);
        snapBox.addItem ("Snap: 1/8", 3);
        snapBox.addItem ("Snap: 1/16", 4);
        snapBox.addItem ("Snap: 1/32", 5);
        snapBox.addItem ("Snap: 1/8 triplet", 6);
        snapBox.addItem ("Snap: off", 7);
        snapBox.onChange = [this, q]
        {
            const int64 v[] = { doc.ticksPerBar(), q, q / 2, q / 4, q / 8, q / 3, 1 };
            roll.setSnap (v[jlimit (0, 6, snapBox.getSelectedId() - 1)]);
        };
        snapBox.setSelectedId (4);

        for (int ch = 1; ch <= 16; ++ch)
            if (doc.channelUsed (ch)) addLane (ch);
        if (! laneRows.empty()) selectChannel (laneRows.front()->channel);
        updateButtons();
        startTimerHz (30);
    }

    ~MidiEditorComponent() override { stopTimer(); }

    // --- for the lanes and the roll -------------------------------------------------
    Engine& engine;
    const int songIndex, stemIndex;
    MidiDoc doc;
    StemDef def;
    UndoManager undo;
    int selectedChannel = 1;
    int trackInput = -1, trackProgram = -1;      // where the file plays: the backing-track input's program for this song

    /** The track program, or null when the song has none (nothing plays the file). */
    const ProgramDef* program() const
    {
        const auto& inputs = engine.getSetup().inputs;
        if (trackInput < 0 || trackInput >= (int) inputs.size() || trackProgram < 0) return nullptr;
        return &inputs[(size_t) trackInput].programs[(size_t) trackProgram];
    }
    bool partMuted (int ch) const
    {
        auto* p = program();
        const int sl = engine.partInstrument (trackInput, trackProgram, ch);
        if (p == nullptr || sl < 0) return true;
        const auto& sd = p->slots[(size_t) sl];
        return ! sd.enabled || (sd.isGm() && sd.gmChannels[(size_t) ch - 1].muted);
    }

    bool isDirty() const          { return dirty; }
    const File& getFile() const   { return file; }

    void selectChannel (int ch)
    {
        selectedChannel = ch;
        roll.setChannel (ch);
        for (auto& l : laneRows) l->repaint();
    }

    void afterEdit()
    {
        dirty = true;
        roll.selection.clear();
        for (auto& l : laneRows) l->repaint();
        roll.updateScrollBars();
        roll.repaint();
        updateButtons();
    }

    void scrollRollTo (int64 tick)  { roll.scrollToTick (tick); }
    void repaintLanes()             { for (auto& l : laneRows) l->repaint(); }
    int64 viewStart = 0, viewEnd = 0;     // the roll's visible range, for the overviews

    bool save()
    {
        String error;
        if (! doc.save (file, error))
        {
            status.setText ("Not saved: " + error, dontSendNotification);
            return false;
        }
        dirty = false;
        engine.reloadTrack (songIndex);
        status.setText ("Saved " + file.getFileName(), dontSendNotification);
        updateButtons();
        return true;
    }

    void paint (Graphics& g) override
    {
        g.fillAll (bgPanel);
        if (! loaded)
        {
            g.setColour (Colours::orange);
            g.drawFittedText (file.getFileName() + ": " + loadError, getLocalBounds().reduced (20), Justification::centred, 3);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto bar = r.removeFromTop (28);
        playBtn.setBounds (bar.removeFromLeft (80));  bar.removeFromLeft (6);
        saveBtn.setBounds (bar.removeFromLeft (70));  bar.removeFromLeft (12);
        undoBtn.setBounds (bar.removeFromLeft (60));  bar.removeFromLeft (4);
        redoBtn.setBounds (bar.removeFromLeft (60));  bar.removeFromLeft (12);
        snapBox.setBounds (bar.removeFromLeft (150)); bar.removeFromLeft (12);
        zoomOut.setBounds (bar.removeFromLeft (30));  bar.removeFromLeft (2);
        zoomIn.setBounds (bar.removeFromLeft (30));   bar.removeFromLeft (12);
        addPart.setBounds (bar.removeFromLeft (80));  bar.removeFromLeft (12);
        status.setBounds (bar);
        r.removeFromTop (6);
        const int lanesH = jlimit (LaneRow::height, LaneRow::height * 6, LaneRow::height * jmax (1, (int) laneRows.size()));
        lanesView.setBounds (r.removeFromTop (lanesH + 2));
        layoutLanes();
        r.removeFromTop (6);
        roll.setBounds (r);
        roll.setVisible (loaded);
    }

private:
    void addLane (int ch)
    {
        auto row = std::make_unique<LaneRow> (*this, ch);
        lanes.addAndMakeVisible (row.get());
        laneRows.push_back (std::move (row));
        std::sort (laneRows.begin(), laneRows.end(), [] (auto& a, auto& b) { return a->channel < b->channel; });
    }

    void layoutLanes()
    {
        const int w = lanesView.getWidth() - (laneRows.size() > 6 ? lanesView.getScrollBarThickness() : 0);
        lanes.setSize (jmax (1, w), jmax (1, (int) laneRows.size() * LaneRow::height));
        int y = 0;
        for (auto& l : laneRows) { l->setBounds (0, y, w, LaneRow::height); y += LaneRow::height; }
    }

    void showAddPart()
    {
        PopupMenu m;
        for (int ch = 1; ch <= 16; ++ch)
        {
            bool shown = false;
            for (auto& l : laneRows) shown = shown || l->channel == ch;
            m.addItem (ch, "Channel " + String (ch) + (ch == 10 ? "  (drums)" : String()), ! shown);
        }
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (addPart), [this] (int ch)
        {
            if (ch < 1) return;
            addLane (ch);
            resized();
            selectChannel (ch);
            status.setText ("Double-click in the roll to add notes to channel " + String (ch) + "; give the part an instrument in the song's Backing track window", dontSendNotification);
        });
    }

public:
    void togglePlay()
    {
        if (engine.getTrackSong() == songIndex && engine.isTrackRunning()) { engine.stopTrack(); return; }
        if (engine.getTrackSong() != songIndex) engine.selectSong (songIndex);
        engine.cueTrack (doc.tickToSeconds (cueTick));
        engine.startTrack();
        if (dirty) status.setText ("Playing the saved file: Save to hear your edits", dontSendNotification);
    }

private:
    void timerCallback() override
    {
        const bool playing = engine.getTrackSong() == songIndex && engine.isTrackRunning();
        playBtn.setButtonText (playing ? "Stop" : "Play");
        roll.setPlayhead (playing ? doc.secondsToTick (engine.getTrackPosition()) : -1);
        if (playing) roll.followPlayhead();
        for (auto& l : laneRows) if (playing || wasPlaying) l->repaint();
        wasPlaying = playing;
    }

    void updateButtons()
    {
        undoBtn.setEnabled (undo.canUndo());
        redoBtn.setEnabled (undo.canRedo());
        saveBtn.setEnabled (dirty);
        if (dirty && ! status.getText().startsWith ("Playing")) status.setText ("Unsaved changes", dontSendNotification);
    }

public:
    int64 cueTick = 0;
    void setCue (int64 t) { cueTick = jmax ((int64) 0, t); roll.setCue (cueTick); if (! engine.isTrackRunning() && engine.getTrackSong() == songIndex) engine.cueTrack (doc.tickToSeconds (cueTick)); }
    void showStatus (const String& s) { status.setText (s, dontSendNotification); }

private:
    std::function<void()> changed;
    File file;
    bool loaded = false, dirty = false, wasPlaying = false;
    String loadError;
    TextButton playBtn { "Play" }, saveBtn { "Save" }, undoBtn { "Undo" }, redoBtn { "Redo" }, zoomIn { "+" }, zoomOut { "-" }, addPart { "+ Part" };
    ComboBox snapBox;
    Label status;
    Viewport lanesView;
    Component lanes;
    std::vector<std::unique_ptr<LaneRow>> laneRows;
    RollView roll;

    friend class RollView;
};

//==============================================================================
// LaneRow
//==============================================================================
LaneRow::LaneRow (MidiEditorComponent& e, int ch) : channel (ch), editor (e)
{
    for (auto* c : { (Component*) &chLabel, (Component*) &name, (Component*) &plays })
        addAndMakeVisible (c);
    chLabel.setText ("Ch " + String (ch), dontSendNotification);
    chLabel.setFont (FontOptions (13.0f, Font::bold));
    name.setColour (Label::textColourId, textDim);
    plays.setColour (Label::textColourId, textDim);
    plays.setFont (FontOptions (12.0f));
    for (auto* c : { &chLabel, &name, &plays }) c->setInterceptsMouseClicks (false, false);
    refresh();
}

void LaneRow::refresh()
{
    const auto part = editor.doc.partName (channel);
    name.setText (part.isNotEmpty() ? part : (channel == 10 ? String ("Drums") : String ("Part")), dontSendNotification);
    plays.setText ("plays on  " + partInstrumentName (editor.engine, editor.trackInput, editor.trackProgram, channel, editor.doc.firstProgram (channel)), dontSendNotification);
    repaint();
}

void LaneRow::resized()
{
    auto r = getLocalBounds().reduced (4, 4);
    chLabel.setBounds (r.removeFromLeft (44));
    name.setBounds (r.removeFromLeft (150));
    plays.setBounds (r.removeFromLeft (300));
}

void LaneRow::paint (Graphics& g)
{
    const bool selected = editor.selectedChannel == channel;
    g.setColour (selected ? bgRow.brighter (0.12f) : bgRow);
    g.fillRect (getLocalBounds().reduced (0, 1));
    if (selected) { g.setColour (accent); g.fillRect (0, 1, 3, getHeight() - 2); }

    // The part's notes across the whole song, the roll's view and the playhead over them.
    auto ov = getLocalBounds().withTrimmedLeft (headerWidth).reduced (4, 6).toFloat();
    if (ov.getWidth() < 20) return;
    g.setColour (Colours::black.withAlpha (0.3f));
    g.fillRect (ov);
    const auto& doc = editor.doc;
    const double len = (double) jmax ((int64) 1, doc.lengthTicks());
    const bool silent = editor.partMuted (channel);
    g.setColour ((silent ? textDim.withAlpha (0.4f) : accent.withAlpha (0.85f)));
    int lo = 127, hi = 0;
    for (auto& n : doc.notes) if (n.channel == channel) { lo = jmin (lo, n.key); hi = jmax (hi, n.key); }
    const float span = (float) jmax (12, hi - lo + 1);
    for (auto& n : doc.notes)
        if (n.channel == channel)
        {
            const float x = ov.getX() + (float) (n.start / len) * ov.getWidth();
            const float w = jmax (1.0f, (float) ((n.end - n.start) / len) * ov.getWidth());
            const float y = ov.getBottom() - ((float) (n.key - lo) + 1.0f) / span * ov.getHeight();
            g.fillRect (x, y, w, jmax (1.0f, ov.getHeight() / span));
        }
    g.setColour (Colours::white.withAlpha (0.25f));
    const float vx0 = ov.getX() + (float) (editor.viewStart / len) * ov.getWidth();
    const float vx1 = ov.getX() + (float) (editor.viewEnd / len) * ov.getWidth();
    if (selected) g.drawRect (Rectangle<float> (vx0, ov.getY(), jmax (2.0f, vx1 - vx0), ov.getHeight()), 1.0f);
    if (editor.engine.getTrackSong() == editor.songIndex && editor.engine.isTrackRunning())
    {
        const float px = ov.getX() + (float) (doc.secondsToTick (editor.engine.getTrackPosition()) / len) * ov.getWidth();
        g.setColour (Colours::white);
        g.drawVerticalLine ((int) px, ov.getY(), ov.getBottom());
    }
}

void LaneRow::mouseDown (const MouseEvent& e)
{
    editor.selectChannel (channel);
    const auto ov = getLocalBounds().withTrimmedLeft (headerWidth).reduced (4, 6);
    if (ov.contains (e.getPosition()))
        editor.scrollRollTo ((int64) ((double) (e.x - ov.getX()) / ov.getWidth() * (double) editor.doc.lengthTicks()));
}

//==============================================================================
// RollView
//==============================================================================
namespace
{
    struct NotesAction : public UndoableAction
    {
        NotesAction (MidiDoc& d, std::vector<MidiDoc::Note> b, std::vector<MidiDoc::Note> a) : doc (d), before (std::move (b)), after (std::move (a)) {}
        bool perform() override { doc.notes = after; return true; }
        bool undo() override    { doc.notes = before; return true; }
        int getSizeInUnits() override { return (int) (before.size() + after.size()) * (int) sizeof (MidiDoc::Note); }
        MidiDoc& doc;
        std::vector<MidiDoc::Note> before, after;
    };

    bool isBlackKey (int k) { const int n = k % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; }
}

RollView::RollView (MidiEditorComponent& e) : editor (e), doc (e.doc)
{
    setWantsKeyboardFocus (true);
    addAndMakeVisible (hbar);
    addAndMakeVisible (vbar);
    hbar.addListener (this);
    vbar.addListener (this);
    lastLength = doc.ticksPerQuarter / 2;
    snap = doc.ticksPerQuarter / 4;
}

void RollView::resized()
{
    hbar.setBounds (kKeys, getHeight() - kBar, getWidth() - kKeys - kBar, kBar);
    vbar.setBounds (getWidth() - kBar, kRuler, kBar, getHeight() - kRuler - kVel - kBar);
    if (! placed && grid().getWidth() > 200)      // not the first, tiny layout: the real width
    {
        // A first view of about eight bars, around this part's notes.
        placed = true;
        pxPerTick = (float) grid().getWidth() / (float) jmax ((int64) 1, doc.ticksPerBar() * 8);
        scrollToNotes();
    }
    updateScrollBars();
}

void RollView::updateScrollBars()
{
    const auto g = grid();
    const double total = (double) (doc.lengthTicks() + doc.ticksPerBar() * 4);
    hbar.setRangeLimits (0.0, total, dontSendNotification);
    hbar.setCurrentRange ((double) scrollTick, g.getWidth() / pxPerTick, dontSendNotification);
    vbar.setRangeLimits (0.0, 128.0, dontSendNotification);
    vbar.setCurrentRange ((double) (127 - topKey), g.getHeight() / kKeyH, dontSendNotification);
    editor.viewStart = scrollTick;
    editor.viewEnd = scrollTick + (int64) (g.getWidth() / pxPerTick);
}

void RollView::scrollBarMoved (ScrollBar* bar, double start)
{
    if (bar == &hbar) scrollTick = jmax ((int64) 0, (int64) start);
    else              topKey = jlimit (0, 127, 127 - (int) start);
    updateScrollBars();
    repaint();
    editor.repaintLanes();
}

void RollView::zoomBy (float factor, int aroundX)
{
    const float x = aroundX >= 0 ? (float) aroundX : grid().getCentreX();
    const int64 anchor = tickAt (x);
    pxPerTick = jlimit (0.002f, 2.0f, pxPerTick * factor);
    scrollTick = jmax ((int64) 0, anchor - (int64) ((x - (float) grid().getX()) / pxPerTick));
    updateScrollBars();
    repaint();
}

void RollView::scrollToTick (int64 t)
{
    scrollTick = jmax ((int64) 0, t - (int64) (grid().getWidth() / pxPerTick / 4.0f));
    updateScrollBars();
    repaint();
}

void RollView::scrollToNotes()
{
    int lo = 127, hi = 0;
    int64 first = -1;
    for (auto& n : doc.notes)
        if (n.channel == channel) { lo = jmin (lo, n.key); hi = jmax (hi, n.key); if (first < 0) first = n.start; }
    if (first >= 0) topKey = jlimit (0, 127, (lo + hi) / 2 + (int) (grid().getHeight() / kKeyH / 2));
    updateScrollBars();
}

void RollView::followPlayhead()
{
    if (playhead < 0) return;
    const auto g = grid();
    if (xOf (playhead) > (float) g.getRight() - 20.0f || xOf (playhead) < (float) g.getX())
    {
        scrollTick = playhead;
        updateScrollBars();
        repaint();
    }
}

void RollView::paint (Graphics& g)
{
    const auto gr = grid();
    const auto va = velArea();
    g.fillAll (bgPanel.darker (0.3f));

    // Rows: the black keys darker, a line under every C.
    for (int k = 0; k < 128; ++k)
    {
        const float y = yOf (k);
        if (y > (float) gr.getBottom() || y + kKeyH < (float) gr.getY()) continue;
        g.setColour (isBlackKey (k) ? Colour (0xff17191d) : Colour (0xff1f2227));
        g.fillRect ((float) gr.getX(), y, (float) gr.getWidth(), kKeyH);
        if (k % 12 == 0) { g.setColour (Colours::white.withAlpha (0.12f)); g.drawHorizontalLine ((int) (y + kKeyH), (float) gr.getX(), (float) gr.getRight()); }
    }

    // Beats and bars, and the snap between when there is room.
    const int64 bar = doc.ticksPerBar(), beat = doc.ticksPerQuarter * 4 / jmax (1, doc.timeSigDenominator);
    const int64 fine = (snap > 1 && snap * pxPerTick >= 6.0f) ? snap : (beat * pxPerTick >= 6.0f ? beat : bar);
    const int64 firstLine = (scrollTick / fine) * fine;
    const int64 lastTick = tickAt ((float) gr.getRight());
    for (int64 t = firstLine; t <= lastTick; t += fine)
    {
        const float x = xOf (t);
        if (x < (float) gr.getX()) continue;
        g.setColour (Colours::white.withAlpha (t % bar == 0 ? 0.28f : t % beat == 0 ? 0.12f : 0.05f));
        g.drawVerticalLine ((int) x, (float) gr.getY(), (float) va.getBottom());
    }

    // The other parts, faint; then this one.
    g.saveState();
    g.reduceClipRegion (gr);
    for (auto& n : doc.notes)
    {
        if (n.channel == channel || n.end < scrollTick || n.start > lastTick) continue;
        g.setColour (Colours::white.withAlpha (0.08f));
        g.fillRect (xOf (n.start), yOf (n.key) + 1.0f, jmax (2.0f, (float) (n.end - n.start) * pxPerTick), kKeyH - 2.0f);
    }
    for (int i = 0; i < (int) doc.notes.size(); ++i)
    {
        const auto& n = doc.notes[(size_t) i];
        if (n.channel != channel || n.end < scrollTick || n.start > lastTick) continue;
        Rectangle<float> rr (xOf (n.start), yOf (n.key) + 1.0f, jmax (3.0f, (float) (n.end - n.start) * pxPerTick), kKeyH - 2.0f);
        const bool sel = selection.count (i) > 0;
        g.setColour ((sel ? selColour : accent).withMultipliedBrightness (0.55f + 0.45f * (float) n.velocity / 127.0f));
        g.fillRoundedRectangle (rr, 2.0f);
        g.setColour (sel ? Colours::white : Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (rr, 2.0f, 1.0f);
    }
    if (drag == Drag::marquee)
    {
        g.setColour (Colours::white.withAlpha (0.15f));
        g.fillRect (marquee);
        g.setColour (Colours::white.withAlpha (0.6f));
        g.drawRect (marquee, 1.0f);
    }
    g.restoreState();

    // Velocity lane.
    g.setColour (Colour (0xff15171b));
    g.fillRect (va);
    g.saveState();
    g.reduceClipRegion (va);
    for (int i = 0; i < (int) doc.notes.size(); ++i)
    {
        const auto& n = doc.notes[(size_t) i];
        if (n.channel != channel || n.start < scrollTick || n.start > lastTick) continue;
        const float h = (float) (va.getHeight() - 4) * (float) n.velocity / 127.0f;
        g.setColour (selection.count (i) > 0 ? selColour : accent);
        g.fillRect (xOf (n.start), (float) va.getBottom() - h, 3.0f, h);
    }
    g.restoreState();
    g.setColour (textDim);
    g.setFont (FontOptions (10.0f));
    g.drawText ("velocity", 0, va.getY(), kKeys - 2, 14, Justification::centredRight);

    // Keyboard.
    g.saveState();
    g.reduceClipRegion (0, gr.getY(), kKeys, gr.getHeight());
    for (int k = 0; k < 128; ++k)
    {
        const float y = yOf (k);
        g.setColour (isBlackKey (k) ? Colour (0xff222222) : Colour (0xffe8e8e8));
        g.fillRect (0.0f, y, (float) kKeys - 1.0f, kKeyH - 0.5f);
        if (k % 12 == 0)
        {
            g.setColour (Colours::black);
            g.setFont (FontOptions (9.5f));
            g.drawText ("C" + String (k / 12 - 1), 2, (int) y, kKeys - 6, (int) kKeyH, Justification::centredRight);
        }
    }
    g.restoreState();

    // Ruler: bar numbers, the song's sections, the cue.
    g.setColour (bgRow);
    g.fillRect (gr.getX(), 0, gr.getWidth(), kRuler);
    g.setFont (FontOptions (11.0f));
    const int64 barStep = jmax ((int64) 1, (int64) std::ceil (40.0f / jmax (0.0001f, bar * pxPerTick))) * bar;
    for (int64 t = (scrollTick / barStep) * barStep; t <= lastTick; t += barStep)
    {
        const float x = xOf (t);
        if (x < (float) gr.getX()) continue;
        g.setColour (textDim);
        g.drawText (String (t / bar + 1), (int) x + 3, 2, 40, kRuler - 4, Justification::centredLeft);
    }
    const auto& songs = editor.engine.getSongs();
    if (editor.songIndex < (int) songs.size())
        for (auto& m : songs[(size_t) editor.songIndex].markers)
        {
            const float x = xOf (doc.secondsToTick (m.seconds));
            if (x < (float) gr.getX() || x > (float) gr.getRight()) continue;
            g.setColour (Colours::gold);
            g.fillRect (x, 0.0f, 2.0f, (float) kRuler);
            g.drawText (m.name, (int) x + 4, 0, 120, 11, Justification::centredLeft);
        }
    if (const float cx = xOf (cue); cx >= (float) gr.getX())
    {
        Path tri;
        tri.addTriangle (cx - 5.0f, 0.0f, cx + 5.0f, 0.0f, cx, 9.0f);
        g.setColour (Colours::limegreen);
        g.fillPath (tri);
    }

    if (playhead >= 0)
        if (const float x = xOf (playhead); x >= (float) gr.getX() && x <= (float) gr.getRight())
        {
            g.setColour (Colours::white);
            g.drawVerticalLine ((int) x, 0.0f, (float) va.getBottom());
        }
}

int RollView::noteAt (Point<float> p, bool& onEdge) const
{
    // Topmost first: the last drawn wins.
    for (int i = (int) doc.notes.size(); --i >= 0;)
    {
        const auto& n = doc.notes[(size_t) i];
        if (n.channel != channel) continue;
        Rectangle<float> rr (xOf (n.start), yOf (n.key), jmax (3.0f, (float) (n.end - n.start) * pxPerTick), kKeyH);
        if (rr.contains (p))
        {
            onEdge = p.x > rr.getRight() - jmin (6.0f, rr.getWidth() / 3.0f);
            return i;
        }
    }
    return -1;
}

void RollView::commit (const std::vector<MidiDoc::Note>& before)
{
    if (before == doc.notes) return;
    // The selection is by index; keep it across the sort by remembering the notes themselves.
    std::vector<MidiDoc::Note> chosen;
    for (int i : selection) if (i < (int) doc.notes.size()) chosen.push_back (doc.notes[(size_t) i]);
    std::stable_sort (doc.notes.begin(), doc.notes.end(), [] (const MidiDoc::Note& a, const MidiDoc::Note& b) { return a.start < b.start; });
    editor.undo.beginNewTransaction();
    editor.undo.perform (new NotesAction (doc, before, doc.notes));
    editor.afterEdit();
    for (auto& c : chosen)
        for (int i = 0; i < (int) doc.notes.size(); ++i)
            if (doc.notes[(size_t) i] == c) { selection.insert (i); break; }
    repaint();
}

void RollView::mouseMove (const MouseEvent& e)
{
    bool edge = false;
    const bool inGrid = grid().contains (e.getPosition());
    const int n = inGrid ? noteAt (e.position, edge) : -1;
    setMouseCursor (n >= 0 ? (edge ? MouseCursor::LeftRightResizeCursor : MouseCursor::DraggingHandCursor) : MouseCursor::NormalCursor);
}

void RollView::mouseDown (const MouseEvent& e)
{
    grabKeyboardFocus();
    dragStart = e.position;
    dragBefore = doc.notes;
    drag = Drag::none;

    if (e.y < kRuler && e.x >= kKeys)
    {
        drag = Drag::cue;
        editor.setCue (snapped (tickAt (e.position.x)));
        return;
    }
    if (velArea().contains (e.getPosition()))
    {
        drag = Drag::velocity;
        mouseDrag (e);
        return;
    }
    if (e.x < kKeys || ! grid().contains (e.getPosition())) return;

    bool edge = false;
    dragNote = noteAt (e.position, edge);
    if (dragNote >= 0)
    {
        if (e.mods.isShiftDown()) { if (selection.count (dragNote)) selection.erase (dragNote); else selection.insert (dragNote); }
        else if (selection.count (dragNote) == 0) { selection.clear(); selection.insert (dragNote); }
        drag = edge ? Drag::resize : Drag::move;
        lastLength = doc.notes[(size_t) dragNote].end - doc.notes[(size_t) dragNote].start;
    }
    else
    {
        if (! e.mods.isShiftDown()) selection.clear();
        drag = Drag::marquee;
        marquee = {};
    }
    repaint();
}

void RollView::mouseDrag (const MouseEvent& e)
{
    switch (drag)
    {
        case Drag::cue:
            editor.setCue (snapped (tickAt (e.position.x)));
            break;
        case Drag::velocity:
        {
            const auto va = velArea();
            const int v = jlimit (1, 127, (int) std::lround ((va.getBottom() - e.position.y) / (float) (va.getHeight() - 4) * 127.0f));
            // The note under the mouse; if it is selected, every selected note.
            int hit = -1;
            float best = 6.0f;
            for (int i = 0; i < (int) doc.notes.size(); ++i)
                if (doc.notes[(size_t) i].channel == channel)
                    if (const float d = std::abs (xOf (doc.notes[(size_t) i].start) - e.position.x); d < best) { best = d; hit = i; }
            if (hit < 0) break;
            if (selection.count (hit)) for (int i : selection) doc.notes[(size_t) i].velocity = v;
            else doc.notes[(size_t) hit].velocity = v;
            editor.showStatus ("Velocity " + String (v));
            repaint();
            break;
        }
        case Drag::move:
        {
            const auto& orig = dragBefore[(size_t) dragNote];
            const int64 rawStart = orig.start + (int64) ((e.position.x - dragStart.x) / pxPerTick);
            const int64 dt = (e.mods.isAltDown() ? rawStart : snapped (rawStart)) - orig.start;
            const int dk = keyAt (e.position.y) - keyAt (dragStart.y);
            for (int i : selection)
            {
                const auto& o = dragBefore[(size_t) i];
                auto& n = doc.notes[(size_t) i];
                const int64 len = o.end - o.start;
                n.start = jmax ((int64) 0, o.start + dt);
                n.end = n.start + len;
                n.key = jlimit (0, 127, o.key + dk);
            }
            repaint();
            break;
        }
        case Drag::resize:
        {
            const auto& orig = dragBefore[(size_t) dragNote];
            const int64 rawEnd = orig.end + (int64) ((e.position.x - dragStart.x) / pxPerTick);
            const int64 de = (e.mods.isAltDown() ? rawEnd : snapped (rawEnd)) - orig.end;
            for (int i : selection)
            {
                const auto& o = dragBefore[(size_t) i];
                doc.notes[(size_t) i].end = jmax (o.start + jmax ((int64) 1, snap / 2), o.end + de);
            }
            repaint();
            break;
        }
        case Drag::marquee:
        {
            marquee = Rectangle<float> (dragStart, e.position);
            std::set<int> inside;
            for (int i = 0; i < (int) doc.notes.size(); ++i)
            {
                const auto& n = doc.notes[(size_t) i];
                if (n.channel != channel) continue;
                if (marquee.intersects (Rectangle<float> (xOf (n.start), yOf (n.key), jmax (3.0f, (float) (n.end - n.start) * pxPerTick), kKeyH)))
                    inside.insert (i);
            }
            selection = inside;
            repaint();
            break;
        }
        case Drag::none: break;
    }
}

void RollView::mouseUp (const MouseEvent&)
{
    const auto was = drag;
    drag = Drag::none;
    if (was == Drag::move || was == Drag::resize || was == Drag::velocity)
    {
        if (was == Drag::resize && dragNote >= 0) lastLength = doc.notes[(size_t) dragNote].end - doc.notes[(size_t) dragNote].start;
        commit (dragBefore);
    }
    repaint();
}

void RollView::mouseDoubleClick (const MouseEvent& e)
{
    if (! grid().contains (e.getPosition())) return;
    bool edge = false;
    if (const int hit = noteAt (e.position, edge); hit >= 0)
    {
        // Double-clicking a note takes it away.
        const auto before = doc.notes;
        doc.notes.erase (doc.notes.begin() + hit);
        selection.clear();
        commit (before);
        return;
    }
    const auto before = doc.notes;
    MidiDoc::Note n;
    n.channel = channel;
    n.track = doc.trackFor (channel);
    n.key = jlimit (0, 127, keyAt (e.position.y));
    n.start = jmax ((int64) 0, (tickAt (e.position.x) / snap) * snap);
    n.end = n.start + jmax ((int64) 1, lastLength > 0 ? lastLength : snap);
    n.velocity = 100;
    doc.notes.push_back (n);
    selection.clear();
    selection.insert ((int) doc.notes.size() - 1);
    commit (before);
}

void RollView::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (e.mods.isCommandDown()) { zoomBy (w.deltaY > 0 ? 1.25f : 0.8f, e.x); return; }
    if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        scrollTick = jmax ((int64) 0, scrollTick - (int64) (d * 200.0f / pxPerTick));
    }
    else
        topKey = jlimit (10, 127, topKey + (int) std::round (w.deltaY * 12.0f));
    updateScrollBars();
    repaint();
}

void RollView::selectAll()
{
    selection.clear();
    for (int i = 0; i < (int) doc.notes.size(); ++i) if (doc.notes[(size_t) i].channel == channel) selection.insert (i);
    repaint();
}

void RollView::deleteSelection()
{
    if (selection.empty()) return;
    const auto before = doc.notes;
    for (auto it = selection.rbegin(); it != selection.rend(); ++it)
        if (*it < (int) doc.notes.size()) doc.notes.erase (doc.notes.begin() + *it);
    selection.clear();
    commit (before);
}

void RollView::moveSelection (int64 dt, int dk)
{
    if (selection.empty()) return;
    const auto before = doc.notes;
    for (int i : selection)
    {
        auto& n = doc.notes[(size_t) i];
        const int64 len = n.end - n.start;
        n.start = jmax ((int64) 0, n.start + dt);
        n.end = n.start + len;
        n.key = jlimit (0, 127, n.key + dk);
    }
    commit (before);
}

bool RollView::keyPressed (const KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (k == KeyPress::deleteKey || k == KeyPress::backspaceKey) { deleteSelection(); return true; }
    if (mods.isCommandDown() && k.getKeyCode() == 'A') { selectAll(); return true; }
    if (mods.isCommandDown() && k.getKeyCode() == 'Z')
    {
        if (mods.isShiftDown()) editor.undo.redo(); else editor.undo.undo();
        editor.afterEdit();
        return true;
    }
    if (mods.isCommandDown() && k.getKeyCode() == 'Y') { editor.undo.redo(); editor.afterEdit(); return true; }
    if (mods.isCommandDown() && k.getKeyCode() == 'S') { editor.save(); return true; }
    if (k.getKeyCode() == KeyPress::upKey)    { moveSelection (0, mods.isShiftDown() ? 12 : 1); return true; }
    if (k.getKeyCode() == KeyPress::downKey)  { moveSelection (0, mods.isShiftDown() ? -12 : -1); return true; }
    if (k.getKeyCode() == KeyPress::leftKey)  { moveSelection (-snap, 0); return true; }
    if (k.getKeyCode() == KeyPress::rightKey) { moveSelection (snap, 0); return true; }
    if (k.getKeyCode() == KeyPress::spaceKey) { editor.togglePlay(); return true; }
    return false;
}

//==============================================================================
// The window
//==============================================================================
namespace
{
    class MidiEditorWindow : public DocumentWindow
    {
    public:
        MidiEditorWindow (Engine& engine, int song, int stem, std::function<void()> changed)
            : DocumentWindow ("Notes: " + engine.getSongs()[(size_t) song].stems[(size_t) stem].displayName()
                                + "  (" + engine.getSongs()[(size_t) song].name + ")",
                              bgPanel, DocumentWindow::allButtons)
        {
            content = new MidiEditorComponent (engine, song, stem, std::move (changed));
            setContentOwned (content, false);
            setUsingNativeTitleBar (true);
            setResizable (true, true);
            setResizeLimits (900, 420, 10000, 10000);
            centreWithSize (jmin (1400, getParentMonitorArea().getWidth() - 80), jmin (860, getParentMonitorArea().getHeight() - 80));
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            if (! content->isDirty()) { close(); return; }
            SafePointer<MidiEditorWindow> self (this);
            AlertWindow::showYesNoCancelBox (MessageBoxIconType::QuestionIcon, "Save changes?",
                                             "Save your edits to " + content->getFile().getFileName() + "?",
                                             "Save", "Discard", "Cancel", this,
                                             ModalCallbackFunction::create ([self] (int r)
                                             {
                                                 if (self == nullptr) return;
                                                 if (r == 1 && ! self->content->save()) return;
                                                 if (r == 1 || r == 2) self->close();
                                             }));
        }

    private:
        void close()
        {
            setVisible (false);
            MessageManager::callAsync ([w = this] { delete w; });
        }
        MidiEditorComponent* content = nullptr;
    };
}

void showMidiEditor (Engine& engine, int songIndex, int stemIndex, std::function<void()> changed)
{
    const auto& songs = engine.getSongs();
    if (songIndex < 0 || songIndex >= (int) songs.size() || stemIndex < 0 || stemIndex >= (int) songs[(size_t) songIndex].stems.size()) return;
    new MidiEditorWindow (engine, songIndex, stemIndex, std::move (changed));     // deletes itself when closed
}

} // namespace perf
