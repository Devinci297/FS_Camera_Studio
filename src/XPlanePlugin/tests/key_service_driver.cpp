#include "xp_key_bindings.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    if(argc!=3 && argc!=4)return 2;
    const auto title=argc==4?std::filesystem::path(argv[3]).u8string():std::u8string();
    fscs::xplane::keys::Service service{std::filesystem::path(argv[1]),std::filesystem::path(argv[2]),[title]{return std::string(title.begin(),title.end());}};
    std::string line;
    while(std::getline(std::cin,line)) {
        if(line=="exit")break;
        std::cout<<service.request(line)<<std::endl;
    }
}
