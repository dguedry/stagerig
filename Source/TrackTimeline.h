#pragma once

#include "Engine.h"
#include "MidiEditor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include <memory>
#include <vector>

namespace perf
{

/** A song's backing track drawn along time -- each audio file as a waveform,
    each MIDI file as its notes -- with the sections above it, so a section
    can be set by looking and listening rather than by typing a time.

    Click anywhere to put the green cue there and Play from it. Double-click
    to start a section there; drag a section's flag to move it (it snaps to
    bars when the song's tempo is known; hold Alt to drag freely); right-click
    a section to rename it, make it a vamp, go to it or remove it. Ctrl+wheel
    zooms around the mouse, the wheel scrolls, Space plays and stops. While
    the track plays, clicking a section queues it, as the Go button does. */
class TrackTimeline : public juce::Component,
                      private juce::Timer,
                      private juce::ChangeListener,
                      private juce::ScrollBar::Listener
{
public:
    TrackTimeline (Engine&, int songIndex);
    ~TrackTimeline() override;

    /** Called after this component changes the song's markers, so a list
        beside it can refresh. */
    std::function<void()> onMarkersChanged;
    /** A line of text about what the mouse is over, for a status label. */
    std::function<void (const juce::String&)> onHint;

    /** Re-reads the song: its files and markers. Cheap when nothing changed. */
    void refresh();

    void togglePlay();
    bool isPlaying() const;
    /** A new section at the playhead while playing, otherwise at the cue. */
    void markHere();
    void zoomBy (float factor);
    void zoomToFit();

    int preferredHeight() const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    struct Lane
    {
        StemDef def;
        std::unique_ptr<juce::AudioThumbnail> thumb;   // an audio file
        std::unique_ptr<MidiDoc> doc;                  // a MIDI file
        int lowKey = 0, highKey = 127;
        double length = 0.0;                           // seconds
    };

    static constexpr int kRuler = 28, kSections = 22, kNames = 96, kScroll = 12, kMinLane = 30, kMaxLane = 70;

    const SongDef* song() const;
    std::vector<MarkerDef> markers() const;
    void setMarkers (std::vector<MarkerDef>, bool notify);
    double lengthSeconds() const;
    juce::Rectangle<int> timeArea() const;             // everything right of the names, above the scrollbar
    int laneHeight() const;
    float xOf (double seconds) const;
    double secondsAt (float x) const;
    double visibleSeconds() const;
    void clampScroll();
    void updateScrollBar();
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    /** Bar lines, when the song's tempo is known: a MIDI file's own tempo map,
        else the song's tempo (or the current one) in 4/4. */
    bool barsKnown() const;
    int beatsPerBar() const;
    double barAt (int bar) const;                      // seconds
    double barOf (double seconds) const;               // fractional bar number
    double snapped (double seconds, bool free) const;

    int markerAt (juce::Point<int>) const;             // a flag or its line, or -1
    int sectionBlockAt (juce::Point<int>) const;       // a block in the sections band, or -1
    void showMarkerMenu (int index, juce::Point<int> at);
    void renameMarker (int index);
    void removeMarker (int index);
    void goToMarker (int index);
    void setCue (double seconds);
    void followPlayhead();
    void hint (const juce::String&);
    static juce::String formatTime (double seconds);

    Engine& engine;
    const int songIndex;
    juce::AudioFormatManager formats;
    juce::AudioThumbnailCache thumbCache { 32 };
    std::vector<Lane> lanes;
    std::vector<juce::String> laneFiles;               // to know when the stems changed
    juce::ScrollBar scrollBar { false };

    double pxPerSecond = 0.0;                          // 0 until first laid out: fit
    bool atFit = true;                                 // the whole song is shown, so a resize refits
    double scrollSeconds = 0.0;
    double cue = 0.0;
    int selected = -1;                                 // a marker
    int dragging = -1;
    double dragStartSeconds = 0.0;
    bool dragMoved = false;
    bool wasPlaying = false;
    double hoverSeconds = -1.0;
};

} // namespace perf
