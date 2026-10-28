#include "ChartViewer.h"
#include "Charts.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <memory>
#include <vector>

namespace perf
{

using namespace juce;

namespace
{
    const Colour bgPanel   { 0xff1b1d22 };
    const Colour bgRow     { 0xff23262d };
    const Colour accent    { 0xff4f9dff };
    const Colour textDim   { 0xff9aa0ab };
    const Colour paper     { 0xfff4f1e8 };
    const Colour ink       { 0xff1a1a1a };
    const Colour chordInk  { 0xff1b56a8 };

    Font monoFont (float size, bool bold = false)
    {
        return Font (FontOptions (Font::getDefaultMonospacedFontName(), size, bold ? Font::bold : Font::plain));
    }
}

//==============================================================================
// ChartTextView
//==============================================================================
void ChartTextView::setText (const String& text, bool chordPro)
{
    lines.clear();
    for (auto raw : StringArray::fromLines (text))
    {
        const String line = raw.trimEnd();
        if (chordPro)
        {
            if (line.startsWith ("{") && line.endsWith ("}"))
            {
                const String inner = line.substring (1, line.length() - 1).trim();
                const String key = inner.upToFirstOccurrenceOf (":", false, false).trim().toLowerCase();
                const String value = inner.containsChar (':') ? inner.fromFirstOccurrenceOf (":", false, false).trim() : String();
                if (key == "title" || key == "t")                                             lines.push_back ({ Line::title, value });
                else if (key == "subtitle" || key == "st" || key == "artist")                 lines.push_back ({ Line::subtitle, value });
                else if (key == "comment" || key == "c" || key == "ci" || key == "cb")        lines.push_back ({ Line::comment, "(" + value + ")" });
                continue;                                                                      // other directives are not shown
            }
            if (line.containsChar ('['))
            {
                String chords, words;
                int pos = 0;
                while (pos < line.length())
                {
                    const int open = line.indexOfChar (pos, '[');
                    if (open < 0) { words += line.substring (pos); break; }
                    const int close = line.indexOfChar (open, ']');
                    if (close < 0) { words += line.substring (pos); break; }
                    words += line.substring (pos, open);
                    while (chords.length() < words.length()) chords += " ";
                    chords += line.substring (open + 1, close) + " ";
                    pos = close + 1;
                }
                lines.push_back ({ Line::chords, chords.trimEnd() });
                lines.push_back ({ Line::words, words });
                continue;
            }
        }
        lines.push_back ({ Line::plain, line });
    }
    repaint();
}

void ChartTextView::setScale (float s)
{
    scale = jlimit (0.5f, 3.0f, s);
    repaint();
}

float ChartTextView::lineHeight() const      { return 15.0f * scale * 1.4f; }

int ChartTextView::preferredHeight() const
{
    float h = 24.0f;
    for (auto& l : lines) h += l.kind == Line::title ? lineHeight() * 1.6f : lineHeight();
    return (int) std::ceil (h) + 24;
}

int ChartTextView::preferredWidth() const
{
    int chars = 20;
    for (auto& l : lines) chars = jmax (chars, l.text.length());
    return (int) (chars * 15.0f * scale * 0.62f) + 48;
}

void ChartTextView::paint (Graphics& g)
{
    g.fillAll (paper);
    float y = 20.0f;
    const float lh = lineHeight();
    for (auto& l : lines)
    {
        switch (l.kind)
        {
            case Line::title:
                g.setColour (ink);
                g.setFont (Font (FontOptions (22.0f * scale, Font::bold)));
                g.drawText (l.text, 24, (int) y, getWidth() - 48, (int) (lh * 1.6f), Justification::centredLeft);
                y += lh * 1.6f;
                break;
            case Line::subtitle:
                g.setColour (ink.withAlpha (0.7f));
                g.setFont (Font (FontOptions (15.0f * scale, Font::italic)));
                g.drawText (l.text, 24, (int) y, getWidth() - 48, (int) lh, Justification::centredLeft);
                y += lh;
                break;
            case Line::comment:
                g.setColour (ink.withAlpha (0.6f));
                g.setFont (Font (FontOptions (14.0f * scale, Font::italic)));
                g.drawText (l.text, 24, (int) y, getWidth() - 48, (int) lh, Justification::centredLeft);
                y += lh;
                break;
            case Line::chords:
                g.setColour (chordInk);
                g.setFont (monoFont (15.0f * scale, true));
                g.drawSingleLineText (l.text, 24, (int) (y + lh * 0.75f));
                y += lh;
                break;
            case Line::words:
            case Line::plain:
                g.setColour (ink);
                g.setFont (monoFont (15.0f * scale));
                g.drawSingleLineText (l.text, 24, (int) (y + lh * 0.75f));
                y += lh;
                break;
        }
    }
}

//==============================================================================
namespace
{
    /** An image shown at a zoom, for a scan or a PDF's page. */
    class ImageView : public Component
    {
    public:
        void setImage (Image im)  { image = std::move (im); repaint(); }
        void paint (Graphics& g) override
        {
            g.fillAll (bgPanel);
            if (image.isValid())
                g.drawImage (image, getLocalBounds().toFloat(), RectanglePlacement::centred | RectanglePlacement::onlyReduceInSize);
        }
        Image image;
    };

    /** One chart: its viewer, and for a typed chart its editor beside it. */
    class ChartPane : public Component,
                      private Timer,
                      private TextEditor::Listener
    {
    public:
        ChartPane (Engine& e, int song, int chartIndex)
            : engine (e), songIndex (song), index (chartIndex)
        {
            const auto& songs = engine.getSongs();
            if (songIndex < (int) songs.size() && index < (int) songs[(size_t) songIndex].charts.size())
                file = engine.getChartsFolder().getChildFile (songs[(size_t) songIndex].charts[(size_t) index].file);
            kind = chartKindFor (file);

            addAndMakeVisible (openBtn);
            openBtn.setTooltip ("Open the file in whatever the computer uses for it");
            openBtn.onClick = [this] { file.startAsProcess(); };
            addAndMakeVisible (status);
            status.setColour (Label::textColourId, textDim);
            status.setFont (FontOptions (12.0f));
            status.setMinimumHorizontalScale (0.8f);

            if (kind == "pdf" || kind == "image")
            {
                for (auto* b : { &prevBtn, &nextBtn, &zoomOut, &zoomIn, &fitBtn }) addAndMakeVisible (b);
                prevBtn.onClick = [this] { showPage (pageNo - 1); };
                nextBtn.onClick = [this] { showPage (pageNo + 1); };
                zoomOut.onClick = [this] { setZoom (zoom / 1.25f); };
                zoomIn.onClick  = [this] { setZoom (zoom * 1.25f); };
                fitBtn.onClick  = [this] { setZoom (0.0f); };
                prevBtn.setVisible (kind == "pdf");
                nextBtn.setVisible (kind == "pdf");
                addAndMakeVisible (viewport);
                viewport.setViewedComponent (&imageView, false);
                viewport.setScrollBarsShown (true, true);
                if (kind == "pdf")
                {
                    pages = chartPages (file);
                    if (pages.isEmpty())
                    {
                        String err;
                        rendering = true;
                        prerenderPdfPagesAsync (file);
                        status.setText ("Turning the PDF into pages...", dontSendNotification);
                        startTimer (500);
                    }
                    showPage (1);
                }
                else
                {
                    pageImage = ImageFileFormat::loadFrom (file);
                    if (! pageImage.isValid()) status.setText ("The image could not be read.", dontSendNotification);
                    layoutImage();
                }
            }
            else
            {
                /* A typed chart: the file on the left, as you would type it, and
                   on the right what the stand will show. Saving writes the file
                   the tablet serves, so the two never disagree. */
                for (auto* b : { &saveBtn, &zoomOut, &zoomIn }) addAndMakeVisible (b);
                saveBtn.onClick = [this] { save(); };
                zoomOut.onClick = [this] { textView.setScale (textView.getScale() / 1.15f); layoutText(); };
                zoomIn.onClick  = [this] { textView.setScale (textView.getScale() * 1.15f); layoutText(); };
                addAndMakeVisible (editor);
                editor.setMultiLine (true, false);
                editor.setReturnKeyStartsNewLine (true);
                editor.setTabKeyUsedAsCharacter (true);
                editor.setFont (monoFont (14.0f));
                editor.setColour (TextEditor::backgroundColourId, bgRow);
                editor.setColour (TextEditor::textColourId, Colours::white);
                editor.setText (file.loadFileAsString(), false);
                editor.addListener (this);
                addAndMakeVisible (viewport);
                viewport.setViewedComponent (&textView, false);
                viewport.setScrollBarsShown (true, true);
                textView.setText (editor.getText(), kind == "chordpro");
                status.setText (kind == "chordpro" ? "Chords go in square brackets before the syllable they fall on: [C]Some[G]where. "
                                                     "{title: ...}, {subtitle: ...} and {comment: ...} are shown; other directives are not."
                                                   : "Plain text, shown as typed in a fixed-width face. Ctrl+S saves.", dontSendNotification);
                layoutText();
            }
            setWantsKeyboardFocus (true);
        }

        ~ChartPane() override { stopTimer(); }

        bool isDirty() const  { return dirty; }
        File getFile() const  { return file; }

        bool save()
        {
            if (kind != "text" && kind != "chordpro") return true;
            if (! file.replaceWithText (editor.getText()))
            {
                status.setText ("Could not write " + file.getFileName(), dontSendNotification);
                return false;
            }
            dirty = false;
            saveBtn.setEnabled (false);
            status.setText ("Saved. The tablet shows this version from now on.", dontSendNotification);
            return true;
        }

        void resized() override
        {
            auto r = getLocalBounds();
            auto bar = r.removeFromTop (34).reduced (8, 4);
            if (kind == "pdf" || kind == "image")
            {
                if (kind == "pdf")
                {
                    prevBtn.setBounds (bar.removeFromLeft (60));   bar.removeFromLeft (4);
                    nextBtn.setBounds (bar.removeFromLeft (60));   bar.removeFromLeft (12);
                }
                zoomOut.setBounds (bar.removeFromLeft (30));       bar.removeFromLeft (4);
                zoomIn.setBounds (bar.removeFromLeft (30));        bar.removeFromLeft (4);
                fitBtn.setBounds (bar.removeFromLeft (40));        bar.removeFromLeft (12);
            }
            else
            {
                saveBtn.setBounds (bar.removeFromLeft (70));       bar.removeFromLeft (12);
                zoomOut.setBounds (bar.removeFromLeft (30));       bar.removeFromLeft (4);
                zoomIn.setBounds (bar.removeFromLeft (30));        bar.removeFromLeft (12);
            }
            openBtn.setBounds (bar.removeFromRight (190));
            bar.removeFromRight (8);
            status.setBounds (bar);
            r.removeFromTop (2);
            if (kind == "text" || kind == "chordpro")
            {
                editor.setBounds (r.removeFromLeft (r.getWidth() / 2).reduced (4, 0));
                viewport.setBounds (r.reduced (4, 0));
                layoutText();
            }
            else
            {
                viewport.setBounds (r);
                layoutImage();
            }
        }

        bool keyPressed (const KeyPress& k) override
        {
            if (k == KeyPress ('s', ModifierKeys::commandModifier, 0)) { save(); return true; }
            if (kind == "pdf")
            {
                if (k == KeyPress::rightKey || k == KeyPress::pageDownKey || k == KeyPress::spaceKey) { showPage (pageNo + 1); return true; }
                if (k == KeyPress::leftKey || k == KeyPress::pageUpKey) { showPage (pageNo - 1); return true; }
            }
            return false;
        }

        void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
        {
            if (! (e.mods.isCtrlDown() || e.mods.isCommandDown())) { Component::mouseWheelMove (e, w); return; }
            if (kind == "pdf" || kind == "image") setZoom ((zoom <= 0.0f ? fitZoom() : zoom) * (w.deltaY > 0 ? 1.1f : 0.9f));
            else { textView.setScale (textView.getScale() * (w.deltaY > 0 ? 1.1f : 0.9f)); layoutText(); }
        }

    private:
        void textEditorTextChanged (TextEditor&) override
        {
            dirty = true;
            saveBtn.setEnabled (true);
            textView.setText (editor.getText(), kind == "chordpro");
            layoutText();
        }

        void timerCallback() override
        {
            if (! rendering) { stopTimer(); return; }
            pages = chartPages (file);
            if (pages.isEmpty()) return;
            rendering = false;
            stopTimer();
            showPage (1);
        }

        void showPage (int n)
        {
            if (kind != "pdf") return;
            if (pages.isEmpty())
            {
                if (! rendering)
                    status.setText ("The PDF's pages could not be made (is poppler-utils installed?). Open it with the button on the right.", dontSendNotification);
                prevBtn.setEnabled (false);
                nextBtn.setEnabled (false);
                return;
            }
            pageNo = jlimit (1, pages.size(), n);
            pageImage = ImageFileFormat::loadFrom (pages[pageNo - 1]);
            status.setText ("Page " + String (pageNo) + " of " + String (pages.size()) + "   (arrow keys turn pages; Ctrl+wheel zooms)", dontSendNotification);
            prevBtn.setEnabled (pageNo > 1);
            nextBtn.setEnabled (pageNo < pages.size());
            layoutImage();
            viewport.setViewPosition (0, 0);
        }

        float fitZoom() const
        {
            if (! pageImage.isValid() || viewport.getWidth() <= 0) return 1.0f;
            return (float) (viewport.getWidth() - 2) / (float) pageImage.getWidth();
        }

        void setZoom (float z)
        {
            zoom = z <= 0.0f ? 0.0f : jlimit (0.1f, 8.0f, z);
            layoutImage();
        }

        void layoutImage()
        {
            if (! pageImage.isValid()) { imageView.setImage ({}); imageView.setSize (viewport.getWidth(), viewport.getHeight()); return; }
            const float z = zoom <= 0.0f ? fitZoom() : zoom;
            imageView.setImage (pageImage);
            imageView.setSize (jmax (1, (int) (pageImage.getWidth() * z)), jmax (1, (int) (pageImage.getHeight() * z)));
        }

        void layoutText()
        {
            textView.setSize (jmax (viewport.getWidth() - 2, textView.preferredWidth()), jmax (viewport.getHeight() - 2, textView.preferredHeight()));
        }

        Engine& engine;
        const int songIndex, index;
        File file;
        String kind;
        Array<File> pages;
        int pageNo = 1;
        bool rendering = false, dirty = false;
        Image pageImage;
        float zoom = 0.0f;                        // 0 = fit to width
        Viewport viewport;
        ImageView imageView;
        ChartTextView textView;
        TextEditor editor;
        Label status;
        TextButton prevBtn { "< Page" }, nextBtn { "Page >" }, zoomOut { "-" }, zoomIn { "+" }, fitBtn { "Fit" },
                   saveBtn { "Save" }, openBtn { "Open in the system app..." };
    };

    /** The window: a tab per chart of the song. */
    class ChartWindow : public DocumentWindow
    {
    public:
        ChartWindow (Engine& e, int song)
            : DocumentWindow ("Charts: " + e.getSongs()[(size_t) song].name, bgPanel, DocumentWindow::allButtons),
              engine (e), songIndex (song)
        {
            tabs.setTabBarDepth (30);
            tabs.setColour (TabbedComponent::backgroundColourId, bgPanel);
            tabs.setColour (TabbedComponent::outlineColourId, bgPanel);
            rebuildTabs();
            setContentNonOwned (&tabs, false);
            setUsingNativeTitleBar (true);
            setResizable (true, true);
            setResizeLimits (640, 420, 10000, 10000);
            centreWithSize (jmin (1100, getParentMonitorArea().getWidth() - 80), jmin (900, getParentMonitorArea().getHeight() - 80));
            setVisible (true);
        }

        void rebuildTabs()
        {
            tabs.clearTabs();
            panes.clear();
            const auto& songs = engine.getSongs();
            if (songIndex >= (int) songs.size()) return;
            const auto& sg = songs[(size_t) songIndex];
            for (int i = 0; i < (int) sg.charts.size(); ++i)
            {
                auto pane = std::make_unique<ChartPane> (engine, songIndex, i);
                tabs.addTab (sg.charts[(size_t) i].displayName(), bgPanel, pane.get(), false);
                panes.push_back (std::move (pane));
            }
            tabs.setTabBarDepth (panes.size() > 1 ? 30 : 0);
        }

        void selectChart (int i)
        {
            if (i >= 0 && i < tabs.getNumTabs()) tabs.setCurrentTabIndex (i);
            toFront (true);
        }

        int getSongIndex() const  { return songIndex; }

        void closeButtonPressed() override
        {
            std::vector<ChartPane*> unsaved;
            for (auto& p : panes) if (p->isDirty()) unsaved.push_back (p.get());
            if (unsaved.empty()) { close(); return; }
            SafePointer<ChartWindow> self (this);
            AlertWindow::showYesNoCancelBox (MessageBoxIconType::QuestionIcon, "Save changes?",
                                             "Save your edits to " + unsaved.front()->getFile().getFileName() + "?",
                                             "Save", "Discard", "Cancel", this,
                                             ModalCallbackFunction::create ([self, unsaved] (int r)
                                             {
                                                 if (self == nullptr) return;
                                                 if (r == 1) for (auto* p : unsaved) if (! p->save()) return;
                                                 if (r == 1 || r == 2) self->close();
                                             }));
        }

    private:
        void close()
        {
            setVisible (false);
            MessageManager::callAsync ([w = this] { delete w; });
        }

        Engine& engine;
        const int songIndex;
        TabbedComponent tabs { TabbedButtonBar::TabsAtTop };
        std::vector<std::unique_ptr<ChartPane>> panes;
    };

    std::vector<Component::SafePointer<ChartWindow>> openWindows;
}

void showChartViewer (Engine& engine, int songIndex, int chartIndex)
{
    const auto& songs = engine.getSongs();
    if (songIndex < 0 || songIndex >= (int) songs.size() || songs[(size_t) songIndex].charts.empty()) return;

    openWindows.erase (std::remove_if (openWindows.begin(), openWindows.end(), [] (auto& w) { return w == nullptr; }), openWindows.end());
    for (auto& w : openWindows)
        if (w != nullptr && w->getSongIndex() == songIndex)
        {
            w->rebuildTabs();
            w->selectChart (chartIndex);
            return;
        }
    auto* w = new ChartWindow (engine, songIndex);      // deletes itself when closed
    w->selectChart (chartIndex);
    openWindows.push_back (w);
}

} // namespace perf
