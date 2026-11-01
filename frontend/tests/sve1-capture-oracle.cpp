// Qualification-only protocol/SVE1 capture oracle for synthetic fixtures.
// Logs token/row hashes and dimensions, never prompt/source/credential bytes.
#include <openssl/evp.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool ok) { if(!ok) throw std::runtime_error("GPU probe invariant failed"); }
static std::string hash(const void * data,size_t bytes) {
    unsigned char out[32]; unsigned n=0; auto * ctx=EVP_MD_CTX_new(); require(ctx);
    require(EVP_DigestInit_ex(ctx,EVP_sha256(),nullptr)==1 && EVP_DigestUpdate(ctx,data,bytes)==1 && EVP_DigestFinal_ex(ctx,out,&n)==1 && n==32);
    EVP_MD_CTX_free(ctx); static const char hex[]="0123456789abcdef"; std::string s;
    for(auto b:out) { s+=hex[b>>4]; s+=hex[b&15]; } return s;
}
int main(int argc,char ** argv) {
    try {
        std::string directory;
        for(int i=1;i+1<argc;++i) if(std::string(argv[i])=="--fixture-dir") directory=argv[++i];
        require(!directory.empty()); std::cout << "READY 262144 stop\n" << std::flush;
        unsigned request=0; std::string line;
        while(std::getline(std::cin,line)) {
            if(line=="QUIT") return 0;
            if(line=="STOP") continue;
            std::istringstream input(line); std::vector<std::string> fields; std::string field;
            while(input>>field) fields.push_back(field);
            require(fields.size()>=3 && (fields[0]=="GEN" || fields[0]=="GENI")); ++request;
            auto & ids=fields.back(); size_t cells=1+std::count(ids.begin(),ids.end(),',');
            std::string path,keys; std::vector<char> rows;
            if(fields[0]=="GENI") {
                require(fields.size()>=4); path=fields[fields.size()-2];
                std::ifstream f(path,std::ios::binary|std::ios::ate); require(bool(f)); auto bytes=f.tellg();
                require(bytes>=0 && bytes<=64*1024*1024); rows.resize(size_t(bytes)); f.seekg(0); f.read(rows.data(),rows.size()); require(bool(f));
            }
            for(auto & value:fields) if(value.rfind("img_keys=",0)==0) keys=value.substr(9);
            size_t at=0,records=0,total=0; std::ostringstream shapes;
            while(at<rows.size()) {
                require(rows.size()-at>=20); uint32_t h[5]; std::memcpy(h,rows.data()+at,20); at+=20;
                require(h[0]==0x31455653 && h[1] && h[2] && h[3] && uint64_t(h[2])*h[3]==h[1] && h[4]);
                uint64_t bytes=uint64_t(h[1])*h[4]*4; require(bytes<=rows.size()-at);
                for(size_t p=at;p<at+bytes;p+=4) { float v; std::memcpy(&v,rows.data()+p,4); require(std::isfinite(v)); }
                if(records++) shapes << ',';
                shapes << '[' << h[1] << ',' << h[2] << ',' << h[3] << ',' << h[4] << ']'; total+=h[1]; at+=size_t(bytes);
            }
            std::ofstream rowfile(directory+"/"+std::to_string(request)+".rows",std::ios::binary); rowfile.write(rows.data(),rows.size()); require(bool(rowfile));
            std::ofstream result(directory+"/results",std::ios::app);
            result << "{\"request\":" << request << ",\"kind\":\"" << fields[0] << "\",\"tokens\":" << cells
                << ",\"token_sha\":\"" << hash(ids.data(),ids.size()) << "\",\"keys_sha\":\"" << hash(keys.data(),keys.size())
                << "\",\"row_sha\":\"" << hash(rows.data(),rows.size()) << "\",\"shapes\":[" << shapes.str() << "],\"image_cells\":" << total << "}\n";
            result.close(); require(bool(result));
            std::cout << "RESUME 0\nPP " << cells << ' ' << cells << " 1 1000\nT 9707\nDONE 1 " << cells << " 1 1 stop 0 0 0\n" << std::flush;
        }
        return 0;
    } catch(...) { std::cerr << "GPU qualification probe failed\n"; return 1; }
}
