#ifndef _array_2d_hpp_
#define _array_2d_hpp_

#include "gdal_priv.h"
#include <vector>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cassert>
#include <algorithm>
#include <typeinfo>
#include <stdexcept>

GDALDataType peekGDALType(const std::string &filename) {
  GDALAllRegister();
  GDALDataset *fin = (GDALDataset*)GDALOpen(filename.c_str(), GA_ReadOnly);
  assert(fin!=NULL);

  GDALRasterBand *band   = fin->GetRasterBand(1);
  GDALDataType data_type = band->GetRasterDataType();

  GDALClose(fin);

  return data_type;
}


template<class T>
void getGDALHeader(
  const std::string &filename,
  int    &height,
  int    &width,
  T      &no_data,
  double *geotrans
){
  GDALAllRegister();
  GDALDataset *fin = (GDALDataset*)GDALOpen(filename.c_str(), GA_ReadOnly);
  assert(fin!=NULL);

  GDALRasterBand *band   = fin->GetRasterBand(1);

  height  = band->GetYSize();
  no_data = band->GetNoDataValue();
  width   = band->GetXSize();

  fin->GetGeoTransform(geotrans);

  GDALClose(fin);
}

int getGDALDimensions(
  const std::string &filename,
  int &height,
  int &width,
  GDALDataType &dtype,
  double geotransform[6]
){
  GDALAllRegister();
  GDALDataset *fin = (GDALDataset*)GDALOpen(filename.c_str(), GA_ReadOnly);
  assert(fin!=NULL);

  GDALRasterBand *band = fin->GetRasterBand(1);
  
  dtype = band->GetRasterDataType();

  if(geotransform!=NULL && fin->GetGeoTransform(geotransform)!=CE_None){
    std::cerr<<"Error getting geotransform from '"<<filename<<"'!"<<std::endl;
    return -1; //TODO: Set error code
  }

  height  = band->GetYSize();
  width   = band->GetXSize();

  GDALClose(fin);

  return 0;
}


template<class T>
GDALDataType NativeTypeToGDAL() {
  if(typeid(T)==typeid(uint8_t))
    return GDT_Byte;
  else if(typeid(T)==typeid(uint16_t))
    return GDT_UInt16;
  else if(typeid(T)==typeid(int16_t))
    return GDT_Int16;
  else if(typeid(T)==typeid(uint32_t))
    return GDT_UInt32;
  else if(typeid(T)==typeid(int32_t))
    return GDT_Int32;
  else if(typeid(T)==typeid(float))
    return GDT_Float32;
  else if(typeid(T)==typeid(double))
    return GDT_Float64;
  else{
    std::cerr<<"Unknown native->GDAL data type conversion."<<std::endl;
    assert(false);
  }
  return GDT_Unknown;
}


template<class T>
class Array2D {
 public:
  std::string filename;
  std::string basename;
  std::vector<double> geotransform;
  std::string projection;

 private:
  template<typename> friend class Array2D;

  std::vector<T> data;

  GDALDataType data_type;

  static const int HEADER_SIZE = 7*sizeof(int) + sizeof(T);

  int total_height;
  int total_width;
  int view_height;
  int view_width;
  int view_xoff;
  int view_yoff;
  int num_data_cells = -1;

  T   no_data;

  void loadGDAL(const std::string &filename, int xOffset=0, int yOffset=0, int part_width=0, int part_height=0){
    assert(empty());
    assert(xOffset>=0);
    assert(yOffset>=0);

    GDALDataset *fin = (GDALDataset*)GDALOpen(filename.c_str(), GA_ReadOnly);
    if(fin==NULL){
      std::cerr<<"Could not open file '"<<filename<<"'!"<<std::endl;
      throw std::runtime_error("Could not open a GDAL file!");
    }

    geotransform.resize(6);
    if(fin->GetGeoTransform(geotransform.data())!=CE_None){
      std::cerr<<"Error getting geotransform from '"<<filename<<"'!"<<std::endl;
      throw std::runtime_error("Error getting geotransform!");
    }

    const char* projection_string=fin->GetProjectionRef();
    projection = std::string(projection_string);

    GDALRasterBand *band = fin->GetRasterBand(1);
    data_type            = band->GetRasterDataType();


    total_width  = band->GetXSize();
    total_height = band->GetYSize();
    no_data      = band->GetNoDataValue();

    if(xOffset+part_width>=total_width)
      part_width  = total_width-xOffset;
    if(yOffset+part_height>=total_height)
      part_height = total_height-yOffset;

    if(part_width==0)
      part_width = total_width;
    view_width = part_width;

    if(part_height==0)
      part_height = total_height;
    view_height = part_height;

    view_xoff = xOffset;
    view_yoff = yOffset;

    std::cerr<<"Allocating: "<<view_height<<" rows by "<<view_width<<" columns"<<std::endl;
    resize(view_width,view_height);
    for(int y=yOffset;y<yOffset+view_height;y++){
      auto temp = band->RasterIO( GF_Read, xOffset, y, view_width, 1, data.data()+(y-yOffset)*view_width, view_width, 1, data_type, 0, 0 );
      if(temp!=CE_None)
        throw std::runtime_error("Error reading file with GDAL!");
    }

    GDALClose(fin);
  }

  GDALDataType myGDALType() const {
    return NativeTypeToGDAL<T>();
  }

 public:
  Array2D(){
    GDALAllRegister();
    total_height = 0;
    total_width  = 0;
    view_width   = 0;
    view_height  = 0;
    view_xoff    = 0;
    view_yoff    = 0;
  }

  //Create an internal array
  Array2D(int width, int height, const T& val = T()) : Array2D() {
    resize(width,height,val);
  }

  //Create internal array from a file
  Array2D(const std::string &filename, bool native, int xOffset=0, int yOffset=0, int part_width=0, int part_height=0) : Array2D() {
    if(native)
      loadNative(filename);
    else
      loadGDAL(filename, xOffset, yOffset, part_width, part_height);
  }

  void saveNative(const std::string &filename){
    std::fstream fout;

    fout.open(filename, std::ios_base::binary | std::ios_base::out | std::ios::trunc);
    if(!fout.good()){
      std::cerr<<"Failed to open file '"<<filename<<"'."<<std::endl;
      assert(fout.good());
    }

    fout.write(reinterpret_cast<char*>(&total_height),   sizeof(int));
    fout.write(reinterpret_cast<char*>(&total_width),    sizeof(int));
    fout.write(reinterpret_cast<char*>(&view_height),    sizeof(int));
    fout.write(reinterpret_cast<char*>(&view_width),     sizeof(int));
    fout.write(reinterpret_cast<char*>(&view_xoff),      sizeof(int));
    fout.write(reinterpret_cast<char*>(&view_yoff),      sizeof(int));
    fout.write(reinterpret_cast<char*>(&num_data_cells), sizeof(int));
    fout.write(reinterpret_cast<char*>(&no_data),        sizeof(T  ));

    for(int y=0;y<view_height;y++)
      fout.write(reinterpret_cast<char*>(data.data()+y*view_width), view_width*sizeof(T));
  }

  void loadNative(const std::string &filename){
    std::ifstream fin(filename, std::ios::in | std::ios::binary);
    assert(fin.good());

    fin.read(reinterpret_cast<char*>(&total_height),   sizeof(int));
    fin.read(reinterpret_cast<char*>(&total_width),    sizeof(int));
    fin.read(reinterpret_cast<char*>(&view_height),    sizeof(int));
    fin.read(reinterpret_cast<char*>(&view_width),     sizeof(int));
    fin.read(reinterpret_cast<char*>(&view_xoff),      sizeof(int));
    fin.read(reinterpret_cast<char*>(&view_yoff),      sizeof(int));
    fin.read(reinterpret_cast<char*>(&num_data_cells), sizeof(int));
    fin.read(reinterpret_cast<char*>(&no_data),        sizeof(T  ));

    resize(view_width,view_height);

    for(int y=0;y<view_height;y++)
      fin.read(reinterpret_cast<char*>(data.data()+y*view_width), view_width*sizeof(T));
  }

  //Note: The following functions return signed integers, which make them
  //generally easier to work with. If your DEM has a dimension which exceeds
  //2147483647, some other modifications to this program will probably be
  //necessary.
  int  viewSize   () const { return view_width*view_height; }
  int  totalWidth () const { return total_width;    }
  int  totalHeight() const { return total_height;   }
  int  viewWidth  () const { return view_width;     }
  int  viewHeight () const { return view_height;    }
  int  viewXoff   () const { return view_xoff;      }
  int  viewYoff   () const { return view_yoff;      }
  bool empty      () const { return data.empty();   }
  T    noData     () const { return no_data;        }

  void iToxy(const int i, int &x, int &y) const {
    x = i%view_width;
    y = i/view_width;
  }

  int xyToI(int x, int y) const {
    return y*view_width+x;
  }

  int nToI(int i, int dx, int dy) const {
    int x=i%view_width+dx;
    int y=i/view_width+dy;
    if(x<0 || y<0 || x==view_width || y==view_height)
      return -1;
    return xyToI(x,y);
  }

  template<class U>
  T& operator=(const Array2D<U> &o){
    data = std::vector<T>(o.data.begin(),o.data.end());
    total_height   = o.total_height;
    total_width    = o.total_width;
    view_height    = o.view_height;
    view_width     = o.view_width;
    view_xoff      = o.view_xoff;
    view_yoff      = o.view_yoff;
    num_data_cells = o.num_data_cells;
    geotransform   = o.geotransform;
    no_data        = (T)o.no_data;
    return *this;
  }

  bool operator==(const Array2D<T> &o){
    if(viewWidth()!=o.viewWidth() || viewHeight()!=o.viewHeight())
      return false;
    if(noData()!=o.noData())
      return false;
    for(int i=0;i<view_width*view_height;i++)
      if(data[i]!=o.data[i])
        return false;
    return true;
  }

  bool isNoData(int x, int y) const {
    return data[xyToI(x,y)]==no_data;
  }

  // void flipVert(){
  //   std::reverse(data.begin(),data.end());
  // }

  // void flipHorz(){
  //   for(auto &row: data)
  //     std::reverse(row.begin(),row.end());
  // }

  bool in_grid(int x, int y) const {
    return 0<=x && x<view_width && 0<=y && y<view_height;
  }

  void setNoData(const T &ndval){
    no_data = ndval;
  }

  void setAll(const T &val){
    std::fill(data.begin(),data.end(),val);
  }

  void init(T val){
    setAll(val);
  }

  //Destructively resizes the array. All data will die!
  void resize(int width, int height, const T& val = T()){
    data.clear();
    data.resize(width*height,val);
    total_height = view_height = height;
    total_width  = view_width  = width;
  }

  template<class U>
  void resize(const Array2D<U> &other, const T& val = T()){
    resize(other.viewWidth(), other.viewHeight(), val);
    geotransform = other.geotransform;
  }

  void countDataCells(){
    num_data_cells = 0;
    for(auto &i: data)
      if(i!=no_data)
        num_data_cells++;
  }

  int numDataCells(){
    if(num_data_cells==-1)
      countDataCells();
    return num_data_cells;
  }

  int numDataCells() const {
    return num_data_cells;
  }

  T& operator()(int i){
    assert(i>=0);
    assert(i<view_width*view_height);
    return data[i];
  }

  T operator()(int i) const {
    assert(i>=0);
    assert(i<view_width*view_height);
    return data[i];    
  }

  T& operator()(int x, int y){
    assert(x>=0);
    assert(y>=0);
    //std::cerr<<"Width: "<<viewWidth()<<" Height: "<<viewHeight()<<" x: "<<x<<" y: "<<y<<std::endl;
    assert(x<view_width);
    assert(y<view_height);
    return data[xyToI(x,y)];
  }

  const T& operator()(int x, int y) const {
    assert(x>=0);
    assert(y>=0);
    assert(x<view_width);
    assert(y<view_height);
    return data[xyToI(x,y)];
  }

  //Row&       topRow   ()       { return data.front(); }
  //Row&       bottomRow()       { return data.back (); }
  //const Row& topRow   () const { return data.front(); }
  //const Row& bottomRow() const { return data.back (); }

  // Row leftColumn() const {
  //   Row temp(data.size());
  //   for(size_t y=0;y<data.size();y++)
  //     temp[y] = data[y][0];
  //   return temp;
  // }

  // Row rightColumn() const {
  //   Row temp(data.size());
  //   size_t right = data[0].size()-1;
  //   for(size_t y=0;y<data.size();y++)
  //     temp[y] = data[y][right];
  //   return temp;
  // }

  // void emplaceRow(Row &row){
  //   data.emplace_back(row);
  // }

  // Row& rowRef(int rownum){
  //   return data[rownum];
  // }

  void setRow(int y, const T &val){
    for(int x=0;x<view_width;x++)
      data[y*view_width+x] = val;
  }

  void setCol(int x, const T &val){
    for(int y=0;y<view_height;y++)
      data[y*view_width+x] = val;
  }

  // const std::vector<T>& getRowData(int rownum){
  //   return data[rownum].data();
  // }

  void clear(){
    data.clear();
    data.shrink_to_fit();
  }

  void saveGDAL(const std::string &filename, int xoffset, int yoffset){
    GDALDriver *poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if(poDriver==NULL){
      std::cerr<<"Could not load GTiff driver!"<<std::endl;
      throw std::runtime_error("Could not load GTiff driver!");
    }
    GDALDataset *fout    = poDriver->Create(filename.c_str(), viewWidth(), viewHeight(), 1, myGDALType(), NULL);
    if(fout==NULL){
      std::cerr<<"Could not open output file!"<<std::endl;
      throw std::runtime_error("Could not open output file!");
    }

    GDALRasterBand *oband = fout->GetRasterBand(1);
    oband->SetNoDataValue(no_data);

    //The geotransform maps each grid cell to a point in an affine-transformed
    //projection of the actual terrain. The geostransform is specified as follows:
    //    Xgeo = GT(0) + Xpixel*GT(1) + Yline*GT(2)
    //    Ygeo = GT(3) + Xpixel*GT(4) + Yline*GT(5)
    //In case of north up images, the GT(2) and GT(4) coefficients are zero, and
    //the GT(1) is pixel width, and GT(5) is pixel height. The (GT(0),GT(3))
    //position is the top left corner of the top left pixel of the raster.

    auto out_geotransform = geotransform;

    //We shift the top-left pixel of hte image eastward to the appropriate
    //coordinate
    out_geotransform[0] += xoffset*geotransform[1];

    //We shift the top-left pixel of the image southward to the appropriate
    //coordinate
    out_geotransform[3] += yoffset*geotransform[5];

    fout->SetGeoTransform(out_geotransform.data());
    fout->SetProjection(projection.c_str());

    #ifdef DEBUG
      std::cerr<<"Filename: "<<std::setw(20)<<filename<<" Xoffset: "<<std::setw(6)<<xoffset<<" Yoffset: "<<std::setw(6)<<yoffset<<" Geotrans0: "<<std::setw(10)<<std::setprecision(10)<<std::fixed<<geotrans[0]<<" Geotrans3: "<<std::setw(10)<<std::setprecision(10)<<std::fixed<<geotrans[3]<< std::endl;
    #endif

    for(int y=0;y<view_height;y++){
      auto temp = oband->RasterIO(GF_Write, 0, y, view_width, 1, data.data()+y*view_width, view_width, 1, myGDALType(), 0, 0); //TODO: Check for success
      if(temp!=CE_None)
        throw std::runtime_error("Error saving file with GDAL!");        
    }






    GDALClose(fout);
  }

  bool isEdgeCell(int x, int y) const {
    return (x==0 || y==0 || x==view_width-1 || y==view_height-1);
  }

  double getCellArea() const {
    return geotransform[1]*geotransform[5];
  }
};

#endif