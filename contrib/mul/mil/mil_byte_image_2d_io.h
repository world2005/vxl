// This is mul/mil/mil_byte_image_2d_io.h
#ifndef mil_byte_image_2d_io_h_
#define mil_byte_image_2d_io_h_
#ifdef VCL_NEEDS_PRAGMA_INTERFACE
#pragma interface
#endif
//:
//  \file
//  \brief Load and save mil_image_2d_of<vil_byte> from named files.
//  \author Tim Cootes

#include <mil/mil_image_io.h>
#include <mil/mil_image_2d_of.h>
#include <vil/vil_byte.h>

class vil_image;

class mil_image;

enum mil_byte_image_2d_io_std_depths
{
  mil_byte_image_2d_io_depth_image = 0,
  mil_byte_image_2d_io_depth_grey = 1,
  mil_byte_image_2d_io_depth_rgb = 3
};

//: Load and save mil_image_2d_of<vil_byte> from named files.
class mil_byte_image_2d_io : public mil_image_io
{
  //: Current image object
  //  image() returns a reference to this
  mil_image_2d_of<vil_byte> image_;

#if 0
  //: Define whether to load images as colour or grey-scale
  //  Options are '' (ie rely on image), 'Grey' or 'RGB'
  vcl_string colour_;
#endif

  //: The colour depth of the image to be loaded by the codes listed above
  mil_byte_image_2d_io_std_depths colour_code_;

  static vcl_string guessFileType(const vcl_string& path);

 public:

  //: Dflt ctor
  mil_byte_image_2d_io();

  //: Destructor
  virtual ~mil_byte_image_2d_io();

  //: Define whether to load images as colour or grey-scale
  //  Options are '' (ie rely on image), 'Grey' or 'RGB'
  void setColour(const vcl_string&);
  //: Set the colour by the depth or using the standard code in the header
  //  Only colour (3-plane) and greyscale (1-plane) currently supported
  //  Returns false if the depth is not suipported
  bool set_colour_depth( int );

  //: Whether to load images as RGB, Grey-scale or leave to image format
  //  Returns "RGB","Grey" or ""
  vcl_string colour() const;
  //: Return the colour depth using the standard code in the header
  //  Only colour (3-plane) and greyscale (1-plane) currently supported
  int colour_depth();

  //: Current image
  //  (The one generated by last call to b_read())
  virtual const mil_image& image() const;

  //: Attempt to load image from named file
  // \param filetype  String hinting at what image format is.
  // \return true if successful
  virtual bool loadImage(const vcl_string& path,
                         const vcl_string& filetype);

  //: Attempt to save image to named filepath
  // \param filetype  String defining what format to save in.
  // \return true if successful
  virtual bool saveImage(const mil_image& image,
	                     const vcl_string& filepath,
                         const vcl_string& filetype) const;

  //: Attempt to load image from named filepath
  // \param filetype  String hinting at what image format is.
  //  If filetype=="", then guess the format from the path extension
  // \return true if successful
  bool loadTheImage(mil_image_2d_of<vil_byte>& image,
                    const vcl_string& filepath,
                    const vcl_string& filetype);

  //: Attempt to save image to named filepath
  // \param filetype  String defining what format to save in.
  static bool saveTheImage(const mil_image_2d_of<vil_byte>& image,
                           const vcl_string& filepath,
                           const vcl_string& filetype);

  //: Version number for I/O
  short version_no() const;

  //: Name of the class
  virtual vcl_string is_a() const;

  //: Does the name of the class match the argument?
  virtual bool is_class(vcl_string const& s) const;

  //: Create a copy on the heap and return base class pointer
  virtual mil_image_io* clone() const;

  //: Print class to os
  virtual void print_summary(vcl_ostream& os) const;

  //: Save class to binary file stream
  virtual void b_write(vsl_b_ostream& bfs) const;

  //: Load class from binary file stream
  virtual void b_read(vsl_b_istream& bfs);
};

#endif // mil_byte_image_2d_io_h_
