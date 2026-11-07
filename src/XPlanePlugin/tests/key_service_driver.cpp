#include "xp_key_bindings.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 2;
    fscs::xplane::keys::Service service{std::filesystem::path(argv[1]),std::filesystem::path(argv[2])};
    std::string line;
    while(std::getline(std::cin,line)) {
        if(line=="exit")break;
        std::cout<<service.request(line)<<std::endl;
    }
}
