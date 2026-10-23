#include "ue_px4/terrain_image.hpp"
#include <opencv2/imgcodecs.hpp>
#include <cassert>
#include <vector>
int main() {
    cv::Mat input(64,64,CV_8UC3);
    for(int y=0;y<input.rows;++y) for(int x=0;x<input.cols;++x)
        input.at<cv::Vec3b>(y,x)=cv::Vec3b((x*19+y*13)%256,(x*3+y*47)%256,(x*37+y*11)%256);
    std::vector<unsigned char> jpeg;
    assert(cv::imencode(".jpg",input,jpeg));
    const auto expected=cv::imdecode(jpeg,cv::IMREAD_COLOR);
    const auto actual=dynamic_terrain::loadTerrainImageBytes(jpeg.data(),jpeg.size());
    assert(!actual.empty()&&actual.size()==expected.size());
    assert(cv::norm(actual,expected,cv::NORM_INF)==0);
    assert(dynamic_terrain::loadTerrainImageBytes(jpeg.data(),jpeg.size()-2).empty());
    std::vector<unsigned char> partial(jpeg.begin(),jpeg.begin()+jpeg.size()*2/3);
    partial.push_back(0xff);partial.push_back(0xd9);
    assert(dynamic_terrain::loadTerrainImageBytes(partial.data(),partial.size()).empty());
    const unsigned char invalid[]={0xff,0xd8,0xff,0xd9};
    assert(dynamic_terrain::loadTerrainImageBytes(invalid,sizeof(invalid)).empty());
    std::vector<unsigned char> png;
    assert(cv::imencode(".png",input,png));
    assert(cv::norm(input,dynamic_terrain::loadTerrainImageBytes(png.data(),png.size()),cv::NORM_INF)==0);
}
