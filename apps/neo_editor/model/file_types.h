#pragma once

#include "model/i18n.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace neo::filetypes {

enum class Category { Unknown, Markdown, Text, Code, Data };
struct Type { const char* extension; const char* language; Category category; };

// Recognition chooses presentation, never permission to read a file as text.
inline const std::vector<Type>& types() {
    static const std::vector<Type> value = {
        {"md","markdown",Category::Markdown},{"markdown","markdown",Category::Markdown},
        {"mdx","markdown",Category::Markdown},{"mkd","markdown",Category::Markdown},
        {"txt","text",Category::Text},{"text","text",Category::Text},
        {"log","text",Category::Text},{"csv","text",Category::Text},{"tsv","text",Category::Text},
        {"json","json",Category::Data},{"jsonc","jsonc",Category::Data},
        {"yaml","yaml",Category::Data},{"yml","yaml",Category::Data},
        {"toml","toml",Category::Data},{"ini","ini",Category::Data},
        {"cfg","ini",Category::Data},{"conf","ini",Category::Data},{"env","ini",Category::Data},
        {"xml","xml",Category::Data},{"svg","xml",Category::Data},
        {"py","python",Category::Code},{"pyw","python",Category::Code},
        {"js","javascript",Category::Code},{"jsx","javascript",Category::Code},
        {"mjs","javascript",Category::Code},{"cjs","javascript",Category::Code},
        {"ts","typescript",Category::Code},{"tsx","typescript",Category::Code},
        {"c","c",Category::Code},{"h","c",Category::Code},
        {"cpp","cpp",Category::Code},{"cc","cpp",Category::Code},{"cxx","cpp",Category::Code},
        {"hpp","cpp",Category::Code},{"hh","cpp",Category::Code},{"hxx","cpp",Category::Code},
        {"cs","cs",Category::Code},{"java","java",Category::Code},
        {"rs","rust",Category::Code},{"go","go",Category::Code},
        {"rb","ruby",Category::Code},{"lua","lua",Category::Code},{"sql","sql",Category::Code},
        {"html","html",Category::Code},{"htm","html",Category::Code},
        {"css","css",Category::Code},{"scss","css",Category::Code},
        {"sh","shell",Category::Code},{"bash","shell",Category::Code},
        {"bat","batch",Category::Code},{"cmd","batch",Category::Code},{"ps1","powershell",Category::Code},
        {"cmake","cmake",Category::Code},{"gradle","java",Category::Code},
        {"glsl","c",Category::Code},{"vert","c",Category::Code},{"frag","c",Category::Code},{"hlsl","c",Category::Code},
    };
    return value;
}
inline std::string lower(std::string value) {
    for (char& c : value) if(c>='A' && c<='Z') c=static_cast<char>(c+'a'-'A');
    return value;
}
inline Type detect(const std::string& path) {
    const auto slash=path.find_last_of("/\\");
    const std::string name=lower(path.substr(slash==std::string::npos?0:slash+1));
    if(name=="cmakelists.txt") return {"","cmake",Category::Code};
    if(name=="makefile" || name=="gnumakefile" || name=="dockerfile") return {"",name=="dockerfile"?"dockerfile":"makefile",Category::Code};
    if(name==".env" || name.rfind(".env.",0)==0 || name==".editorconfig") return {"","ini",Category::Data};
    if(name==".gitignore" || name==".gitattributes" || name==".gitmodules" || name=="hosts" ||
       name=="license" || name=="licence" || name=="readme" || name=="authors" || name=="notice") return {"","text",Category::Unknown};
    const auto dot=name.find_last_of('.');
    const std::string ext=dot==std::string::npos?"":name.substr(dot+1);
    for(const auto& type:types()) if(ext==type.extension) return type;
    return {"","text",Category::Unknown};
}
// Recognition is separate from presentation category: special extensionless/plain-text names
// remain visible in the vault while still using the neutral generic-file presentation.
inline bool known(const std::string& path) {
    const Type type=detect(path);
    if(type.category!=Category::Unknown) return true;
    const auto slash=path.find_last_of("/\\");
    const std::string name=lower(path.substr(slash==std::string::npos?0:slash+1));
    return name==".gitignore" || name==".gitattributes" || name==".gitmodules" || name=="hosts" ||
           name=="license" || name=="licence" || name=="readme" || name=="authors" || name=="notice";
}

// Association eligibility follows recognition. Registering an extension only adds EUI-Edits
// as an explicit Open With option; the user chooses which extensions to register.
inline bool associationEligible(const std::string& extension) {
    const std::string normalized=lower(extension);
    if(normalized.empty()) return false;
    return std::any_of(types().begin(),types().end(),[&](const Type& type) {
        return normalized==type.extension;
    });
}

inline const std::vector<Type>& associationTypes() {
    static const std::vector<Type> value=[] {
        std::vector<Type> result;
        for(const auto& type:types()) if(associationEligible(type.extension)) result.push_back(type);
        return result;
    }();
    return value;
}
inline std::vector<std::string> extensions() {
    std::vector<std::string> result;
    for(const auto& type:types()) result.emplace_back(type.extension);
    return result;
}
inline bool source(const std::string& language) { return language!="text" && language!="markdown"; }
inline const std::vector<std::string>& languages() {
    static const std::vector<std::string> value={"text","markdown","json","jsonc","python","javascript","typescript","yaml","toml","ini","xml","html","css","c","cpp","cs","java","rust","go","ruby","lua","sql","shell","powershell","batch","cmake","makefile","dockerfile"};
    return value;
}
inline bool validLanguage(const std::string& value) {
    const auto& values=languages();return std::find(values.begin(),values.end(),value)!=values.end();
}
inline std::string label(const std::string& language) {
    if(language=="text") return i18n::tr("type.text");
    if(language=="markdown") return "Markdown";
    if(language=="python") return "Python";
    if(language=="javascript") return "JavaScript";
    if(language=="typescript") return "TypeScript";
    if(language=="cpp") return "C++";
    if(language=="cs") return "C#";
    std::string result=language;for(char& c:result) if(c>='a'&&c<='z') c=static_cast<char>(c+'A'-'a');return result;
}
} // namespace neo::filetypes
