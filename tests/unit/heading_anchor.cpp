#include "model/heading_anchor.h"
#include "model/link_target.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

int offsetOf(const std::string& text, const std::string& part) {
    const auto found = text.find(part);
    return found == std::string::npos ? -1 : static_cast<int>(found);
}

int targetOf(const neo::LpPlan& plan, const std::string& text, const std::string& url) {
    const auto parsed = neo::parseLocalHeadingLink(url);
    return parsed && parsed->hasFragment ? neo::findHeadingAnchor(plan, text, parsed->fragment) : -1;
}

} // namespace

int main() {
    const auto samePage = neo::parseLocalHeadingLink("#%E4%B8%AD%E6%96%87%20%E6%A0%87%E9%A2%98");
    check(samePage && samePage->path.empty() && samePage->hasFragment &&
              samePage->fragment == "中文 标题", "same-page percent-encoded Chinese heading");
    const auto crossPage = neo::parseLocalHeadingLink("notes/second%23draft.md#Hello-World");
    check(crossPage && crossPage->path == "notes/second#draft.md" && crossPage->hasFragment &&
              crossPage->fragment == "Hello-World", "cross-page target keeps encoded hash in filename");
    const auto pathOnly = neo::parseLocalHeadingLink("notes/second%23draft.md");
    check(pathOnly && pathOnly->path == "notes/second#draft.md" && !pathOnly->hasFragment,
          "path without literal hash has no fragment");
    check(!neo::parseLocalHeadingLink("a%2.md#title") &&
              !neo::parseLocalHeadingLink("a.md#%GG") &&
              !neo::parseLocalHeadingLink("a.md#%00") &&
              !neo::parseLocalHeadingLink("a%FF.md#title") &&
              !neo::parseLocalHeadingLink("a.md#%C0%AF") &&
              !neo::parseLocalHeadingLink("a.md#%ED%A0%80") &&
              !neo::parseLocalHeadingLink(std::string("a\0.md#title", 11)),
          "reject malformed percent escapes, NUL, invalid and overlong UTF-8");

    const std::string document =
        "# 中文 标题\n"
        "# Hello, **World**!\n"
        "# Hello, World!\n"
        "# Hello-World-1\n"
        "# Hello, World!\n"
        "Setext **格式**\n"
        "===\n"
        "```md\n"
        "# 隐藏标题\n"
        "```\n";
    const neo::LpPlan plan = neo::buildLpPlan(document);
    check(targetOf(plan, document, "#%E4%B8%AD%E6%96%87%20%E6%A0%87%E9%A2%98") ==
              offsetOf(document, "# 中文 标题"), "Chinese original heading navigates by source byte offset");
    check(targetOf(plan, document, "#中文 标题") == offsetOf(document, "# 中文 标题"),
          "raw UTF-8 Chinese title also navigates");
    check(targetOf(plan, document, "other.md#hello-world") ==
              offsetOf(document, "# Hello, **World**!"), "formatted heading uses concealed display text");
    check(targetOf(plan, document, "#HELLO-WORLD-1") ==
              offsetOf(document, "# Hello, World!"), "ASCII slug is case-insensitive and duplicate gets -1");
    check(targetOf(plan, document, "#hello-world-1-1") ==
              offsetOf(document, "# Hello-World-1"), "natural suffix avoids an earlier duplicate ID");
    check(targetOf(plan, document, "#hello-world-2") ==
              static_cast<int>(document.rfind("# Hello, World!")), "third duplicate gets unused -2 ID");
    check(targetOf(plan, document, "#setext-%E6%A0%BC%E5%BC%8F") ==
              offsetOf(document, "Setext **格式**"), "setext heading resolves to text line, not underline");
    check(targetOf(plan, document, "#%E9%9A%90%E8%97%8F%E6%A0%87%E9%A2%98") == -1,
          "fenced pseudo heading does not navigate");
    check(targetOf(plan, document, "#missing") == -1, "unknown heading remains unresolved");

    const std::string links =
        "[jump](notes/second%23draft.md#Hello-World)\n"
        "[[other.md#中文 标题|别名]]\n"
        "[[#中文 标题]]\n";
    const neo::LpPlan linkPlan = neo::buildLpPlan(links);
    const neo::LinkTarget markdown = neo::linkTargetAt(linkPlan, links, offsetOf(links, "jump"));
    check(markdown.valid && !markdown.wikilink && markdown.url == "notes/second%23draft.md#Hello-World" &&
              markdown.navigationUrl == markdown.url, "Markdown navigation keeps encoded filename and fragment");
    const neo::LinkTarget wikiAlias = neo::linkTargetAt(linkPlan, links, offsetOf(links, "别名"));
    check(wikiAlias.valid && wikiAlias.wikilink && wikiAlias.url == "other.md" &&
              wikiAlias.navigationUrl == "other.md#中文 标题" &&
              links.substr(static_cast<std::size_t>(wikiAlias.urlBeg),
                           static_cast<std::size_t>(wikiAlias.urlEnd - wikiAlias.urlBeg)) == wikiAlias.url,
          "Wiki edit URL is path only; navigation keeps section and drops alias");
    const neo::LinkTarget wikiSamePage = neo::linkTargetAt(linkPlan, links,
        offsetOf(links, "[[#中文 标题]]") + 2);
    check(wikiSamePage.valid && wikiSamePage.wikilink && wikiSamePage.url.empty() &&
              wikiSamePage.navigationUrl == "#中文 标题", "Wiki same-page section has empty edit path");

    // Markdown destinations are also the right-click edit range. A title and
    // the optional angle brackets belong to the surrounding source, not URL.
    const auto checkDestination = [&](const std::string& source, const std::string& label,
                                      const std::string& destination, const char* message) {
        const neo::LpPlan sourcePlan = neo::buildLpPlan(source);
        const neo::LinkTarget link = neo::linkTargetAt(sourcePlan, source, offsetOf(source, label));
        const bool validRange = link.urlBeg >= 0 && link.urlEnd >= link.urlBeg &&
            link.urlEnd <= static_cast<int>(source.size());
        check(link.valid && !link.wikilink && link.url == destination &&
                  link.navigationUrl == destination && validRange &&
                  source.substr(static_cast<std::size_t>(link.urlBeg),
                                static_cast<std::size_t>(link.urlEnd - link.urlBeg)) == destination,
              message);
        return link;
    };
    checkDestination("[double](file.md#title \"tooltip\")\n", "double", "file.md#title",
                     "double-quoted title is outside navigation and edit destination");
    checkDestination("[single](file.md#title 'tooltip')\n", "single", "file.md#title",
                     "single-quoted title is outside navigation and edit destination");
    checkDestination("[paren](file.md#title (tooltip))\n", "paren", "file.md#title",
                     "parenthesized title is outside navigation and edit destination");
    checkDestination("[spaces](  file.md#title  \"tooltip\"  )\n", "spaces", "file.md#title",
                     "surrounding whitespace is outside navigation and edit destination");
    checkDestination("[balanced](file(name).md#title \"tooltip\")\n", "balanced",
                     "file(name).md#title", "balanced parentheses stay in destination");
    const std::string angleSource = "[angle](<file name.md#title> \"tooltip\")\n";
    const neo::LinkTarget angle = checkDestination(angleSource, "angle", "file name.md#title",
        "angle destination keeps its spaces but excludes brackets and title");
    if (angle.valid && angle.urlBeg >= 0 && angle.urlEnd >= angle.urlBeg &&
        angle.urlEnd <= static_cast<int>(angleSource.size())) {
        std::string edited = angleSource;
        edited.replace(static_cast<std::size_t>(angle.urlBeg),
                       static_cast<std::size_t>(angle.urlEnd - angle.urlBeg), "next.md#other");
        check(edited == "[angle](<next.md#other> \"tooltip\")\n",
              "editing the destination preserves angle brackets and title");
    }
    checkDestination("[empty]()\n", "empty", "", "empty Markdown destination remains empty");
    checkDestination("[empty-angle](<> \"tooltip\")\n", "empty-angle", "",
                     "empty angle destination excludes its title");
    checkDestination("![image](<image name.png> 'caption')\n", "image", "image name.png",
                     "image destination uses the same bounded edit range");
    checkDestination("[newline](file.md#title\n\"tooltip\")\n", "newline", "file.md#title",
                     "title on the next line is outside the destination");
    checkDestination("[escaped](<file\\>name.md> \"tooltip\")\n", "escaped", "file\\>name.md",
                     "escaped angle bracket stays inside the source edit range");

    // Keep the already supported autolink extraction separate from the
    // parenthesized Markdown destination rules.
    checkDestination("<https://example.test/a#frag>\n", "https://", "https://example.test/a#frag",
                     "autolink navigation keeps its complete external URL");

    if (failures == 0) std::cout << "heading_anchor passed\n";
    return failures == 0 ? 0 : 1;
}
