#include <journal/journal.hpp>
#include <fstream>
#include <iostream>
#include <vector>
void require(bool ok){if(!ok)throw std::runtime_error("Check failed");}
template<class F>void rejects(F f){bool thrown=false;try{f();}catch(const journal::error&){thrown=true;}require(thrown);}
int main(){
    const std::filesystem::path path="journal-test.bin";
    try {
        std::filesystem::remove(path);
        {journal::log log(path);require(log.append("first")==1);require(log.append(std::string("a\0b",3))==2);require(log.append("")==3);
         rejects([&]{journal::log second(path);});}
        {journal::log log(path);std::vector<std::string> rows;log.replay([&](auto seq,auto payload){require(seq==rows.size()+1);rows.emplace_back(payload);});
         require(rows.size()==3&&rows[1]==std::string("a\0b",3));require(log.append("last")==4);}
        const auto complete=std::filesystem::file_size(path);
        // Every partial byte boundary in the final frame must preserve the prior prefix.
        for(std::uint64_t keep=1;keep<28;++keep){
            std::filesystem::resize_file(path,complete-28);
            {journal::log log(path);log.append("last");}
            std::filesystem::resize_file(path,complete-28+keep);
            rejects([&]{journal::log log(path);});
            require(std::filesystem::file_size(path)==complete-28+keep);
            {journal::log log(path,journal::recovery::truncate_incomplete_tail);require(log.count()==3);require(log.repaired_bytes()==keep);require(log.append("last")==4);}
        }
        {std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);file.seekp(24);file.put('X');}
        rejects([&]{journal::log log(path,journal::recovery::truncate_incomplete_tail);});
        require(std::filesystem::file_size(path)==complete);
        std::filesystem::remove(path);
        {std::ofstream file(path,std::ios::binary);file<<"BAD";}
        rejects([&]{journal::log log(path,journal::recovery::truncate_incomplete_tail);});
        std::filesystem::remove(path);
        std::cout<<"Roundtrip, sequence, exclusive ownership, 27 torn-tail boundaries and corruption tests passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
