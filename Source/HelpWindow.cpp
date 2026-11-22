#include "HelpWindow.h"
#include "Theme.h"
#include "BinaryData.h"

using namespace juce;

namespace perf
{

namespace
{
    using namespace perf::theme;
    const Colour bgCode    { 0xff000712 };
}

//==============================================================================
/** One laid-out piece of the manual. The renderer is deliberately flat: a list
    of blocks, each of which knows its own height, rather than a tree. Our
    manual has no nesting deeper than a list inside a section. */
struct Block
{
    enum class Kind { heading1, heading2, heading3, paragraph, bullet, numbered, code, rule, image, tableRow };

    Kind kind = Kind::paragraph;
    String text;            // already stripped of its markers
    String marker;          // "1." for numbered items
    int height = 0;
    Image image;
    String sectionKey;      // the heading this block sits under
    StringArray cells;      // tableRow only
    bool header = false;    // tableRow only: the row that names the columns
    Array<Array<Range<int>>> cellBold, cellMono, cellItalic;

    // Ranges within `text` that are bold, inline code or italic, as (start, length).
    Array<Range<int>> bold, mono, italic;
};

//==============================================================================
/* Parses just the Markdown our manual uses. Anything unrecognised is shown as
   plain text rather than silently dropped: a manual that quietly loses a
   paragraph is worse than one that renders it plainly. */
class ManualParser
{
public:
    static Array<Block> parse (const String& markdown, const File& imageDir)
    {
        Array<Block> blocks;
        auto lines = StringArray::fromLines (markdown);

        String section;     // the current heading, stamped onto every block
        bool inCode = false;
        String codeAccum;
        String paraAccum;

        auto flushParagraph = [&blocks, &paraAccum]
        {
            if (paraAccum.isEmpty()) return;
            blocks.add (makeInline (Block::Kind::paragraph, paraAccum.trim()));
            paraAccum.clear();
        };

        for (int i = 0; i < lines.size(); ++i)
        {
            const auto raw = lines[i];
            const auto line = raw.trimEnd();

            if (line.startsWith ("```"))
            {
                if (inCode)
                {
                    Block b; b.kind = Block::Kind::code; b.text = codeAccum.trimEnd();
                    blocks.add (b);
                    codeAccum.clear();
                }
                else flushParagraph();

                inCode = ! inCode;
                continue;
            }

            if (inCode) { codeAccum << line << "\n"; continue; }

            if (line.isEmpty()) { flushParagraph(); continue; }

            // Images: ![alt](path)
            if (line.startsWith ("!["))
            {
                flushParagraph();
                const auto path = line.fromFirstOccurrenceOf ("](", false, false)
                                      .upToLastOccurrenceOf (")", false, false);
                Block b; b.kind = Block::Kind::image;
                b.text = line.fromFirstOccurrenceOf ("![", false, false).upToFirstOccurrenceOf ("]", false, false);
                const auto f = imageDir.getChildFile (path);
                if (f.existsAsFile()) b.image = ImageFileFormat::loadFrom (f);
                blocks.add (b);
                continue;
            }

            if (line.startsWith ("### ")) { flushParagraph(); blocks.add (makeInline (Block::Kind::heading3, line.substring (4))); continue; }
            if (line.startsWith ("## "))  { flushParagraph(); blocks.add (makeInline (Block::Kind::heading2, line.substring (3))); continue; }
            if (line.startsWith ("# "))   { flushParagraph(); blocks.add (makeInline (Block::Kind::heading1, line.substring (2))); continue; }

            if (line.startsWith ("---") && line.trim().containsOnly ("-"))
            {
                flushParagraph();
                Block b; b.kind = Block::Kind::rule; blocks.add (b);
                continue;
            }

            const auto trimmed = line.trimStart();

            if (trimmed.startsWith ("- ") || trimmed.startsWith ("* "))
            {
                flushParagraph();
                auto item = trimmed.substring (2);
                // A list item can wrap onto following indented lines.
                while (i + 1 < lines.size())
                {
                    const auto next = lines[i + 1];
                    if (next.trim().isEmpty()) break;
                    const auto nt = next.trimStart();
                    if (nt.startsWith ("- ") || nt.startsWith ("* ") || nt.startsWith ("#")
                        || nt.startsWith ("```") || isNumbered (nt)) break;
                    if (! next.startsWith (" ")) break;
                    item << " " << nt;
                    ++i;
                }
                blocks.add (makeInline (Block::Kind::bullet, item));
                continue;
            }

            if (isNumbered (trimmed))
            {
                flushParagraph();
                auto item = trimmed.fromFirstOccurrenceOf (" ", false, false);
                const auto marker = trimmed.upToFirstOccurrenceOf (" ", false, false);

                // Continuation lines are indented under the number; without this
                // the rest of the step escapes to the left margin as a paragraph.
                while (i + 1 < lines.size())
                {
                    const auto next = lines[i + 1];
                    if (next.trim().isEmpty()) break;
                    if (! next.startsWith (" ")) break;
                    const auto nt = next.trimStart();
                    if (nt.startsWith ("- ") || nt.startsWith ("* ") || nt.startsWith ("#")
                        || nt.startsWith ("```") || isNumbered (nt)) break;
                    item << " " << nt;
                    ++i;
                }

                Block b = makeInline (Block::Kind::numbered, item);
                b.marker = marker;
                blocks.add (b);
                continue;
            }

            // Tables: "| a | b |" with a "|---|---|" separator we skip.
            if (line.startsWith ("|"))
            {
                flushParagraph();
                auto body = line.trim();
                if (body.startsWith ("|")) body = body.substring (1);
                if (body.endsWith ("|"))   body = body.dropLastCharacters (1);

                // A separator row carries no content, but it says the row
                // above it names the columns.
                if (body.removeCharacters ("-: |").isEmpty())
                {
                    if (! blocks.isEmpty() && blocks.getReference (blocks.size() - 1).kind == Block::Kind::tableRow)
                        blocks.getReference (blocks.size() - 1).header = true;
                    continue;
                }

                Block b; b.kind = Block::Kind::tableRow;
                for (auto& cell : StringArray::fromTokens (body, "|", ""))
                {
                    const auto parsed = makeInline (Block::Kind::paragraph, cell.trim());
                    b.cells.add (parsed.text);
                    b.cellBold.add (parsed.bold);
                    b.cellMono.add (parsed.mono);
                    b.cellItalic.add (parsed.italic);
                }
                blocks.add (b);
                continue;
            }

            paraAccum << (paraAccum.isEmpty() ? "" : " ") << trimmed;
        }

        flushParagraph();

        /* Stamp every block with the heading it sits under, in one pass at the
           end rather than threading it through every branch above. Search uses
           this to show whole sections. */
        String current;
        for (auto& b : blocks)
        {
            if (b.kind == Block::Kind::heading1 || b.kind == Block::Kind::heading2
                || b.kind == Block::Kind::heading3)
                current = b.text;
            b.sectionKey = current;
        }

        return blocks;
    }

private:
    static bool isNumbered (const String& s)
    {
        int i = 0;
        while (i < s.length() && CharacterFunctions::isDigit (s[i])) ++i;
        return i > 0 && i < s.length() - 1 && s[i] == '.' && s[i + 1] == ' ';
    }

    /** Pulls **bold** and `code` out of the text, recording where they were so
        the painter can switch fonts mid-line. */
    static Block makeInline (Block::Kind kind, const String& src)
    {
        Block b; b.kind = kind;
        String out;

        for (int i = 0; i < src.length(); )
        {
            if (src[i] == '*' && i + 1 < src.length() && src[i + 1] == '*')
            {
                const auto end = src.indexOf (i + 2, "**");
                if (end > 0)
                {
                    const auto inner = src.substring (i + 2, end);
                    b.bold.add (Range<int>::withStartAndLength (out.length(), inner.length()));
                    out << inner;
                    i = end + 2;
                    continue;
                }
            }
            /* *italic*: a single star hard against a word, closed by one hard
               against a word. A star standing alone is left as it is. */
            if (src[i] == '*' && i + 1 < src.length() && src[i + 1] != '*' && src[i + 1] != ' ')
            {
                const auto end = src.indexOf (i + 1, "*");
                if (end > i + 1 && src[end - 1] != ' ' && ! src.substring (i + 1, end).containsChar ('`'))
                {
                    const auto inner = src.substring (i + 1, end);
                    b.italic.add (Range<int>::withStartAndLength (out.length(), inner.length()));
                    out << inner;
                    i = end + 1;
                    continue;
                }
            }
            if (src[i] == '`')
            {
                const auto end = src.indexOf (i + 1, "`");
                if (end > 0)
                {
                    const auto inner = src.substring (i + 1, end);
                    b.mono.add (Range<int>::withStartAndLength (out.length(), inner.length()));
                    out << inner;
                    i = end + 1;
                    continue;
                }
            }
            // Markdown links: keep the text, drop the target -- there is nothing
            // to click to in a desktop window and the raw URL is just noise.
            if (src[i] == '[')
            {
                const auto close = src.indexOf (i + 1, "]");
                if (close > 0 && close + 1 < src.length() && src[close + 1] == '(')
                {
                    const auto endParen = src.indexOf (close + 2, ")");
                    if (endParen > 0)
                    {
                        out << src.substring (i + 1, close);
                        i = endParen + 1;
                        continue;
                    }
                }
            }
            out << src[i];
            ++i;
        }

        b.text = out;
        return b;
    }
};

//==============================================================================
/** Draws the parsed blocks and handles wrapping. */
class ManualView : public Component
{
public:
    explicit ManualView (Array<Block> b) : blocks (std::move (b))
    {
        setWantsKeyboardFocus (true);
        setMouseCursor (MouseCursor::IBeamCursor);
    }

    //==========================================================================
    /* Copying. Text is chosen a piece at a time -- a paragraph, a step, a
       command, a table row -- which is what gets copied out of a manual, and
       cannot go wrong half-way through a word. Drag or shift-click for more
       than one; Ctrl+C or the right-click menu copies. */
    void mouseDown (const MouseEvent& e) override
    {
        grabKeyboardFocus();
        const int at = pieceAt (e.y);

        if (e.mods.isPopupMenu())
        {
            if (at >= 0 && ! isChosen (at)) { anchor = last = at; repaint(); }

            PopupMenu m;
            m.addItem (1, "Copy", anchor >= 0);
            m.addItem (2, "Copy this whole section", at >= 0);
            m.addSeparator();
            m.addItem (3, "Select everything");
            m.showMenuAsync (PopupMenu::Options().withMousePosition(),
                             [safe = Component::SafePointer<ManualView> (this), at] (int choice)
            {
                if (safe == nullptr) return;
                if (choice == 1) safe->copyChosen();
                if (choice == 2) safe->chooseSection (at), safe->copyChosen();
                if (choice == 3) safe->chooseAll();
            });
            return;
        }

        if (e.mods.isShiftDown() && anchor >= 0 && at >= 0) last = at;
        else                                                anchor = last = at;
        repaint();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || anchor < 0) return;
        if (const int at = pieceAt (e.y); at >= 0 && at != last) { last = at; repaint(); }

        if (auto* vp = findParentComponentOfClass<Viewport>())
        {
            const auto inVp = e.getEventRelativeTo (vp);
            vp->autoScroll (inVp.x, inVp.y, 24, 14);
        }
    }

    bool keyPressed (const KeyPress& k) override
    {
        if (k == KeyPress ('c', ModifierKeys::commandModifier, 0)) { copyChosen(); return true; }
        if (k == KeyPress ('a', ModifierKeys::commandModifier, 0)) { chooseAll();  return true; }
        if (k == KeyPress::escapeKey && anchor >= 0) { anchor = last = -1; repaint(); return true; }
        return false;
    }

    void setFilter (const String& f)
    {
        anchor = last = -1;
        filter = f.trim();
        layout (getWidth());
        repaint();
    }

    /** Scrolls so `heading` is at the top. Matched loosely so callers can pass
        something human ("tablet") rather than an exact title. */
    int findHeadingY (const String& heading) const
    {
        for (const auto& b : visible)
            if (isHeading (b.kind) && b.text.containsIgnoreCase (heading))
                return b.y;
        return -1;
    }

    /** The contents list. Built from the parsed blocks rather than the laid-out
        ones: layout needs a width, and at construction there isn't one yet,
        which used to leave the list empty until the window was resized. */
    StringArray headings() const
    {
        StringArray h;
        for (const auto& b : (filter.isEmpty() ? blocks : visibleAsBlocks()))
            if (b.kind == Block::Kind::heading2 || b.kind == Block::Kind::heading3)
                h.add ((b.kind == Block::Kind::heading3 ? "    " : "") + b.text);
        return h;
    }

    /** While filtering, the contents should list only what survived. */
    Array<Block> visibleAsBlocks() const
    {
        Array<Block> out;
        for (const auto& p : visible) out.add (static_cast<const Block&> (p));
        return out;
    }

    int yForHeadingIndex (int index) const
    {
        int seen = 0;
        for (const auto& b : visible)
            if (b.kind == Block::Kind::heading2 || b.kind == Block::Kind::heading3)
                if (seen++ == index) return b.y;
        return -1;
    }

    /** Lays out at a given width without needing to be on screen, so the
        contents list and any jump target are correct before the first paint. */
    void prepare (int width) { layout (width); }

    void resized() override { layout (getWidth()); }

    void paint (Graphics& g) override
    {
        g.fillAll (bgDark);
        const auto clip = g.getClipBounds();

        for (int i = 0; i < visible.size(); ++i)
        {
            const auto& b = visible.getReference (i);
            if (b.y + b.height < clip.getY() || b.y > clip.getBottom()) continue;

            if (isChosen (i) && b.height > 1)
            {
                g.setColour (accent.withAlpha (0.28f));
                g.fillRect (14, b.y - 3, getWidth() - 28, b.height + 5);
            }
            paintBlock (g, b);
        }

        if (visible.isEmpty() && filter.isNotEmpty())
        {
            g.setColour (textDim);
            g.setFont (FontOptions (15.0f));
            g.drawFittedText ("Nothing in the manual matches \"" + filter + "\".",
                              getLocalBounds().withTrimmedTop (40), Justification::centredTop, 2);
        }
    }

private:
    struct Placed : Block { int y = 0; };

    static bool isHeading (Block::Kind k)
    {
        return k == Block::Kind::heading1 || k == Block::Kind::heading2 || k == Block::Kind::heading3;
    }

    static Font fontFor (Block::Kind k)
    {
        switch (k)
        {
            case Block::Kind::heading1: return Font (FontOptions (26.0f, Font::bold));
            case Block::Kind::heading2: return Font (FontOptions (19.0f, Font::bold));
            case Block::Kind::heading3: return Font (FontOptions (15.5f, Font::bold));
            case Block::Kind::code:     return Font (FontOptions (Font::getDefaultMonospacedFontName(), 13.0f, Font::plain));
            default:                    return Font (FontOptions (14.5f));
        }
    }

    static int indentFor (Block::Kind k)
    {
        return k == Block::Kind::bullet || k == Block::Kind::numbered ? 22 : 0;
    }

    /* Appends text to an AttributedString, switching font across the bold and
       inline-code runs. Shared between measuring and painting so the two cannot
       disagree about how wide a line is. */
    static void appendStyled (AttributedString& s, const String& text,
                              const Array<Range<int>>& bold, const Array<Range<int>>& mono,
                              const Array<Range<int>>& italic, const Font& base, Colour colour)
    {
        const int len = text.length();
        int pos = 0;
        while (pos < len)
        {
            int next = len;
            bool isBold = false, isMono = false, isItalic = false;

            for (const auto& r : bold)
            {
                if (pos >= r.getStart() && pos < r.getEnd()) { isBold = true; next = jmin (next, r.getEnd()); }
                else if (r.getStart() > pos) next = jmin (next, r.getStart());
            }
            for (const auto& r : mono)
            {
                if (pos >= r.getStart() && pos < r.getEnd()) { isMono = true; next = jmin (next, r.getEnd()); }
                else if (r.getStart() > pos) next = jmin (next, r.getStart());
            }

            for (const auto& r : italic)
            {
                if (pos >= r.getStart() && pos < r.getEnd()) { isItalic = true; next = jmin (next, r.getEnd()); }
                else if (r.getStart() > pos) next = jmin (next, r.getStart());
            }

            auto f = base;
            if (isMono) f = Font (FontOptions (Font::getDefaultMonospacedFontName(), base.getHeight() - 1.0f, Font::plain));
            else if (isBold) f = base.boldened();
            else if (isItalic) f = base.italicised();

            s.append (text.substring (pos, next), f, isMono ? Colour (0xffb8d4a8) : colour);
            pos = next;
        }
    }

    /** Where each column of a table row sits within `avail`, as (x, width).
        Two columns are a narrow label and its description; with more, the
        columns after the first share what is left, so none is drawn over
        another. */
    static Array<Range<int>> columnsFor (int count, int avail)
    {
        const int gap = 16;
        Array<Range<int>> cols;
        if (count <= 0) return cols;

        const int first = count == 1 ? avail
                        : count == 2 ? jmin (190, avail / 3)
                                     : jmin (230, avail / (count + 1));
        cols.add (Range<int>::withStartAndLength (0, first));

        const int rest = count - 1;
        const int each = rest > 0 ? (avail - first - gap * rest) / rest : 0;
        for (int c = 0; c < rest; ++c)
            cols.add (Range<int>::withStartAndLength (first + gap + c * (each + gap), jmax (40, each)));
        return cols;
    }

    static AttributedString cellText (const Block& b, int c, Colour colour)
    {
        const Array<Range<int>> none;
        auto font = fontFor (Block::Kind::paragraph);
        if (b.header) font = Font (FontOptions (12.5f, Font::bold));

        AttributedString as;
        appendStyled (as, b.header ? b.cells[c].toUpperCase() : b.cells[c],
                      c < b.cellBold.size() ? b.cellBold[c] : none,
                      c < b.cellMono.size() ? b.cellMono[c] : none,
                      c < b.cellItalic.size() ? b.cellItalic[c] : none,
                      font, colour);
        as.setLineSpacing (3.0f);
        return as;
    }

    /** True when the block belongs to a section we are showing. Computed from a
        first pass over the blocks, so a match late in a section still brings the
        earlier parts of it along. */
    bool sectionWanted (const Block& b) const { return wantedSections.contains (b.sectionKey); }

    void markWantedSections (const String& f)
    {
        wantedSections.clear();
        for (const auto& b : blocks)
            if (b.text.containsIgnoreCase (f) || b.sectionKey.containsIgnoreCase (f))
                wantedSections.addIfNotAlreadyThere (b.sectionKey);
    }

    void layout (int width)
    {
        visible.clear();
        if (width <= 0) return;
        if (filter.isNotEmpty()) markWantedSections (filter);

        const int margin = 26;
        const int textWidth = jmax (120, width - margin * 2);

        /* Filtering keeps the section heading above any matching block, so a
           result is never a stray sentence with no indication of where it came
           from. */
        String currentH2, currentH3;
        bool emittedH2 = false, emittedH3 = false;

        int y = 18;
        for (const auto& b : blocks)
        {
            if (b.kind == Block::Kind::heading2) { currentH2 = b.text; emittedH2 = false; currentH3.clear(); emittedH3 = true; }
            if (b.kind == Block::Kind::heading3) { currentH3 = b.text; emittedH3 = false; }

            if (filter.isNotEmpty())
            {
                /* Show whole sections, not matching sentences. Someone
                   searching "firewall" needs the ufw commands that follow the
                   explanation, and those commands contain neither the word
                   "firewall" nor anything else they typed. A section is shown
                   when its heading matches or when anything inside it does. */
                if (! sectionWanted (b)) continue;

                if (! emittedH2 && currentH2.isNotEmpty() && b.kind != Block::Kind::heading2)
                {
                    Block h; h.kind = Block::Kind::heading2; h.text = currentH2;
                    place (h, y, textWidth, margin);
                    emittedH2 = true;
                }
                if (! emittedH3 && currentH3.isNotEmpty() && b.kind != Block::Kind::heading3)
                {
                    Block h; h.kind = Block::Kind::heading3; h.text = currentH3;
                    place (h, y, textWidth, margin);
                    emittedH3 = true;
                }
                if (b.kind == Block::Kind::heading2) emittedH2 = true;
                if (b.kind == Block::Kind::heading3) emittedH3 = true;
            }

            place (b, y, textWidth, margin);
        }

        setSize (width, y + 40);
    }

    void place (const Block& b, int& y, int textWidth, int margin)
    {
        Placed p;
        static_cast<Block&> (p) = b;
        p.y = y + spaceBefore (b.kind);

        const int avail = textWidth - indentFor (b.kind);

        if (b.kind == Block::Kind::tableRow)
        {
            // Height is whichever cell wraps tallest.
            const auto cols = columnsFor (b.cells.size(), avail);
            int tallest = 0;
            for (int c = 0; c < b.cells.size(); ++c)
            {
                TextLayout tl;
                tl.createLayout (cellText (b, c, Colours::white), (float) cols[c].getLength());
                tallest = jmax (tallest, (int) std::ceil (tl.getHeight()));
            }
            p.height = tallest + (b.header ? 12 : 8);
        }
        else if (b.kind == Block::Kind::rule)     p.height = 1;
        else if (b.kind == Block::Kind::image)    p.height = b.image.isValid()
                                                                ? jmin (b.image.getHeight(),
                                                                        (int) (avail * (double) b.image.getHeight() / jmax (1, b.image.getWidth())))
                                                                : 0;
        else if (b.kind == Block::Kind::code)
        {
            const int lines = StringArray::fromLines (b.text).size();
            p.height = lines * 17 + 16;
        }
        else
        {
            /* Measure with the same styling we paint with: a bold run is wider
               than the plain text, and measuring plain then painting bold is
               what makes a long item overlap whatever follows it. */
            AttributedString s;
            appendStyled (s, b.text, b.bold, b.mono, b.italic, fontFor (b.kind), Colours::white);
            s.setLineSpacing (3.0f);
            TextLayout tl;
            tl.createLayout (s, (float) avail);
            p.height = (int) std::ceil (tl.getHeight()) + 3;
        }

        y = p.y + p.height;
        p.marker = b.marker;
        visible.add (p);
    }

    static int spaceBefore (Block::Kind k)
    {
        switch (k)
        {
            case Block::Kind::heading1: return 10;
            case Block::Kind::heading2: return 26;
            case Block::Kind::heading3: return 18;
            case Block::Kind::code:     return 10;
            case Block::Kind::image:    return 12;
            case Block::Kind::rule:     return 16;
            case Block::Kind::bullet:
            case Block::Kind::numbered: return 7;
            case Block::Kind::tableRow: return 2;
            default:                    return 11;
        }
    }

    void paintBlock (Graphics& g, const Placed& b)
    {
        const int margin = 26;
        const int indent = indentFor (b.kind);
        auto area = Rectangle<int> (margin + indent, b.y, getWidth() - margin * 2 - indent, b.height);

        switch (b.kind)
        {
            case Block::Kind::rule:
                g.setColour (Colours::white.withAlpha (0.12f));
                g.fillRect (area.withHeight (1));
                return;

            case Block::Kind::image:
                if (b.image.isValid())
                    g.drawImage (b.image, area.toFloat(), RectanglePlacement::xLeft | RectanglePlacement::yTop);
                return;

            case Block::Kind::code:
            {
                g.setColour (bgCode);
                g.fillRoundedRectangle (area.toFloat().expanded (6.0f, 2.0f), 4.0f);
                g.setColour (Colour (0xffb8d4a8));
                g.setFont (fontFor (b.kind));
                int y = area.getY() + 8;
                for (const auto& line : StringArray::fromLines (b.text))
                {
                    g.drawSingleLineText (line, area.getX() + 4, y + 12);
                    y += 17;
                }
                return;
            }

            case Block::Kind::tableRow:
            {
                const auto cols = columnsFor (b.cells.size(), area.getWidth());
                g.setColour (Colours::white.withAlpha (0.06f));
                g.fillRect (area.withHeight (1));

                for (int c = 0; c < b.cells.size(); ++c)
                {
                    const auto cellArea = Rectangle<int> (area.getX() + cols[c].getStart(), area.getY(),
                                                          cols[c].getLength(), area.getHeight())
                                              .withTrimmedTop (b.header ? 8 : 5);

                    cellText (b, c, b.header ? textDim
                                  : c == 0   ? Colours::white : perf::theme::text.withAlpha (0.9f))
                        .draw (g, cellArea.toFloat());
                }
                return;
            }

            case Block::Kind::bullet:
                g.setColour (accent);
                g.setFont (fontFor (Block::Kind::paragraph));
                g.drawSingleLineText ("-", margin + 8, b.y + 14);
                break;

            case Block::Kind::numbered:
                g.setColour (accent);
                g.setFont (fontFor (Block::Kind::paragraph));
                g.drawSingleLineText (b.marker, margin + 2, b.y + 14);
                break;

            default: break;
        }

        const auto base = fontFor (b.kind);
        const auto colour = isHeading (b.kind) ? Colours::white
                          : b.kind == Block::Kind::paragraph || b.kind == Block::Kind::bullet
                            || b.kind == Block::Kind::numbered ? perf::theme::text.withAlpha (0.9f)
                          : textDim;

        AttributedString s;
        appendStyled (s, b.text, b.bold, b.mono, b.italic, base, colour);
        s.setLineSpacing (3.0f);
        s.draw (g, area.toFloat());
    }

    //==========================================================================
    bool isChosen (int i) const
    {
        return anchor >= 0 && i >= jmin (anchor, last) && i <= jmax (anchor, last);
    }

    /** The piece under y, or the nearest one above it; -1 above the first. */
    int pieceAt (int y) const
    {
        int found = -1;
        for (int i = 0; i < visible.size(); ++i)
        {
            if (visible.getReference (i).y - 4 > y) break;
            found = i;
        }
        return found;
    }

    void chooseAll()
    {
        if (visible.isEmpty()) return;
        anchor = 0; last = visible.size() - 1;
        repaint();
    }

    /** From the heading above piece `at` down to the next heading of its level
        or higher. */
    void chooseSection (int at)
    {
        if (! isPositiveAndBelow (at, visible.size())) return;
        auto level = [] (Block::Kind k) { return k == Block::Kind::heading1 ? 1 : k == Block::Kind::heading2 ? 2
                                               : k == Block::Kind::heading3 ? 3 : 9; };
        int from = at;
        while (from > 0 && ! isHeading (visible.getReference (from).kind)) --from;
        const int top = level (visible.getReference (from).kind);
        int to = from;
        while (to + 1 < visible.size() && level (visible.getReference (to + 1).kind) > top) ++to;
        anchor = from; last = to;
        repaint();
    }

    void copyChosen()
    {
        if (anchor < 0) return;
        String out;
        bool wasRow = false;

        for (int i = jmin (anchor, last); i <= jmax (anchor, last) && i < visible.size(); ++i)
        {
            const auto& b = visible.getReference (i);
            String line;
            switch (b.kind)
            {
                case Block::Kind::rule:
                case Block::Kind::image:    continue;
                case Block::Kind::bullet:   line = "- " + b.text; break;
                case Block::Kind::numbered: line = b.marker + " " + b.text; break;
                case Block::Kind::tableRow: line = b.cells.joinIntoString ("\t"); break;
                default:                    line = b.text; break;
            }

            // List items and table rows sit on consecutive lines; everything
            // else is set apart by an empty one.
            const bool row = b.kind == Block::Kind::bullet || b.kind == Block::Kind::numbered
                             || b.kind == Block::Kind::tableRow;
            if (out.isNotEmpty()) out << (row && wasRow ? "\n" : "\n\n");
            out << line;
            wasRow = row;
        }

        if (out.isNotEmpty()) SystemClipboard::copyTextToClipboard (out);
    }

    Array<Block> blocks;
    Array<Placed> visible;
    int anchor = -1, last = -1;     // the chosen pieces, as indexes into `visible`
    String filter;
    StringArray wantedSections;
};

//==============================================================================
class HelpComponent : public Component
{
public:
    HelpComponent()
    {
        setComponentID ("helpContent");
        const auto markdown = String::createStringFromData (BinaryData::MANUAL_md, BinaryData::MANUAL_mdSize);

        // Images live beside the manual in the source tree. In an installed
        // build they may not be there, and the manual still reads fine without
        // them, so a missing image is simply skipped.
        File imageDir (File::getSpecialLocation (File::currentExecutableFile));
        for (int i = 0; i < 6 && imageDir.exists(); ++i)
        {
            if (imageDir.getChildFile ("docs/images").isDirectory())
            {
                imageDir = imageDir.getChildFile ("docs");
                break;
            }
            imageDir = imageDir.getParentDirectory();
        }

        view = std::make_unique<ManualView> (ManualParser::parse (markdown, imageDir));
        viewport.setViewedComponent (view.get(), false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        search.setComponentID ("helpSearch");
        search.setTextToShowWhenEmpty ("Search the manual", textDim);
        search.setColour (TextEditor::backgroundColourId, bgCode);
        search.setColour (TextEditor::outlineColourId, Colours::white.withAlpha (0.12f));
        search.onTextChange = [this]
        {
            view->setFilter (search.getText());
            rebuildContents();
            viewport.setViewPosition (0, 0);
        };
        addAndMakeVisible (search);

        contents.setColour (ListBox::backgroundColourId, bgPanel);
        contents.setRowHeight (22);
        addAndMakeVisible (contents);

        setSize (980, 720);
        view->prepare (viewport.getWidth() - viewport.getScrollBarThickness());
        rebuildContents();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto side = r.removeFromLeft (250);
        search.setBounds (side.removeFromTop (34).reduced (8, 5));
        contents.setBounds (side.reduced (8, 0));
        viewport.setBounds (r);
        view->setSize (viewport.getWidth() - viewport.getScrollBarThickness(), view->getHeight());
    }

    void paint (Graphics& g) override
    {
        g.fillAll (bgDark);
        g.setColour (bgPanel);
        g.fillRect (0, 0, 250, getHeight());
    }

    void scrollTo (const String& section)
    {
        if (section.isEmpty()) return;
        if (const auto y = view->findHeadingY (section); y >= 0)
            viewport.setViewPosition (0, jmax (0, y - 12));
    }

private:
    struct ContentsModel : public ListBoxModel
    {
        explicit ContentsModel (HelpComponent& o) : owner (o) {}

        int getNumRows() override { return items.size(); }

        void paintListBoxItem (int row, Graphics& g, int w, int h, bool selected) override
        {
            if (! isPositiveAndBelow (row, items.size())) return;
            if (selected) { g.setColour (Colours::white.withAlpha (0.10f)); g.fillRect (0, 0, w, h); }
            const auto text = items[row];
            const bool sub = text.startsWith ("    ");
            g.setColour (sub ? textDim : perf::theme::text.withAlpha (0.9f));
            g.setFont (FontOptions (sub ? 12.5f : 13.5f, sub ? Font::plain : Font::bold));
            g.drawFittedText (text.trim(), sub ? 22 : 10, 0, w - 26, h, Justification::centredLeft, 1);
        }

        void listBoxItemClicked (int row, const MouseEvent&) override { owner.jumpTo (row); }

        HelpComponent& owner;
        StringArray items;
    };

    void rebuildContents()
    {
        model.items = view->headings();
        contents.setModel (&model);
        contents.updateContent();
        contents.repaint();
    }

    void jumpTo (int headingIndex)
    {
        if (const auto y = view->yForHeadingIndex (headingIndex); y >= 0)
            viewport.setViewPosition (0, jmax (0, y - 12));
    }

    std::unique_ptr<ManualView> view;
    Viewport viewport;
    TextEditor search;
    ListBox contents;
    ContentsModel model { *this };
};

//==============================================================================
class HelpDocumentWindow : public DocumentWindow
{
public:
    HelpDocumentWindow() : DocumentWindow ("StageRig manual", bgDark, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        auto* c = new HelpComponent();
        setContentOwned (c, true);
        setResizable (true, false);
        centreWithSize (980, 720);
        setVisible (true);
    }

    void closeButtonPressed() override { HelpWindow::close(); }

    HelpComponent* content() { return dynamic_cast<HelpComponent*> (getContentComponent()); }
};

static std::unique_ptr<HelpDocumentWindow> helpWindow;

void HelpWindow::show (const String& section)
{
    if (helpWindow == nullptr)
        helpWindow = std::make_unique<HelpDocumentWindow>();

    helpWindow->setVisible (true);
    helpWindow->toFront (true);

    if (auto* c = helpWindow->content())
        MessageManager::callAsync ([c, section] { c->scrollTo (section); });
}

void HelpWindow::close() { helpWindow.reset(); }

} // namespace perf
