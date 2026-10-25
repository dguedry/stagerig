#pragma once

#include "Engine.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace perf
{

/** Building and playing set lists.

    Two lists side by side: every song this setup knows, and the running order
    of the chosen set. The gap this fills is that a song is otherwise only
    reachable by hand-editing the setup file -- the engine has known about songs
    and sets for a while, and the tablet plays them, but nothing here could make
    one.

    Built around Capture rather than a form. Getting a song right means picking
    the sound on each keyboard and settling on a tempo, which is exactly what
    the rest of the window is for; so you do that, then press Capture and the
    song remembers it. Typing program numbers into boxes would be the same work
    done twice, and wrong more often. */
class SongsPanel : public juce::Component,
                   public juce::FileDragAndDropTarget,
                   private juce::ListBoxModel,
                   private juce::Timer
{
public:
    /* Narrow on purpose: song names and a tempo, nothing else. The panel
       shares a 1280-wide window with four others, and every pixel it takes
       comes out of the mappings table, which has real columns to fit. */
    static constexpr int preferredWidth = 230;
    static constexpr int minimumWidth   = 190;

    explicit SongsPanel (Engine&);
    ~SongsPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Called when the setup changes underneath us -- a song selected from the
        tablet, a footswitch step, a file opened. */
    void refresh();

private:
    // ListBoxModel: one model serves both lists, told apart by which box asks.
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    void timerCallback() override;

    /** Which list a click came from: the ListBox that currently has focus, or
        the one we are painting. JUCE hands a model no sense of "which box", so
        the two lists get their own thin models. */
    struct ListProxy : public juce::ListBoxModel
    {
        ListProxy (SongsPanel& o, bool set) : owner (o), isSet (set) {}
        int getNumRows() override;
        void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool sel) override;
        void listBoxItemClicked (int row, const juce::MouseEvent&) override;
        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
        void returnKeyPressed (int row) override;
        void play (int row);
        SongsPanel& owner;
        const bool isSet;
    };

    juce::String describeCurrent() const;
    void addSong();
    void removeSong();
    void captureIntoSelectedSong();
    void editSelectedSong();
    void addToSet();
    void removeFromSet();
    void moveInSet (int delta);
    void moveSongInList (int delta);
    void chooseSet();
    void newSet();
    void showSetMenu();
    void showFootswitchMenu();

    /** The song a row of the set list refers to, or -1. */
    int songAtSetPosition (int position) const;

    Engine& engine;

    juce::Label songsHeader { {}, "SONGS" }, setHeader { {}, "SET LIST" };
    juce::ListBox songsList, setList;
    ListProxy songsModel { *this, false }, setModel { *this, true };

    juce::TextButton addBtn { "+" }, removeBtn { "-" },
                     captureBtn { "Capture" }, editBtn { "Edit..." }, chartBtn { "Attach..." };
    juce::TextButton toSetBtn { "Add to set list >" }, fromSetBtn { "< Remove" },
                     upBtn { "^" }, downBtn { "v" };
    juce::TextButton songUpBtn { "^" }, songDownBtn { "v" };
    juce::TextButton setBtn { "Set list..." }, footBtn { "Footswitches..." };

    // The backing track of the song in play: start/stop, and where it is.
    juce::TextButton playBtn { "Play" }, nextBtn { "Next" };
    juce::Label trackLabel;
    void refreshTransport();
    void showTracksDialog (int songIndex);
public:
    /** Copies an audio file beside the setup and adds it to the song as a stem. */
    void addTrackFile (const juce::File&, int songIndex);
private:

    int selectedSong = -1;        // index into engine.getSongs()
    int selectedInSet = -1;       // position within the current set

    std::unique_ptr<juce::FileChooser> chooser;

    // Sheet music: a file copied into the setup's charts folder and listed on the song.
    void showChartMenu();
    void attachChartFile (const juce::File&, int songIndex);
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongsPanel)
};

} // namespace perf
