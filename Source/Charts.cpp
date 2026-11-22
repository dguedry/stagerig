#include "Charts.h"

using namespace juce;

namespace perf
{

String chartKindFor (const File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    if (ext == ".pdf") return "pdf";
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".webp") return "image";
    if (ext == ".cho" || ext == ".crd" || ext == ".chordpro" || ext == ".chopro") return "chordpro";
    if (ext == ".txt") return "text";
    return {};
}

bool isChartFile (const File& f)              { return chartKindFor (f).isNotEmpty(); }

String chartFilePatterns()                     { return "*.pdf;*.png;*.jpg;*.jpeg;*.gif;*.webp;*.txt;*.cho;*.crd;*.chordpro;*.chopro"; }

String chartContentTypeFor (const File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    if (ext == ".pdf")                   return "application/pdf";
    if (ext == ".png")                   return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")                   return "image/gif";
    if (ext == ".webp")                  return "image/webp";
    return "text/plain; charset=utf-8";
}

File chartPagesFolder (const File& chart)     { return chart.getSiblingFile (chart.getFileName() + ".pages"); }

Array<File> chartPages (const File& chart)
{
    Array<File> pages;
    const auto folder = chartPagesFolder (chart);
    if (! folder.isDirectory()) return pages;
    /* pdftoppm zero-pads page numbers to the width of the last one, so the
       names sort in page order as plain strings. */
    pages = folder.findChildFiles (File::findFiles, false, "page-*.png");
    pages.sort();
    return pages;
}

bool prerenderPdfPages (const File& pdf, String& error)
{
    File tool;
    for (auto* dir : { "/usr/bin", "/usr/local/bin" })
        if (auto f = File (dir).getChildFile ("pdftoppm"); f.existsAsFile()) { tool = f; break; }
    if (tool == File())
    {
        error = "pdftoppm is not installed (package poppler-utils), so this PDF will open in the browser as it is.";
        return false;
    }

    const auto folder = chartPagesFolder (pdf);
    folder.deleteRecursively();
    if (! folder.createDirectory()) { error = "Could not create " + folder.getFullPathName(); return false; }

    /* 120 dpi: a letter page comes out about 1000 px wide, which reads at full
       width on a tablet without being heavy to send; PNG keeps staff lines
       and small text crisp where JPEG would smear them. */
    ChildProcess p;
    if (! p.start (StringArray { tool.getFullPathName(), "-png", "-r", "120",
                                 pdf.getFullPathName(), folder.getChildFile ("page").getFullPathName() }))
    {
        error = "Could not start pdftoppm.";
        return false;
    }
    p.waitForProcessToFinish (180000);
    if (chartPages (pdf).isEmpty()) { error = "pdftoppm produced no pages from " + pdf.getFileName(); return false; }
    return true;
}

void prerenderPdfPagesAsync (const File& pdf)
{
    Thread::launch ([pdf]
    {
        String error;
        if (! prerenderPdfPages (pdf, error))
            std::fprintf (stderr, "[charts] %s\n", error.toRawUTF8());
    });
}

bool isTrackFile (const File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;mid;midi");
}

String trackFilePatterns()
{
    return "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.mid;*.midi";
}

//==============================================================================
// Chords moved to a key
//==============================================================================
String noteNameFor (int pitchClass, bool flats)
{
    static const char* const sharps[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* const flatNames[] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    const int pc = (pitchClass % 12 + 12) % 12;
    return flats ? flatNames[pc] : sharps[pc];
}

/* The root at the head of a chord name, and how many characters it takes:
   a letter A-G and a sharp or flat after it. -1 when it does not start with one. */
static int rootAtHead (const String& s, int& length)
{
    length = 0;
    if (s.isEmpty()) return -1;
    static const int letters[] = { 9, 11, 0, 2, 4, 5, 7 };    // A B C D E F G
    const auto c = s[0];
    if (c < 'A' || c > 'G') return -1;
    int pc = letters[c - 'A'];
    length = 1;
    if (s.length() > 1)
    {
        if (s[1] == '#')      { ++pc; ++length; }
        else if (s[1] == 'b') { --pc; ++length; }
    }
    return (pc % 12 + 12) % 12;
}

int pitchClassOf (const String& name)
{
    int n;
    return rootAtHead (name.trim(), n);
}

int chordProKeyRoot (const String& text)
{
    for (const auto& raw : StringArray::fromLines (text))
    {
        const auto line = raw.trim();
        if (! (line.startsWith ("{") && line.endsWith ("}"))) continue;
        const auto inner = line.substring (1, line.length() - 1).trim();
        if (inner.upToFirstOccurrenceOf (":", false, false).trim().toLowerCase() != "key") continue;
        return pitchClassOf (inner.fromFirstOccurrenceOf (":", false, false));
    }
    return -1;
}

String transposeChordName (const String& chord, int semitones, bool flats)
{
    /* A note written sharp stays sharp, written flat stays flat -- the
       leading tone in D minor is C#, whatever the key signature says -- and
       a natural takes the key's spelling. */
    auto spelt = [flats] (const String& written, int length, int pc)
    {
        const bool sharp = length == 2 && written[1] == '#', flat = length == 2 && written[1] == 'b';
        return noteNameFor (pc, sharp ? false : flat ? true : flats);
    };
    int n;
    const int root = rootAtHead (chord, n);
    if (root < 0) return chord;
    String out = spelt (chord, n, root + semitones);
    String rest = chord.substring (n);
    // A slash bass moves too: D/F# up a tone is E/G#.
    if (const int slash = rest.indexOfChar ('/'); slash >= 0)
    {
        int bn;
        const String bassName = rest.substring (slash + 1);
        if (const int bass = rootAtHead (bassName, bn); bass >= 0)
            rest = rest.substring (0, slash + 1) + spelt (bassName, bn, bass + semitones) + rest.substring (slash + 1 + bn);
    }
    return out + rest;
}

String transposeChordPro (const String& text, int toRoot, bool flats)
{
    const int from = chordProKeyRoot (text);
    if (from < 0 || toRoot < 0 || from == ((toRoot % 12 + 12) % 12)) return text;
    const int delta = ((toRoot - from) % 12 + 12) % 12;

    String out;
    int pos = 0;
    while (pos <= text.length())
    {
        const int nl = text.indexOfChar (pos, '\n');
        const String line = nl < 0 ? text.substring (pos) : text.substring (pos, nl);
        const String t = line.trim();
        if (t.startsWith ("{") && t.endsWith ("}")
            && t.substring (1).upToFirstOccurrenceOf (":", false, false).trim().toLowerCase() == "key")
        {
            // {key: G} -> {key: Bb}; a minor key keeps its "m" and whatever else follows the note.
            const String head = line.upToFirstOccurrenceOf (":", true, false);
            const String value = t.substring (1, t.length() - 1).fromFirstOccurrenceOf (":", false, false).trim();
            out += head + " " + transposeChordName (value, delta, flats) + "}";
        }
        else
        {
            int p = 0;
            while (p < line.length())
            {
                const int open = line.indexOfChar (p, '[');
                if (open < 0) { out += line.substring (p); break; }
                const int close = line.indexOfChar (open, ']');
                if (close < 0) { out += line.substring (p); break; }
                out += line.substring (p, open + 1) + transposeChordName (line.substring (open + 1, close), delta, flats) + "]";
                p = close + 1;
            }
        }
        if (nl < 0) break;
        out += "\n";
        pos = nl + 1;
    }
    return out;
}

//==============================================================================
// Chords above the words, as typed on the web, into ChordPro
//==============================================================================
namespace
{
    /* A chord name: a root, then only what chord names are made of, then
       perhaps a slash bass. "Am7", "F#m7b5", "Dsus4", "Eadd9", "G/B", "C(add9)".
       Strict on purpose: "A" and "Am" are also words, and a line is only
       taken for chords when everything on it passes. */
    bool isChordName (const String& token)
    {
        int n;
        if (rootAtHead (token, n) < 0) return false;
        String rest = token.substring (n);
        if (const int slash = rest.indexOfChar ('/'); slash >= 0)
        {
            int bn;
            const String bass = rest.substring (slash + 1);
            if (rootAtHead (bass, bn) < 0 || bn != bass.length()) return false;
            rest = rest.substring (0, slash);
        }
        static const char* const parts[] = { "maj", "min", "dim", "aug", "sus", "add", "m", "M", "+", "-", "#", "b", "(", ")",
                                             "\xc2\xb0", "\xc3\xb8", "\xce\x94" };     // degree sign, o-slash, delta
        while (rest.isNotEmpty())
        {
            if (CharacterFunctions::isDigit (rest[0])) { rest = rest.substring (1); continue; }
            bool took = false;
            for (const auto* part : parts)
            {
                const String ps (CharPointer_UTF8 { part });
                if (rest.startsWith (ps)) { rest = rest.substring (ps.length()); took = true; break; }
            }
            if (! took) return false;
        }
        return true;
    }

    /* What sits between chords on a chord line without being one: bar lines, repeats, "no chord". */
    bool isChordLineFiller (const String& token)
    {
        if (token.containsOnly ("|/-%.:")) return true;
        const auto t = token.toLowerCase().removeCharacters ("()");
        if (t == "n.c." || t == "nc" || t == "n.c") return true;
        return (t.startsWithChar ('x') && t.substring (1).containsOnly ("0123456789") && t.length() > 1)
            || (t.endsWithChar ('x') && t.dropLastCharacters (1).containsOnly ("0123456789") && t.length() > 1);
    }

    struct Token { int column; String text; };

    std::vector<Token> tokensOf (const String& line)
    {
        std::vector<Token> out;
        int i = 0;
        while (i < line.length())
        {
            while (i < line.length() && CharacterFunctions::isWhitespace (line[i])) ++i;
            const int start = i;
            while (i < line.length() && ! CharacterFunctions::isWhitespace (line[i])) ++i;
            if (i > start) out.push_back ({ start, line.substring (start, i) });
        }
        return out;
    }

    bool isChordLine (const String& line)
    {
        const auto tokens = tokensOf (line);
        int chords = 0;
        for (const auto& t : tokens)
        {
            if (isChordName (t.text)) ++chords;
            else if (! isChordLineFiller (t.text)) return false;
        }
        return chords > 0;
    }

    /* "[Verse]", "[Chorus 2]": a heading in square brackets -- not a chord in them, which is ChordPro already. */
    String headingOf (const String& line)
    {
        const auto t = line.trim();
        if (! (t.startsWithChar ('[') && t.endsWithChar (']')) || t.length() < 3) return {};
        const auto inner = t.substring (1, t.length() - 1).trim();
        if (inner.isEmpty() || inner.containsAnyOf ("[]") || isChordName (inner)) return {};
        return inner;
    }

    /* Tabs to the eight-column stops they were typed against, or the chords would land early. */
    String withoutTabs (const String& line)
    {
        if (! line.containsChar ('\t')) return line;
        String out;
        for (int i = 0; i < line.length(); ++i)
        {
            if (line[i] == '\t') out += String::repeatedString (" ", 8 - out.length() % 8);
            else                 out += String::charToString (line[i]);
        }
        return out;
    }

    StringArray plainLines (const String& text)
    {
        auto lines = StringArray::fromLines (text);
        for (auto& l : lines)
            l = withoutTabs (l.replaceCharacter ((juce_wchar) 0xa0, ' ')).trimEnd();      // web pages pad with no-break spaces
        return lines;
    }
}

bool hasChordsOverWords (const String& text)
{
    for (const auto& l : plainLines (text))
        if (isChordLine (l)) return true;
    return false;
}

String guessKeyOfChordsOverWords (const String& text)
{
    /* How often each chord is played, as major or minor on its root. */
    float major[12] = {}, minor[12] = {};
    bool any = false;
    for (const auto& l : plainLines (text))
    {
        if (! isChordLine (l)) continue;
        for (const auto& t : tokensOf (l))
        {
            if (! isChordName (t.text)) continue;
            int n;
            const int root = rootAtHead (t.text, n);
            const String rest = t.text.substring (n);
            const bool isMinor = (rest.startsWithChar ('m') && ! rest.startsWith ("maj")) || rest.startsWith ("dim");
            (isMinor ? minor : major)[root] += 1.0f;
            any = true;
        }
    }
    if (! any) return {};
    float total = 0.0f;
    for (int i = 0; i < 12; ++i) total += major[i] + minor[i];

    /* Each key is scored by how much of what is played belongs to it, its
       own chord counting double: a song sits on its home chord. The flat
       seventh counts a little in a major key, as rock and soul lean on it.
       What does not belong counts against it. */
    float best = -1.0f;
    int bestRoot = 0;
    bool bestMinor = false;
    for (int k = 0; k < 12; ++k)
    {
        auto M = [&] (int step) { return major[(k + step) % 12]; };
        auto m = [&] (int step) { return minor[(k + step) % 12]; };
        const float inMajor = M (0) + m (2) + m (4) + M (5) + M (7) + m (9) + M (10);
        const float inMinor = m (0) + M (3) + m (5) + m (7) + M (7) + M (8) + M (10);
        const float asMajor = inMajor + M (0) - 0.5f * M (10) - 0.5f * (total - inMajor);
        const float asMinor = inMinor + m (0) - 0.5f * (total - inMinor);
        if (asMajor > best) { best = asMajor; bestRoot = k; bestMinor = false; }
        if (asMinor > best) { best = asMinor; bestRoot = k; bestMinor = true; }
    }
    // Spelt as the chart spells it: flats if it uses more flats than sharps.
    int sharps = 0, flats = 0;
    for (const auto& l : plainLines (text))
        if (isChordLine (l))
            for (const auto& t : tokensOf (l))
                if (isChordName (t.text) && t.text.length() > 1) { sharps += t.text[1] == '#'; flats += t.text[1] == 'b'; }
    static const bool flatKeys[12] = { false, true, false, true, false, true, false, false, true, false, true, false };
    const bool useFlats = flats != sharps ? flats > sharps : flatKeys[bestMinor ? (bestRoot + 3) % 12 : bestRoot];
    return noteNameFor (bestRoot, useFlats) + (bestMinor ? "m" : "");
}

String chordProFromChordsOverWords (const String& text, const String& title, const String& key)
{
    const auto lines = plainLines (text);
    StringArray out;
    if (title.isNotEmpty()) out.add ("{title: " + title + "}");
    if (key.isNotEmpty())   out.add ("{key: " + key + "}");
    if (out.size() > 0)     out.add ({});

    auto addBlank = [&out] { if (out.size() > 0 && out[out.size() - 1].isNotEmpty()) out.add ({}); };

    for (int i = 0; i < lines.size(); ++i)
    {
        const auto& line = lines[i];
        if (line.trim().isEmpty()) { addBlank(); continue; }
        if (const auto heading = headingOf (line); heading.isNotEmpty())
        {
            addBlank();
            out.add ("{comment: " + heading + "}");
            continue;
        }
        if (! isChordLine (line)) { out.add (line); continue; }

        const auto tokens = tokensOf (line);
        const bool wordsFollow = i + 1 < lines.size() && lines[i + 1].trim().isNotEmpty()
                                 && ! isChordLine (lines[i + 1]) && headingOf (lines[i + 1]).isEmpty();
        if (! wordsFollow)
        {
            /* Chords with no words under them: an intro, a turnaround. Each in
               its brackets, spaced as they were; bar lines and repeats as typed. */
            String s;
            int at = 0;
            for (const auto& t : tokens)
            {
                s += String::repeatedString (" ", jmax (at > 0 ? 1 : 0, t.column - at));
                s += isChordName (t.text) ? "[" + t.text + "]" : t.text;
                at = t.column + t.text.length();
            }
            out.add (s);
            continue;
        }

        /* Each chord into the words, at the column it stood over -- from the
           right, so a chord put in does not move the place of the one before. */
        String words = lines[++i];
        const int length = words.length();
        String after;                                   // chords past the last word follow it, a space apart
        for (int t = (int) tokens.size() - 1; t >= 0; --t)
        {
            const auto& tok = tokens[(size_t) t];
            if (tok.column >= length) { after = " [" + tok.text + "]" + after; continue; }
            /* A chord typed a letter or two into a word belongs to the word:
               charts are spaced by eye, and "l[Am]ook" is never what was meant. */
            int at = tok.column;
            auto inWord = [&words] (int c) { return c >= 0 && c < words.length() && ! CharacterFunctions::isWhitespace (words[c]); };
            for (int back = 0; back < 2 && inWord (at) && inWord (at - 1); ++back) --at;
            if (inWord (at) && inWord (at - 1)) at = tok.column;      // further in than that: left where it was put
            words = words.substring (0, at) + "[" + tok.text + "]" + words.substring (at);
        }
        out.add (words + after);
    }
    while (out.size() > 0 && out[out.size() - 1].isEmpty()) out.remove (out.size() - 1);
    return out.joinIntoString ("\n") + "\n";
}

} // namespace perf
