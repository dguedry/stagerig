#include "TrackTimeline.h"
#include <algorithm>
#include <cmath>

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
    const Colour gold      { 0xffe0b64a };
    const Colour vampCol   { 0xff8a6a2a };

    constexpr double kMaxPxPerSecond = 400.0;
    constexpr int kGrab = 6;           // pixels either side of a marker's line that pick it up
    constexpr int kSnapPx = 8;         // how close to a bar line a dragged marker snaps
}

TrackTimeline::TrackTimeline (Engine& e, int song) : engine (e), songIndex (song)
{
    formats.registerBasicFormats();
    setWantsKeyboardFocus (true);
    addAndMakeVisible (scrollBar);
    scrollBar.setAutoHide (false);
    scrollBar.addListener (this);
    refresh();
    if (engine.getTrackSong() == songIndex) cue = engine.getTrackCue();
    startTimerHz (30);
}

TrackTimeline::~TrackTimeline()
{
    stopTimer();
    scrollBar.removeListener (this);
    for (auto& l : lanes)
        if (l.thumb != nullptr) l.thumb->removeChangeListener (this);
}

//==============================================================================
const SongDef* TrackTimeline::song() const
{
    const auto& songs = engine.getSongs();
    return songIndex >= 0 && songIndex < (int) songs.size() ? &songs[(size_t) songIndex] : nullptr;
}

std::vector<MarkerDef> TrackTimeline::markers() const
{
    auto* s = song();
    return s != nullptr ? s->markers : std::vector<MarkerDef>();
}

void TrackTimeline::setMarkers (std::vector<MarkerDef> m, bool notify)
{
    engine.setSongMarkers (songIndex, m);
    repaint();
    if (notify && onMarkersChanged) onMarkersChanged();
}

void TrackTimeline::refresh()
{
    auto* s = song();
    std::vector<String> files;
    if (s != nullptr) for (auto& st : s->stems) files.push_back (st.file);
    if (files == laneFiles) { repaint(); return; }
    laneFiles = files;

    for (auto& l : lanes)
        if (l.thumb != nullptr) l.thumb->removeChangeListener (this);
    lanes.clear();

    if (s != nullptr)
        for (auto& st : s->stems)
        {
            Lane lane;
            lane.def = st;
            const File f = engine.getChartsFolder().getChildFile (st.file);
            if (st.isMidi())
            {
                lane.doc = std::make_unique<MidiDoc>();
                String err;
                if (lane.doc->load (f, err))
                {
                    lane.length = lane.doc->tickToSeconds (lane.doc->lengthTicks());
                    int lo = 127, hi = 0;
                    for (auto& n : lane.doc->notes) { lo = jmin (lo, n.key); hi = jmax (hi, n.key); }
                    if (lo > hi) { lo = 36; hi = 84; }
                    lane.lowKey = jmax (0, lo - 1);
                    lane.highKey = jmin (127, hi + 1);
                }
                else
                    lane.doc.reset();
            }
            else
            {
                lane.thumb = std::make_unique<AudioThumbnail> (512, formats, thumbCache);
                lane.thumb->addChangeListener (this);
                if (lane.thumb->setSource (new FileInputSource (f)))
                    lane.length = lane.thumb->getTotalLength();
                else
                    lane.thumb.reset();
            }
            lanes.push_back (std::move (lane));
        }

    pxPerSecond = 0.0;        // fit again: the length may have changed
    resized();
    repaint();
}

void TrackTimeline::changeListenerCallback (ChangeBroadcaster* src)
{
    for (auto& l : lanes)
        if (l.thumb.get() == src)
        {
            const double len = l.thumb->getTotalLength();
            if (std::abs (len - l.length) > 0.01) { l.length = len; updateScrollBar(); }
        }
    repaint();
}

double TrackTimeline::lengthSeconds() const
{
    double len = 0.0;
    for (auto& l : lanes) len = jmax (len, l.length);
    if (engine.getTrackSong() == songIndex) len = jmax (len, engine.getTrackLength());
    for (auto& m : markers()) len = jmax (len, m.seconds + 5.0);
    return jmax (10.0, len);
}

//==============================================================================
Rectangle<int> TrackTimeline::timeArea() const
{
    return getLocalBounds().withTrimmedLeft (kNames).withTrimmedBottom (kScroll);
}

int TrackTimeline::laneHeight() const
{
    const int h = timeArea().getHeight() - kRuler - kSections;
    return jmax (1, h / jmax (1, (int) lanes.size()));
}

int TrackTimeline::preferredHeight() const
{
    const int n = jmax (1, (int) lanes.size());
    const int laneH = jlimit (kMinLane, kMaxLane, 150 / n);
    return kRuler + kSections + n * laneH + kScroll;
}

float TrackTimeline::xOf (double seconds) const
{
    return (float) timeArea().getX() + (float) ((seconds - scrollSeconds) * pxPerSecond);
}

double TrackTimeline::secondsAt (float x) const
{
    return pxPerSecond > 0.0 ? scrollSeconds + (x - (float) timeArea().getX()) / pxPerSecond : 0.0;
}

double TrackTimeline::visibleSeconds() const
{
    return pxPerSecond > 0.0 ? timeArea().getWidth() / pxPerSecond : lengthSeconds();
}

void TrackTimeline::clampScroll()
{
    scrollSeconds = jlimit (0.0, jmax (0.0, lengthSeconds() - visibleSeconds()), scrollSeconds);
}

void TrackTimeline::updateScrollBar()
{
    scrollBar.setRangeLimits (0.0, lengthSeconds(), dontSendNotification);
    scrollBar.setCurrentRange (scrollSeconds, visibleSeconds(), dontSendNotification);
}

void TrackTimeline::scrollBarMoved (ScrollBar*, double newRangeStart)
{
    scrollSeconds = newRangeStart;
    clampScroll();
    repaint();
}

void TrackTimeline::resized()
{
    scrollBar.setBounds (getLocalBounds().removeFromBottom (kScroll).withTrimmedLeft (kNames));
    if (pxPerSecond <= 0.0 || atFit) zoomToFit();     // showing the whole song: keep showing it at the new width
    else { clampScroll(); updateScrollBar(); }
}

void TrackTimeline::zoomToFit()
{
    const int w = timeArea().getWidth();
    if (w <= 0) { pxPerSecond = 0.0; return; }     // not laid out yet: fit on the first real resize
    pxPerSecond = w / lengthSeconds();
    scrollSeconds = 0.0;
    atFit = true;
    updateScrollBar();
    repaint();
}

void TrackTimeline::zoomBy (float factor)
{
    const int w = jmax (1, timeArea().getWidth());
    const double fit = w / lengthSeconds();
    const double anchor = hoverSeconds >= 0.0 ? hoverSeconds : scrollSeconds + visibleSeconds() / 2.0;
    const float anchorX = xOf (anchor);
    pxPerSecond = jlimit (fit, kMaxPxPerSecond, pxPerSecond * (double) factor);
    atFit = pxPerSecond <= fit + 1e-9;
    scrollSeconds = anchor - (anchorX - (float) timeArea().getX()) / pxPerSecond;
    clampScroll();
    updateScrollBar();
    repaint();
}

//==============================================================================
bool TrackTimeline::barsKnown() const
{
    for (auto& l : lanes) if (l.doc != nullptr) return true;
    auto* s = song();
    return s != nullptr && s->tempoBpm > 0.0;
}

int TrackTimeline::beatsPerBar() const
{
    for (auto& l : lanes)
        if (l.doc != nullptr) return jmax (1, l.doc->timeSigNumerator);
    auto* s = song();
    return s != nullptr ? jmax (1, s->beatsPerBar) : 4;
}

double TrackTimeline::barAt (int bar) const
{
    for (auto& l : lanes)
        if (l.doc != nullptr) return l.doc->tickToSeconds ((int64) bar * l.doc->ticksPerBar());
    auto* s = song();
    return s != nullptr && s->tempoBpm > 0.0 ? s->firstBeat + bar * jmax (1, s->beatsPerBar) * 60.0 / s->tempoBpm : 0.0;
}

double TrackTimeline::barOf (double seconds) const
{
    for (auto& l : lanes)
        if (l.doc != nullptr) return (double) l.doc->secondsToTick (seconds) / (double) jmax ((int64) 1, l.doc->ticksPerBar());
    auto* s = song();
    return s != nullptr && s->tempoBpm > 0.0 ? (seconds - s->firstBeat) * s->tempoBpm / (60.0 * jmax (1, s->beatsPerBar)) : 0.0;
}

double TrackTimeline::snapped (double seconds, bool free) const
{
    seconds = jlimit (0.0, lengthSeconds(), seconds);
    if (free || ! barsKnown()) return seconds;
    const double b = barAt ((int) std::llround (barOf (seconds)));
    return std::abs ((b - seconds) * pxPerSecond) <= kSnapPx ? b : seconds;
}

String TrackTimeline::formatTime (double s)
{
    const int m = (int) (s / 60.0);
    return String (m) + ":" + String (s - m * 60.0, 1).paddedLeft ('0', 4);
}

//==============================================================================
void TrackTimeline::paint (Graphics& g)
{
    g.fillAll (bgPanel);
    const auto area = timeArea();
    const int laneH = laneHeight();
    const int lanesTop = area.getY() + kRuler + kSections;
    const int lanesBottom = area.getBottom();
    const double t0 = scrollSeconds, t1 = scrollSeconds + visibleSeconds();

    // Lanes: a waveform per audio file, the notes of a MIDI file.
    if (lanes.empty())
    {
        g.setColour (bgRow);
        g.fillRect (area.getX(), lanesTop, area.getWidth(), lanesBottom - lanesTop);
        g.setColour (textDim);
        g.setFont (FontOptions (12.0f));
        g.drawText ("No files yet: press  + Add audio or MIDI...  below. Sections can still be set here by time.",
                    area.getX(), lanesTop, area.getWidth(), lanesBottom - lanesTop, Justification::centred);
    }
    int y = lanesTop;
    for (size_t i = 0; i < lanes.size(); ++i)
    {
        auto& l = lanes[i];
        const Rectangle<int> lr (area.getX(), y, area.getWidth(), laneH);
        g.setColour (i % 2 ? bgRow : bgRow.darker (0.12f));
        g.fillRect (lr);
        g.setColour (textDim);
        g.setFont (FontOptions (11.0f));
        g.drawFittedText (l.def.displayName(), 4, y, kNames - 8, laneH, Justification::centredLeft, 2);

        g.saveState();
        g.reduceClipRegion (lr);
        if (l.thumb != nullptr)
        {
            const double end = jmin (t1, l.thumb->getTotalLength());
            if (end > t0)
            {
                Rectangle<int> wr (lr.getX(), lr.getY() + 3, (int) std::ceil ((end - t0) * pxPerSecond), laneH - 6);
                g.setColour (accent.withAlpha (0.85f));
                l.thumb->drawChannels (g, wr, t0, end, 1.0f);
            }
            if (! l.thumb->isFullyLoaded())
            {
                g.setColour (textDim);
                g.drawText ("reading the file...", lr.reduced (6, 0), Justification::centredLeft);
            }
        }
        else if (l.doc != nullptr)
        {
            const float keyH = (float) (laneH - 6) / (float) (l.highKey - l.lowKey + 1);
            for (auto& n : l.doc->notes)
            {
                const double ns = l.doc->tickToSeconds (n.start), ne = l.doc->tickToSeconds (n.end);
                if (ne < t0 || ns > t1) continue;
                const float x0 = xOf (ns), x1 = xOf (ne);
                g.setColour (Colour::fromHSV ((float) (n.channel - 1) / 16.0f, 0.5f, 0.95f, 1.0f));
                g.fillRect (x0, (float) y + 3.0f + (float) (l.highKey - n.key) * keyH, jmax (1.0f, x1 - x0), jmax (1.0f, keyH));
            }
        }
        else
        {
            g.setColour (textDim);
            g.drawText ("This file could not be read", lr.reduced (6, 0), Justification::centredLeft);
        }
        g.restoreState();
        g.setColour (bgPanel);
        g.drawHorizontalLine (lr.getBottom() - 1, 0.0f, (float) getWidth());
        y += laneH;
    }

    // Ruler.
    g.setColour (bgRow.darker (0.3f));
    g.fillRect (area.getX(), area.getY(), area.getWidth(), kRuler);
    g.setFont (FontOptions (10.5f));
    {
        const double steps[] = { 0.1, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 };
        double step = 600.0;
        for (double s : steps) if (s * pxPerSecond >= 64.0) { step = s; break; }
        for (double t = std::floor (t0 / step) * step; t <= t1; t += step)
        {
            const float x = xOf (t);
            if (x < (float) area.getX()) continue;
            g.setColour (textDim.withAlpha (0.5f));
            g.drawVerticalLine ((int) x, (float) area.getY() + 9.0f, (float) area.getY() + 14.0f);
            g.setColour (textDim);
            g.drawText (formatTime (t), (int) x + 3, area.getY(), 60, 14, Justification::centredLeft);
        }
    }

    // Bars, when the tempo is known: faint lines down the lanes, numbers in the ruler's lower half.
    if (barsKnown() && ! lanes.empty())
    {
        const double barPx = (barAt (1) - barAt (0)) * pxPerSecond;
        if (barPx >= 4.0)
        {
            int labelEvery = 1;
            while (labelEvery * barPx < 44.0) labelEvery *= 2;
            const int first = (int) std::floor (barOf (t0));          // negative before the first beat: lines, no numbers
            const int last = (int) std::ceil (barOf (t1));
            for (int b = first; b <= last; ++b)
            {
                const float x = xOf (barAt (b));
                const bool onScreen = x >= (float) area.getX() && x <= (float) area.getRight();
                const bool labelled = b >= 0 && b % labelEvery == 0;
                if (onScreen)
                {
                    g.setColour (Colours::white.withAlpha (labelled ? 0.22f : 0.14f));
                    g.drawVerticalLine ((int) x, (float) lanesTop, (float) lanesBottom);
                }
                // Close in, the beats between the bars too: to see the grid sit on the hits. (The bar's
                // own line may be off the left edge while its beats are not, so these come first.)
                if (const int bpb = beatsPerBar(); bpb > 1 && barPx / bpb >= 14.0)
                {
                    const double next = barAt (b + 1);
                    for (int k = 1; k < bpb; ++k)
                    {
                        const float bx = xOf (barAt (b) + (next - barAt (b)) * k / bpb);
                        if (bx < (float) area.getX() || bx > (float) area.getRight()) continue;
                        g.setColour (Colours::white.withAlpha (0.07f));
                        g.drawVerticalLine ((int) bx, (float) lanesTop, (float) lanesBottom);
                    }
                }
                if (! onScreen) continue;
                g.setColour (textDim.withAlpha (labelled ? 0.6f : 0.3f));
                g.drawVerticalLine ((int) x, (float) area.getY() + 22.0f, (float) area.getY() + kRuler);
                if (labelled)
                {
                    g.setColour (textDim.withAlpha (0.9f));
                    g.setFont (FontOptions (9.5f));
                    g.drawText (b == 0 ? String ("bar 1") : String (b + 1), (int) x + 3, area.getY() + 14, 44, 13, Justification::centredLeft);
                }
            }
        }
    }

    // Sections band: a block from each marker to the next; the stretch before the first marker is "Start".
    const Rectangle<int> band (area.getX(), area.getY() + kRuler, area.getWidth(), kSections);
    g.setColour (bgPanel);
    g.fillRect (band);
    const auto ms = markers();
    std::vector<int> order ((size_t) ms.size());
    for (int i = 0; i < (int) ms.size(); ++i) order[(size_t) i] = i;
    std::sort (order.begin(), order.end(), [&ms] (int a, int b) { return ms[(size_t) a].seconds < ms[(size_t) b].seconds; });
    const double length = lengthSeconds();
    g.setFont (FontOptions (11.0f, Font::bold));
    if (ms.empty() || ms[(size_t) order.front()].seconds > 0.05)
    {
        const double end = ms.empty() ? length : ms[(size_t) order.front()].seconds;
        const Rectangle<float> br (xOf (0.0), (float) band.getY() + 2.0f, (float) ((end - 0.0) * pxPerSecond), (float) kSections - 4.0f);
        g.setColour (bgRow);
        g.fillRoundedRectangle (br, 3.0f);
        g.setColour (textDim.withAlpha (0.7f));
        g.drawText (ms.empty() ? "Whole song  (double-click where a section should start)" : "Start",
                    br.reduced (6.0f, 0.0f).toNearestInt(), Justification::centredLeft, true);
    }
    for (size_t k = 0; k < order.size(); ++k)
    {
        const int idx = order[k];
        const auto& m = ms[(size_t) idx];
        const double end = k + 1 < order.size() ? ms[(size_t) order[k + 1]].seconds : jmax (length, m.seconds + 1.0);
        const float x0 = xOf (m.seconds), x1 = xOf (end);
        if (x1 < (float) area.getX() || x0 > (float) area.getRight()) continue;
        const Rectangle<float> br (x0 + 1.0f, (float) band.getY() + 2.0f, jmax (2.0f, x1 - x0 - 2.0f), (float) kSections - 4.0f);
        g.setColour (idx == selected ? accent.withAlpha (0.9f) : m.vamp ? vampCol : accentDim);
        g.fillRoundedRectangle (br, 3.0f);
        g.setColour (Colours::white);
        g.drawText (m.name + (m.vamp ? "  (vamp)" : String())
                      + (m.actions.empty() ? String() : "  \u2022 " + String ((int) m.actions.size()) + (m.actions.size() == 1 ? " action" : " actions")),
                    br.reduced (6.0f, 0.0f).toNearestInt(), Justification::centredLeft, true);
    }
    // Marker flags: a line down the lanes, a flag in the band.
    for (int i = 0; i < (int) ms.size(); ++i)
    {
        const float x = xOf (ms[(size_t) i].seconds);
        if (x < (float) area.getX() - 1.0f || x > (float) area.getRight() + 1.0f) continue;
        g.setColour (i == selected ? Colours::white : gold);
        g.fillRect (x - 1.0f, (float) band.getY(), 2.0f, (float) (lanesBottom - band.getY()));
        Path flag;
        flag.addTriangle (x, (float) band.getY(), x + 8.0f, (float) band.getY() + 5.0f, x, (float) band.getY() + 10.0f);
        g.fillPath (flag);
    }

    // The cue (where Play starts) and the playhead.
    const bool playing = isPlaying();
    if (const float cx = xOf (cue); cx >= (float) area.getX() && cx <= (float) area.getRight())
    {
        g.setColour (Colours::limegreen);
        Path tri;
        tri.addTriangle (cx - 6.0f, (float) area.getY(), cx + 6.0f, (float) area.getY(), cx, (float) area.getY() + 9.0f);
        g.fillPath (tri);
        if (! playing)
        {
            g.setColour (Colours::limegreen.withAlpha (0.6f));
            g.drawVerticalLine ((int) cx, (float) area.getY() + 9.0f, (float) lanesBottom);
        }
    }
    if (playing)
        if (const float px = xOf (engine.getTrackPosition()); px >= (float) area.getX() && px <= (float) area.getRight())
        {
            g.setColour (Colours::white);
            g.drawVerticalLine ((int) px, (float) area.getY(), (float) lanesBottom);
        }

    // The time under the mouse.
    if (hoverSeconds >= 0.0 && dragging < 0)
    {
        const float hx = xOf (hoverSeconds);
        g.setColour (Colours::white.withAlpha (0.25f));
        g.drawVerticalLine ((int) hx, (float) area.getY(), (float) lanesBottom);
        const String t = formatTime (hoverSeconds);
        g.setFont (FontOptions (10.5f, Font::bold));
        const int tw = 44;
        const int tx = hx + tw + 8 > (float) area.getRight() ? (int) hx - tw - 4 : (int) hx + 4;
        g.setColour (bgPanel.withAlpha (0.9f));
        g.fillRoundedRectangle ((float) tx - 2.0f, (float) area.getY() + 1.0f, (float) tw, (float) kRuler - 3.0f, 2.0f);
        g.setColour (Colours::white);
        g.drawText (t, tx, area.getY(), tw - 4, kRuler - 2, Justification::centred);
    }

    g.setColour (bgPanel.brighter (0.3f));
    g.drawVerticalLine (area.getX() - 1, 0.0f, (float) getHeight());
}

//==============================================================================
int TrackTimeline::markerAt (Point<int> p) const
{
    const auto area = timeArea();
    if (! area.contains (p) || p.y < area.getY() + kRuler) return -1;
    const auto ms = markers();
    int best = -1;
    float bestDist = (float) kGrab + 0.5f;
    for (int i = 0; i < (int) ms.size(); ++i)
    {
        const float d = std::abs (xOf (ms[(size_t) i].seconds) - (float) p.x);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

int TrackTimeline::sectionBlockAt (Point<int> p) const
{
    const auto area = timeArea();
    if (p.y < area.getY() + kRuler || p.y >= area.getY() + kRuler + kSections || p.x < area.getX()) return -1;
    const double t = secondsAt ((float) p.x);
    const auto ms = markers();
    int best = -1;
    for (int i = 0; i < (int) ms.size(); ++i)
        if (ms[(size_t) i].seconds <= t && (best < 0 || ms[(size_t) i].seconds > ms[(size_t) best].seconds))
            best = i;
    return best;
}

bool TrackTimeline::isPlaying() const
{
    return engine.getTrackSong() == songIndex && engine.isTrackRunning();
}

void TrackTimeline::setCue (double seconds)
{
    cue = jlimit (0.0, lengthSeconds(), seconds);
    if (engine.getTrackSong() == songIndex && ! engine.isTrackRunning())
        engine.cueTrack (cue);
    repaint();
}

void TrackTimeline::togglePlay()
{
    if (isPlaying()) { engine.stopTrack(); return; }
    if (engine.getTrackSong() != songIndex) engine.selectSong (songIndex);
    engine.cueTrack (cue);
    engine.startTrack();
}

void TrackTimeline::markHere()
{
    const double at = isPlaying() ? engine.getTrackPosition() : cue;
    auto m = markers();
    m.push_back ({ "Section " + String ((int) m.size() + 1), at, false });
    std::sort (m.begin(), m.end(), [] (const MarkerDef& a, const MarkerDef& b) { return a.seconds < b.seconds; });
    for (int i = 0; i < (int) m.size(); ++i) if (std::abs (m[(size_t) i].seconds - at) < 1e-9) selected = i;
    setMarkers (m, true);
    hint ("New section at " + formatTime (at) + ". Double-click it to name it; drag its flag to move it.");
}

void TrackTimeline::removeMarker (int index)
{
    auto m = markers();
    if (index < 0 || index >= (int) m.size()) return;
    m.erase (m.begin() + index);
    selected = -1;
    setMarkers (m, true);
}

void TrackTimeline::goToMarker (int index)
{
    const auto m = markers();
    if (index < 0 || index >= (int) m.size()) return;
    if (engine.getTrackSong() != songIndex || ! engine.isTrackRunning()) { setCue (m[(size_t) index].seconds); return; }
    for (int s = 0; s < engine.getTrackSectionCount(); ++s)
        if (std::abs (engine.getTrackSectionStart (s) - m[(size_t) index].seconds) < 0.001)
        {
            engine.queueTrackSection (s);
            hint ("Going to " + m[(size_t) index].name + " when this section ends");
            return;
        }
}

void TrackTimeline::renameMarker (int index)
{
    const auto m = markers();
    if (index < 0 || index >= (int) m.size()) return;
    auto* w = new AlertWindow ("Name this section", "Intro, Verse, Chorus, Solo: what the band calls it.", MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", m[(size_t) index].name, "Name");
    w->getTextEditor ("name")->setSelectAllWhenFocused (true);
    w->addButton ("Save", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    SafePointer<TrackTimeline> self (this);
    w->enterModalState (true, ModalCallbackFunction::create ([self, w, index] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0 || self == nullptr) return;
        auto mm = self->markers();
        if (index >= (int) mm.size()) return;
        const auto name = w->getTextEditorContents ("name").trim();
        mm[(size_t) index].name = name.isNotEmpty() ? name : "Section " + String (index + 1);
        self->setMarkers (mm, true);
    }));
}

void TrackTimeline::showMarkerMenu (int index, Point<int> at)
{
    const auto m = markers();
    if (index < 0 || index >= (int) m.size()) return;
    selected = index;
    repaint();
    PopupMenu menu;
    menu.addItem (1, "Rename...");
    menu.addItem (2, "Vamp: repeat until released", true, m[(size_t) index].vamp);
    menu.addItem (3, isPlaying() ? "Go here when this section ends" : "Start Play here", true);
    menu.addItem (6, m[(size_t) index].actions.empty() ? "Actions when the track enters it..."
                                                         : String ((int) m[(size_t) index].actions.size()) + (m[(size_t) index].actions.size() == 1 ? " action" : " actions") + " when the track enters it...");
    menu.addItem (5, "Start the song here  (Play from the top begins at this section)", true,
                  song() != nullptr && std::abs (song()->startSeconds - m[(size_t) index].seconds) < 1e-6);
    menu.addSeparator();
    menu.addItem (4, "Remove section (the music is untouched)");
    SafePointer<TrackTimeline> self (this);
    menu.showMenuAsync (PopupMenu::Options().withTargetScreenArea (Rectangle<int> (localPointToGlobal (at), localPointToGlobal (at)).expanded (1)),
                        [self, index] (int choice)
    {
        if (self == nullptr || choice == 0) return;
        switch (choice)
        {
            case 1: self->renameMarker (index); break;
            case 2:
            {
                auto mm = self->markers();
                if (index < (int) mm.size()) { mm[(size_t) index].vamp = ! mm[(size_t) index].vamp; self->setMarkers (mm, true); }
                break;
            }
            case 3: self->goToMarker (index); break;
            case 4: self->removeMarker (index); break;
            case 6: if (self->onEditActions) self->onEditActions (index); break;
            case 5:
            {
                const auto mm = self->markers();
                if (index < (int) mm.size())
                {
                    self->engine.setSongStart (self->songIndex, mm[(size_t) index].seconds);
                    self->hint ("Play from the top now begins at " + mm[(size_t) index].name);
                    if (self->onMarkersChanged) self->onMarkersChanged();
                }
                break;
            }
            default: break;
        }
    });
}

void TrackTimeline::showTimelineMenu (double at, Point<int> where)
{
    auto* s = song();
    if (s == nullptr) return;
    PopupMenu menu;
    menu.addItem (1, "Start the song here  (Play from the top begins at " + formatTime (at) + ")");
    menu.addItem (2, "Start at the first sound  (the usual: silence at the head of the files is skipped)", s->startSeconds > 0.0, s->startSeconds <= 0.0);
    menu.addSeparator();
    menu.addItem (3, "Start a section here...");
    SafePointer<TrackTimeline> self (this);
    menu.showMenuAsync (PopupMenu::Options().withTargetScreenArea (Rectangle<int> (localPointToGlobal (where), localPointToGlobal (where)).expanded (1)),
                        [self, at] (int choice)
    {
        if (self == nullptr || choice == 0) return;
        if (choice == 1)      { self->engine.setSongStart (self->songIndex, at); self->hint ("Play from the top now begins at " + formatTime (at)); }
        else if (choice == 2) { self->engine.setSongStart (self->songIndex, 0.0); self->hint ("Play from the top begins at the first sound"); }
        else if (choice == 3)
        {
            auto m = self->markers();
            m.push_back ({ "Section " + String ((int) m.size() + 1), at, false });
            std::sort (m.begin(), m.end(), [] (const MarkerDef& a, const MarkerDef& b) { return a.seconds < b.seconds; });
            for (int i = 0; i < (int) m.size(); ++i) if (std::abs (m[(size_t) i].seconds - at) < 1e-9) self->selected = i;
            self->setMarkers (m, true);
            self->renameMarker (self->selected);
        }
        if (self->onMarkersChanged) self->onMarkersChanged();
    });
}

void TrackTimeline::hint (const String& text)
{
    if (onHint) onHint (text);
}

//==============================================================================
void TrackTimeline::mouseDown (const MouseEvent& e)
{
    grabKeyboardFocus();
    const auto p = e.getPosition();
    if (! timeArea().contains (p)) return;

    if (e.mods.isPopupMenu())
    {
        int idx = markerAt (p);
        if (idx < 0) idx = sectionBlockAt (p);
        if (idx >= 0) showMarkerMenu (idx, p);
        else showTimelineMenu (snapped (secondsAt ((float) p.x), e.mods.isAltDown()), p);
        return;
    }

    if (const int idx = markerAt (p); idx >= 0)
    {
        dragging = idx;
        selected = idx;
        dragMoved = false;
        dragStartSeconds = markers()[(size_t) idx].seconds;
        repaint();
        return;
    }

    if (const int b = sectionBlockAt (p); b >= 0)
    {
        selected = b;
        if (isPlaying()) goToMarker (b);
        else setCue (markers()[(size_t) b].seconds);
        repaint();
        return;
    }

    selected = -1;
    setCue (snapped (secondsAt ((float) p.x), e.mods.isAltDown()));
}

void TrackTimeline::mouseDrag (const MouseEvent& e)
{
    const float x = (float) jlimit (timeArea().getX(), timeArea().getRight(), e.x);
    if (dragging >= 0)
    {
        auto m = markers();
        if (dragging >= (int) m.size()) { dragging = -1; return; }
        const double s = snapped (secondsAt (x), e.mods.isAltDown());
        if (std::abs (s - m[(size_t) dragging].seconds) < 1e-9) return;
        m[(size_t) dragging].seconds = s;
        dragMoved = true;
        setMarkers (m, false);
        hint (m[(size_t) dragging].name + " starts at " + formatTime (s) + (barsKnown() ? "  (bar " + String (barOf (s) + 1.0, 2) + "; hold Alt to drag off the bars)" : String()));
        return;
    }
    if (markerAt (e.getMouseDownPosition()) < 0 && sectionBlockAt (e.getMouseDownPosition()) < 0 && ! isPlaying())
        setCue (snapped (secondsAt (x), e.mods.isAltDown()));
}

void TrackTimeline::mouseUp (const MouseEvent&)
{
    if (dragging >= 0)
    {
        if (dragMoved)
        {
            auto m = markers();
            const double at = dragging < (int) m.size() ? m[(size_t) dragging].seconds : -1.0;
            std::sort (m.begin(), m.end(), [] (const MarkerDef& a, const MarkerDef& b) { return a.seconds < b.seconds; });
            selected = -1;
            for (int i = 0; i < (int) m.size(); ++i) if (std::abs (m[(size_t) i].seconds - at) < 1e-9) selected = i;
            setMarkers (m, true);
        }
        dragging = -1;
    }
}

void TrackTimeline::mouseDoubleClick (const MouseEvent& e)
{
    const auto p = e.getPosition();
    if (! timeArea().contains (p) || e.mods.isPopupMenu()) return;
    int idx = markerAt (p);
    if (idx < 0) idx = sectionBlockAt (p);
    if (idx >= 0) { renameMarker (idx); return; }

    const double at = snapped (secondsAt ((float) p.x), e.mods.isAltDown());
    auto m = markers();
    m.push_back ({ "Section " + String ((int) m.size() + 1), at, false });
    std::sort (m.begin(), m.end(), [] (const MarkerDef& a, const MarkerDef& b) { return a.seconds < b.seconds; });
    for (int i = 0; i < (int) m.size(); ++i) if (std::abs (m[(size_t) i].seconds - at) < 1e-9) selected = i;
    setMarkers (m, true);
    renameMarker (selected);
}

void TrackTimeline::mouseMove (const MouseEvent& e)
{
    const auto p = e.getPosition();
    if (! timeArea().contains (p)) { mouseExit (e); return; }
    hoverSeconds = jlimit (0.0, lengthSeconds(), secondsAt ((float) p.x));
    const int mk = markerAt (p);
    setMouseCursor (mk >= 0 ? MouseCursor::LeftRightResizeCursor : MouseCursor::NormalCursor);
    const auto ms = markers();
    if (mk >= 0)
        hint (ms[(size_t) mk].name + " starts at " + formatTime (ms[(size_t) mk].seconds)
              + ".  Drag the flag to move it; double-click to rename; right-click for vamp, go and remove.");
    else if (const int b = sectionBlockAt (p); b >= 0)
        hint (ms[(size_t) b].name + (ms[(size_t) b].vamp ? " (vamp)" : "")
              + ".  Click to play from its start; double-click to rename; right-click for vamp, go and remove.");
    else
        hint (formatTime (hoverSeconds) + ".  Click to play from here (Space or Play); double-click to start a section here; right-click to start the song here."
              + (barsKnown() ? String ("  Snaps to bars; hold Alt for free.") : String()));
    repaint();
}

void TrackTimeline::mouseExit (const MouseEvent&)
{
    if (hoverSeconds >= 0.0) { hoverSeconds = -1.0; hint ({}); repaint(); }
}

void TrackTimeline::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        hoverSeconds = jlimit (0.0, lengthSeconds(), secondsAt ((float) e.x));
        zoomBy (w.deltaY > 0 ? 1.25f : 0.8f);
        return;
    }
    const float d = w.deltaX != 0.0f ? w.deltaX : w.deltaY;
    scrollSeconds -= d * visibleSeconds() * 0.5;
    clampScroll();
    updateScrollBar();
    repaint();
}

bool TrackTimeline::keyPressed (const KeyPress& k)
{
    if (k.getKeyCode() == KeyPress::spaceKey) { togglePlay(); return true; }
    if (k.getTextCharacter() == 'm' || k.getTextCharacter() == 'M') { markHere(); return true; }
    if (k.getKeyCode() == KeyPress::deleteKey || k.getKeyCode() == KeyPress::backspaceKey)
    {
        if (selected >= 0) { removeMarker (selected); return true; }
        return false;
    }
    if (k.getKeyCode() == KeyPress::leftKey || k.getKeyCode() == KeyPress::rightKey)
    {
        if (selected < 0) return false;
        auto m = markers();
        if (selected >= (int) m.size()) return false;
        const double step = (k.getModifiers().isShiftDown() ? 1.0 : 0.1) * (k.getKeyCode() == KeyPress::leftKey ? -1.0 : 1.0);
        m[(size_t) selected].seconds = jlimit (0.0, lengthSeconds(), m[(size_t) selected].seconds + step);
        setMarkers (m, true);
        hint (m[(size_t) selected].name + " starts at " + formatTime (m[(size_t) selected].seconds));
        return true;
    }
    if (k.getKeyCode() == KeyPress::escapeKey && selected >= 0) { selected = -1; repaint(); return true; }
    return false;
}

//==============================================================================
void TrackTimeline::followPlayhead()
{
    const double p = engine.getTrackPosition();
    const auto area = timeArea();
    const float x = xOf (p);
    if (x > (float) area.getRight() - 16.0f || x < (float) area.getX())
    {
        scrollSeconds = p - visibleSeconds() * 0.1;
        clampScroll();
        updateScrollBar();
    }
}

void TrackTimeline::timerCallback()
{
    const bool playing = isPlaying();
    if (playing) { followPlayhead(); repaint(); }
    else if (wasPlaying) repaint();
    else if (engine.getTrackSong() == songIndex && dragging < 0)
    {
        // Stopped: the cue follows the player's -- the song's start after a stop, a section asked for from the panel.
        const double c = engine.getTrackCue();
        if (std::abs (c - cue) > 1e-6) { cue = c; repaint(); }
    }
    wasPlaying = playing;
}

} // namespace perf
