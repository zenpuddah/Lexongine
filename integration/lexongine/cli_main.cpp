#include "cli.hpp"
#include <csignal>
#include <iostream>
int main(int argc,char** argv) {
    std::signal(SIGPIPE,SIG_IGN);
    if(argc<2||argc>static_cast<int>(lexongine::cli::max_arguments+1)){std::cerr<<"<diagnostic version=\"1\" code=\"InvalidConfig\" message=\"explicit bounded command required\"/>\n";return 2;}
    std::array<std::string_view,lexongine::cli::max_arguments> arguments{};
    for(int i=1;i<argc;++i){arguments[static_cast<std::size_t>(i-1)]=argv[i];}
    return lexongine::cli::dispatch({arguments.data(),static_cast<std::size_t>(argc-1)},LEXONGINE_HOST_SOURCE_BUILD,std::cout,std::cerr);
}
