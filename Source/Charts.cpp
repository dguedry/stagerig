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

} // namespace perf
