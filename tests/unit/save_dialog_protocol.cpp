#include "../../apps/neo_editor/platform/save_dialog_protocol.h"
#include <iostream>

int main() {
    namespace wire = neo::dialogs::wire;
    wire::Request original;
    original.owner = 0x123456789abcdef0ull;
    original.fields = {u"D:/中文 空格/目录", u"新文稿.md", {}, u"md"};
    original.fields[2] = u"文档 (*.md)"; original.fields[2].push_back(0);
    original.fields[2] += u"*.md"; original.fields[2].push_back(0); original.fields[2].push_back(0);
    std::string packet;
    wire::Request decoded;
    if (!wire::encode(original,packet) || !wire::decode(packet,decoded) || decoded.owner != original.owner || decoded.fields != original.fields) return 1;
    // Truncated pipe data must never become a partial chosen path/request.
    for (std::size_t n = 0; n < packet.size(); ++n) if (wire::decode(packet.substr(0,n),decoded)) return 2;
    if (wire::decode(packet + "x",decoded)) return 3;
    std::string bad = packet; bad[0] ^= 0x40;
    if (wire::decode(bad,decoded)) return 4;
    auto invalid = original; invalid.fields[0].push_back(0);
    if (!wire::encode(invalid,bad) || wire::decode(bad,decoded)) return 5;
    invalid = original; invalid.fields[2].pop_back();
    if (!wire::encode(invalid,bad) || wire::decode(bad,decoded)) return 6;
    invalid = original; invalid.fields[1].assign(wire::kMaximumPacket,u'X');
    if (wire::encode(invalid,bad) || wire::decode(std::string(wire::kMaximumPacket+1,'X'),decoded)) return 7;
    unsigned status = 99; std::uint32_t error = 99; std::u16string path;
    for (unsigned expected=0;expected<3;++expected) {
        const std::u16string selected=expected==1?u"D:/保存 空格/😀.txt":u"";
        if (!wire::encodeResult(expected,expected==2?123:0,selected,packet) ||
            !wire::decodeResult(packet,status,error,path) || status!=expected || path!=selected || error!=(expected==2?123:0)) return 8;
        for (std::size_t n=0;n<packet.size();++n) if(wire::decodeResult(packet.substr(0,n),status,error,path)) return 9;
        if(wire::decodeResult(packet+"x",status,error,path)) return 10;
    }
    if(wire::encodeResult(3,0,u"",packet) || wire::encodeResult(1,0,u"",packet) || wire::encodeResult(0,0,u"unexpected.txt",packet)) return 11;
    std::cout << "Save picker bounded Unicode protocol: passed\n";
}
