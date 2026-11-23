#include "summary.hpp"

#include <regex>

#include "jobs.hpp"

using json = nlohmann::json;

namespace ai_desktop::summary {

namespace {

std::string trimmed(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

// Does `text` hold `heading` as a whole line at or after `from`? Sets `at` to the position after it.
bool heading_from(const std::string& text, const std::string& heading, std::size_t from, std::size_t& at) {
    std::size_t pos = from;
    while ((pos = text.find(heading, pos)) != std::string::npos) {
        const bool line_start = pos == 0 || text[pos - 1] == '\n';
        const std::size_t after = pos + heading.size();
        const bool line_end = after == text.size() || text[after] == '\n' || text[after] == '\r';
        if (line_start && line_end) {
            at = after;
            return true;
        }
        pos = after;
    }
    return false;
}

}  // namespace

std::string clean(const std::string& block) {
    std::string text = block;
    if (text.size() > kMaxRawBytes) {
        std::size_t cut = kMaxRawBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        text.resize(cut);
    }
    text = jobs::sanitize(text);
    // A path where a person's machine shows: the drive, the share or a folder at the root of a Unix system. The
    // character before it must not be part of a word, so `ai-desktop/x` and a URL stay as they are.
    static const std::regex windows(R"((^|[^A-Za-z0-9])[A-Za-z]:[\\/][^\s`'"<>)\],;]*)");
    static const std::regex unc(R"((^|[^A-Za-z0-9])\\\\[^\s`'"<>)\],;]+)");
    static const std::regex unix(
        R"((^|[\s`'"(\[=])/((home|Users|tmp|var|etc|opt|usr|mnt|root|private|Volumes|srv|proc|workspace)(/[^\s`'"<>)\],;]*)?|[A-Za-z]/[^\s`'"<>)\],;]*))");
    text = std::regex_replace(text, windows, "$1<path>");
    text = std::regex_replace(text, unc, "$1<path>");
    text = std::regex_replace(text, unix, "$1<path>");
    return text;
}

Result extract(const std::string& output, const std::string& job_id, const std::string& date) {
    Result result;
    const std::size_t begin = output.rfind(kBegin);
    if (begin == std::string::npos) {
        result.error = "the answer holds no summary block";
        return result;
    }
    const std::size_t start = begin + std::string(kBegin).size();
    const std::size_t end = output.find(kEnd, start);
    if (end == std::string::npos) {
        result.error = "the summary block of the answer does not end";
        return result;
    }
    // Before cleaning: a block of hundreds of KiB (tool output between the markers) is refused at once.
    if (end - start > kMaxRawBytes) {
        result.error = "the summary block has " + std::to_string(end - start) + " bytes; at most " +
                       std::to_string(kMaxBytes) + " are kept";
        return result;
    }
    const std::string text = trimmed(clean(output.substr(start, end - start)));
    std::size_t at = 0;
    for (const char* heading : kHeadings) {
        if (!heading_from(text, heading, at, at)) {
            result.error = std::string("the summary lacks the heading '") + heading + "' (or has it out of order)";
            return result;
        }
    }
    if (text.size() > kMaxBytes) {
        result.error = "the summary has " + std::to_string(text.size()) + " bytes; at most " +
                       std::to_string(kMaxBytes) + " are kept";
        return result;
    }
    result.summary = {{"schema", kSchema}, {"job_id", job_id}, {"date", date}, {"markdown", text}};
    return result;
}

std::string instructions() {
    std::string text =
        "\n\nWhen you are done (or give the task back), end your last message with one summary block for the person "
        "who handed the task over, between the markers below, in German, at most 3500 bytes, with exactly these four "
        "headings in this order:\n\n";
    text += kBegin;
    text += "\n## Getan\n\n(what you did, in a few lines)\n\n## Commits\n\n(one line per commit: `repo@sha` and what it "
            "holds; \"keine\" when there is none)\n\n## Prüfungen\n\n(each check you ran and its verdict, e.g. `ci "
            "commit` CLEAN or REGRESSION; \"keine\" when you ran none)\n\n## Offen\n\n(what is left, what failed, what "
            "you could not check; \"nichts\" when nothing)\n";
    text += kEnd;
    text += "\n\nNo absolute paths (name files relative to the repository), no secrets, no long output of tools: it is "
            "shown to the person, who decides whether it goes into the task.";
    return text;
}

}  // namespace ai_desktop::summary
