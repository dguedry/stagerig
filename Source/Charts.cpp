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

} // namespace perf
