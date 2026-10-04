#pragma once
#include "components/input_model.h"
#include "model/lp_syntax.h"
#include "model/style_schema.h"

namespace neo::source {
using namespace components::input_detail;
inline bool sameState(const lp::SyntaxState& a,const lp::SyntaxState& b) {
    return a.blockComment==b.blockComment && a.tripleString==b.tripleString && a.tripleChar==b.tripleChar;
}
struct Row {
    std::string text;
    lp::SyntaxState in, out;
    std::vector<lp::SyntaxToken> tokens;
    bool ready=false;
};
struct Cache {
    std::vector<Row> rows;
    std::vector<int> starts;
    LineDecorationSnapshot snapshot;
    std::string language;
    unsigned long long revision=0, palette=0;
    std::size_t bytes=0;
    std::size_t tokenizedRows=0;
};
inline Cache& cache() {static Cache value;return value;}
inline void invalidate() {cache()=Cache{};}
inline core::Color tokenColor(lp::TokenKind kind,const EditorColors& colors) {
    switch(kind) {
    case lp::TokenKind::Comment:return colors.tokenComment;
    case lp::TokenKind::Keyword:return colors.tokenKeyword;
    case lp::TokenKind::String:return colors.tokenString;
    case lp::TokenKind::Number:return colors.tokenNumber;
    case lp::TokenKind::Operator:return colors.tokenOperator;
    case lp::TokenKind::Function:return colors.tokenFunction;
    case lp::TokenKind::Property:return colors.tokenProperty;
    default:return colors.text;
    }
}
inline LineDecorationSnapshot build(Cache& c,const std::string& text,const std::string& language,
    const EditorColors& colors,unsigned long long palette,const DecoratorEditInfo& info) {
    c.tokenizedRows=0;
    if(c.snapshot && info.committed && c.revision==info.textRevision && c.bytes==text.size() &&
       c.language==language && c.palette==palette) return c.snapshot;
    const bool sameLanguage=c.language==language;
    const auto* e=info.edit;
    const int delta=e?e->newEnd-e->oldEnd:0;
    bool incremental=c.snapshot && sameLanguage && info.committed && e && e->valid &&
        e->revision==info.textRevision && c.revision+1==info.textRevision &&
        static_cast<long long>(c.bytes)+delta==static_cast<long long>(text.size()) &&
        e->firstLine>=0 && e->firstLine<static_cast<int>(c.rows.size()) &&
        e->oldTailLine>=e->firstLine && e->oldTailLine<=static_cast<int>(c.rows.size()) &&
        e->newLastLine>=e->firstLine;
    std::size_t first=0;
    if(incremental) {
        first=static_cast<std::size_t>(e->firstLine);
        int at=c.starts[first];
        const int count=e->newLastLine-e->firstLine+1;
        std::vector<Row> replacement;
        for(int i=0;i<count && at<=static_cast<int>(text.size());++i) {
            const auto end=text.find('\n',static_cast<std::size_t>(at));
            const int stop=end==std::string::npos?static_cast<int>(text.size()):static_cast<int>(end);
            Row row;row.text=text.substr(at,stop-at);replacement.push_back(std::move(row));at=stop+1;
        }
        const int expected=e->oldTailLine<static_cast<int>(c.starts.size())
            ?c.starts[static_cast<std::size_t>(e->oldTailLine)]+delta:static_cast<int>(text.size())+1;
        if(static_cast<int>(replacement.size())!=count || at!=expected) incremental=false;
        else {
            c.rows.erase(c.rows.begin()+e->firstLine,c.rows.begin()+e->oldTailLine);
            c.rows.insert(c.rows.begin()+e->firstLine,std::make_move_iterator(replacement.begin()),std::make_move_iterator(replacement.end()));
        }
    }
    if(!incremental) {
        c.rows.clear();first=0;
        std::size_t at=0;
        do {const auto end=text.find('\n',at);Row row;row.text=text.substr(at,end==std::string::npos?text.size()-at:end-at);
            c.rows.push_back(std::move(row));if(end==std::string::npos) break;at=end+1;} while(at<=text.size());
    }
    auto state=first?c.rows[first-1].out:lp::SyntaxState{};
    for(std::size_t i=first;i<c.rows.size();++i) {
        auto& row=c.rows[i];
        // Once the outgoing edit state converges, the untouched suffix stays valid.
        if(row.ready && sameState(row.in,state)) break;
        row.in=state;row.tokens.clear();lp::tokenizeCodeLine(row.text,0,static_cast<int>(row.text.size()),language,state,row.tokens);
        row.out=state;row.ready=true;++c.tokenizedRows;
    }
    c.starts.resize(c.rows.size());int at=0;
    std::vector<LineDecoration> full;
    std::vector<int> changed;
    std::vector<LineDecoration> patches;
    const bool paged=c.snapshot && c.snapshot->size()==c.rows.size();
    if(!paged) full.reserve(c.rows.size());
    for(std::size_t i=0;i<c.rows.size();++i) {
        c.starts[i]=at;LineDecoration d;
        for(const auto& token:c.rows[i].tokens) {
            if(token.kind==lp::TokenKind::Plain) continue;
            LineRun run;run.beg=at+token.beg;run.end=at+token.end;run.style.color=tokenColor(token.kind,colors);d.runs.push_back(std::move(run));
        }
        if(paged) {if(d!=(*c.snapshot)[i]) {changed.push_back(static_cast<int>(i));patches.push_back(std::move(d));}}
        else full.push_back(std::move(d));
        at+=static_cast<int>(c.rows[i].text.size())+1;
    }
    if(paged) {if(!changed.empty()) c.snapshot=c.snapshot->replacing(changed,std::move(patches));}
    else c.snapshot=std::make_shared<const LineDecorationTable>(std::move(full));
    c.language=language;c.palette=palette;c.revision=info.textRevision;c.bytes=text.size();return c.snapshot;
}
inline LineDecorationSnapshot snapshot(const std::string& text,const std::string& language,
    const EditorColors& colors,unsigned long long palette,const DecoratorEditInfo& info) {
    // IME preedit owns a separate generation and never advances the committed cache.
    if(!info.committed) {Cache transient;return build(transient,text,language,colors,palette,info);}
    return build(cache(),text,language,colors,palette,info);
}
} // namespace neo::source
