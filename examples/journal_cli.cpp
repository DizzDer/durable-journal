#include <journal/journal.hpp>
#include <iostream>
int main(int argc,char** argv) {
    if(argc<3) { std::cerr<<"Usage: journal_cli FILE append TEXT | replay | repair\n"; return 2; }
    try {
        const std::string command=argv[2];
        if(command!="append" && command!="replay" && command!="repair") throw journal::error("Unknown command");
        if((command=="append" && argc!=4)||(command!="append" && argc!=3)) throw journal::error("Invalid arguments");
        journal::log log(argv[1],command=="repair"?journal::recovery::truncate_incomplete_tail:journal::recovery::reject_incomplete_tail);
        if(command=="append") std::cout<<log.append(argv[3])<<'\n';
        else if(command=="repair") std::cout<<"Recovered "<<log.count()<<" records; removed "<<log.repaired_bytes()<<" tail bytes\n";
        else log.replay([](std::uint64_t seq,std::string_view text){std::cout<<seq<<"\t"<<text<<'\n';});
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
