#include "ue_px4/terrain_image.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <cstdio>
#include <jpeglib.h>
#include <csetjmp>
#include <dlfcn.h>
#include <link.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
#include <limits>

namespace dynamic_terrain {
namespace {
struct JpegLibrary {
    // Isolate only libjpeg's C ABI. Isolating all OpenCV/GStreamer dependencies
    // would interfere with their process-wide allocators and plugin registries.
    void* handle=dlmopen(LM_ID_NEWLM,XOVIUM_JPEG_LIBRARY,RTLD_NOW|RTLD_LOCAL);
    decltype(&jpeg_std_error) error=nullptr;
    decltype(&jpeg_CreateDecompress) create=nullptr;
    decltype(&jpeg_mem_src) source=nullptr;
    decltype(&jpeg_read_header) header=nullptr;
    decltype(&jpeg_start_decompress) start=nullptr;
    decltype(&jpeg_read_scanlines) read=nullptr;
    decltype(&jpeg_finish_decompress) finish=nullptr;
    decltype(&jpeg_destroy_decompress) destroy=nullptr;
    bool ready=false;
    template<typename T> bool bind(T& fn,const char* name) {
        fn=reinterpret_cast<T>(dlsym(handle,name));return fn!=nullptr;
    }
    JpegLibrary() {
        if(handle) ready=bind(error,"jpeg_std_error") && bind(create,"jpeg_CreateDecompress") &&
            bind(source,"jpeg_mem_src") && bind(header,"jpeg_read_header") &&
            bind(start,"jpeg_start_decompress") && bind(read,"jpeg_read_scanlines") &&
            bind(finish,"jpeg_finish_decompress") && bind(destroy,"jpeg_destroy_decompress");
        if(!ready) std::fprintf(stderr,"[DynamicTerrain][JPEG] Cannot load private decoder: %s\n",dlerror());
    }
    // A single process-lifetime namespace; no callbacks survive in unloaded code.
};
struct JpegError {jpeg_error_mgr manager;std::jmp_buf jump;};
struct Decode {
    jpeg_decompress_struct info{};
    JpegError error{};
    std::vector<unsigned char> pixels;
};
void jpegError(j_common_ptr common) {
    auto* error=reinterpret_cast<JpegError*>(common->err);
    std::longjmp(error->jump,1);
}
void jpegMessage(j_common_ptr common,int level) {
    // A warning can mean reconstructed pixels from missing entropy/EOI bytes.
    // Reject the download instead of caching a visually degraded partial image.
    if(level<0) jpegError(common);
}
}
cv::Mat loadTerrainImage(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file) return {};
    std::vector<unsigned char> encoded((std::istreambuf_iterator<char>(file)),{});
    return loadTerrainImageBytes(encoded.data(),encoded.size());
}
cv::Mat loadTerrainImageBytes(const unsigned char* data,std::size_t size) {
    if(!data||size<2) return {};
    if(data[0]!=0xff||data[1]!=0xd8) {
        if(size>static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
        return cv::imdecode(cv::Mat(1,static_cast<int>(size),CV_8UC1,const_cast<unsigned char*>(data)),cv::IMREAD_COLOR);
    }
    static JpegLibrary library;
    if(!library.ready) return {};
    // Decoder state is on the heap so a libjpeg error cannot bypass destructors
    // or make modified stack variables indeterminate when it longjmps back.
    auto state=std::make_unique<Decode>();
    state->info.err=library.error(&state->error.manager);
    state->error.manager.error_exit=jpegError;
    state->error.manager.emit_message=jpegMessage;
    if(setjmp(state->error.jump)) {
        char message[JMSG_LENGTH_MAX]{};
        state->error.manager.format_message(reinterpret_cast<j_common_ptr>(&state->info),message);
        std::fprintf(stderr,"[DynamicTerrain][JPEG] Decode failed: %s: %s\n","encoded imagery",message);
        if(state->info.mem) library.destroy(&state->info);
        return {};
    }
    library.create(&state->info,JPEG_LIB_VERSION,sizeof(state->info));
    library.source(&state->info,data,size);
    library.header(&state->info,TRUE);
    state->info.out_color_space=JCS_RGB;
    if(!state->info.image_width || !state->info.image_height || state->info.image_width>8192 || state->info.image_height>8192) {
        library.destroy(&state->info);return {};
    }
    library.start(&state->info);
    const int width=int(state->info.output_width),height=int(state->info.output_height);
    if(state->info.output_components!=3) {library.destroy(&state->info);return {};}
    state->pixels.resize(size_t(width)*height*3);
    while(state->info.output_scanline<state->info.output_height) {
        unsigned char* row=state->pixels.data()+size_t(state->info.output_scanline)*width*3;
        if(library.read(&state->info,&row,1)!=1) {library.destroy(&state->info);return {};}
    }
    library.finish(&state->info);library.destroy(&state->info);
    cv::Mat output;
    cv::cvtColor(cv::Mat(height,width,CV_8UC3,state->pixels.data()),output,cv::COLOR_RGB2BGR);
    return output;
}
}
