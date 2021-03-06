// This is brl/bseg/boxm2/ocl/pro/processes/boxm2_ocl_update_aux_per_view_naa_process.cxx
//:
// \file
// \brief  A process for setting aux seg_len,vis,pre values
//
// \author Daniel Crispell
// \date Mar 3, 2012

#include <fstream>
#include <iostream>
#include <algorithm>
#include <bprb/bprb_func_process.h>

#include <vcl_compiler.h>
#include <boxm2/ocl/boxm2_opencl_cache.h>
#include <boxm2/boxm2_scene.h>
#include <boxm2/boxm2_block.h>
#include <boxm2/boxm2_data_base.h>
#include <boxm2/ocl/boxm2_ocl_util.h>
#include <boxm2/boxm2_util.h>
#include <vil/vil_image_view.h>
#include <boxm2/ocl/algo/boxm2_ocl_camera_converter.h>
#include <brad/brad_image_metadata.h>
#include <brad/brad_atmospheric_parameters.h>
#include <brad/brad_illum_util.h>

//brdb stuff
#include <brdb/brdb_value.h>

//directory utility
#include <vul/vul_timer.h>
#include <vcl_where_root_dir.h>
#include <bocl/bocl_device.h>
#include <bocl/bocl_kernel.h>

namespace boxm2_ocl_update_aux_per_view_naa_process_globals
{
  const unsigned n_inputs_  = 10;
  const unsigned n_outputs_ = 0;
  enum {
    UPDATE_SEGLEN         = 0,
    UPDATE_AUX_PREVIS     = 1,
    CONVERT_AUX_INT_FLOAT = 2
  };

  bool compile_kernel(bocl_device_sptr device,std::vector<bocl_kernel*> & vec_kernels,std::string opts)
  {
    //gather all render sources... seems like a lot for rendering...
    std::vector<std::string> src_paths;
    std::string source_dir = boxm2_ocl_util::ocl_src_root();
    src_paths.push_back(source_dir + "scene_info.cl");
    src_paths.push_back(source_dir + "cell_utils.cl");
    src_paths.push_back(source_dir + "bit/bit_tree_library_functions.cl");
    src_paths.push_back(source_dir + "backproject.cl");
    src_paths.push_back(source_dir + "statistics_library_functions.cl");
    src_paths.push_back(source_dir + "ray_bundle_library_opt.cl");
    src_paths.push_back(source_dir + "bit/update_kernels.cl");
    src_paths.push_back(source_dir + "bit/batch_update_kernels.cl");
    src_paths.push_back(source_dir + "bit/update_naa_kernels.cl");

    //convert aux buffer int values to float (just divide by SEGLENFACTOR
    bocl_kernel* convert_aux_int_float = new bocl_kernel();
    if (!convert_aux_int_float->create_kernel(&device->context(),device->device_id(), src_paths, "convert_aux_int_to_float", opts+" -D CONVERT_AUX ", "batch_update::convert_aux_int_to_float")) {
      std::cerr << "ERROR compiling kernel convert_aux_int_float\n";
      return false;
    }

    src_paths.push_back(source_dir + "update_functors.cl");
    src_paths.push_back(source_dir + "update_naa_functors.cl");
    src_paths.push_back(source_dir + "bit/cast_ray_bit.cl");

    // push back seg_len kernel
    bocl_kernel* seg_len = new bocl_kernel();
    std::string seg_opts = "-D SEGLEN -D STEP_CELL=step_cell_seglen(aux_args,data_ptr,llid,d) ";
    if (!seg_len->create_kernel(&device->context(),device->device_id(), src_paths, "seg_len_main", seg_opts, "update::seg_len")) {
      std::cerr << "ERROR compiling kernel seg_len\n";
      return false;
    }
    vec_kernels.push_back(seg_len);

    //push back cast_ray_bit
    bocl_kernel* aux_previs_naa_kernel = new bocl_kernel();
    std::string aux_opt = opts + " -D AUX_PREVIS_NAA -D STEP_CELL=step_cell_aux_previs_naa(aux_args,data_ptr,llid,d) ";
    if (!aux_previs_naa_kernel->create_kernel(&device->context(),device->device_id(), src_paths, "aux_previs_main_naa", aux_opt, "batch_update::aux_previs_main_naa")) {
      std::cerr << "ERROR compiling kernel aux_previs_naa_kernel\n";
      return false;
    }
    vec_kernels.push_back(aux_previs_naa_kernel);

    //may need DIFF LIST OF SOURCES FOR THSI GUY TOO
    vec_kernels.push_back(convert_aux_int_float);

    return true;
  }

  static std::map<std::string,std::vector<bocl_kernel*> > kernels;
}

bool boxm2_ocl_update_aux_per_view_naa_process_cons(bprb_func_process& pro)
{
  using namespace boxm2_ocl_update_aux_per_view_naa_process_globals;

  //process takes 10 inputs
  std::vector<std::string> input_types_(n_inputs_);
  input_types_[0] = "bocl_device_sptr";
  input_types_[1] = "boxm2_scene_sptr";
  input_types_[2] = "boxm2_opencl_cache_sptr";
  input_types_[3] = "vpgl_camera_double_sptr";
  input_types_[4] = "vil_image_view_base_sptr";
  input_types_[5] = "brad_image_metadata_sptr";
  input_types_[6] = "brad_atmospheric_parameters_sptr";
  input_types_[7] = "vcl_string";
  input_types_[8] = "vil_image_view_base_sptr";
  input_types_[9] = "vil_image_view_base_sptr";

  // process has no outputs
  std::vector<std::string>  output_types_(n_outputs_);

  bool good = pro.set_input_types(input_types_) && pro.set_output_types(output_types_);
  // in case the input 5 is not set
  brdb_value_sptr idx5 = new brdb_value_t<std::string>("");
  pro.set_input(5, idx5);

  return good;
}

bool boxm2_ocl_update_aux_per_view_naa_process(bprb_func_process& pro)
{
  using namespace boxm2_ocl_update_aux_per_view_naa_process_globals;
  std::size_t local_threads[2]={8,8};
  std::size_t global_threads[2]={8,8};

  if ( pro.n_inputs() < n_inputs_ ) {
    std::cout << pro.name() << ": The input number should be " << n_inputs_<< std::endl;
    return false;
  }
  float transfer_time=0.0f;
  float gpu_time=0.0f;
  //get the inputs
  unsigned i = 0;
  bocl_device_sptr device               = pro.get_input<bocl_device_sptr>(i++);
  boxm2_scene_sptr scene                = pro.get_input<boxm2_scene_sptr>(i++);
  boxm2_opencl_cache_sptr opencl_cache  = pro.get_input<boxm2_opencl_cache_sptr>(i++);
  vpgl_camera_double_sptr cam           = pro.get_input<vpgl_camera_double_sptr>(i++);
  vil_image_view_base_sptr img          = pro.get_input<vil_image_view_base_sptr>(i++);
  brad_image_metadata_sptr metadata        = pro.get_input<brad_image_metadata_sptr>(i++);
  brad_atmospheric_parameters_sptr atm_params = pro.get_input<brad_atmospheric_parameters_sptr>(i++);
  std::string suffix                     = pro.get_input<std::string>(i++);
  vil_image_view_base_sptr alt_prior_base    = pro.get_input<vil_image_view_base_sptr>(i++);
  vil_image_view_base_sptr alt_density_base  = pro.get_input<vil_image_view_base_sptr>(i++);

  vil_image_view<float> *alt_prior = dynamic_cast<vil_image_view<float>*>(alt_prior_base.ptr());
  if (!alt_prior) {
    std::cerr << "ERROR casting alt_prior to vil_image_view<float>\n";
    return false;
  }
  vil_image_view<float> *alt_density = dynamic_cast<vil_image_view<float>*>(alt_density_base.ptr());
  if (!alt_density) {
    std::cerr << "ERROR casting alt_density to vil_image_view<float>\n";
    return false;
  }
  // variances
  const double reflectance_var = boxm2_normal_albedo_array_constants::sigma_albedo * boxm2_normal_albedo_array_constants::sigma_albedo;
  const double airlight_var = boxm2_normal_albedo_array_constants::sigma_airlight * boxm2_normal_albedo_array_constants::sigma_airlight;
  const double optical_depth_var = boxm2_normal_albedo_array_constants::sigma_optical_depth * boxm2_normal_albedo_array_constants::sigma_optical_depth;
  const double skylight_var = boxm2_normal_albedo_array_constants::sigma_skylight * boxm2_normal_albedo_array_constants::sigma_skylight;

  // get normal directions
  std::vector<vgl_vector_3d<double> > normals = boxm2_normal_albedo_array::get_normals();
  unsigned int num_normals = normals.size();
  // opencl code depends on there being exactly 16 normal directions - do sanity check here
  if (num_normals != 16) {
    std::cerr << "ERROR: boxm2_ocl_update_aux_per_view_naa_process: num_normals = " << num_normals << ".  Expected 16\n";
    return false;
  }

  long binCache = opencl_cache.ptr()->bytes_in_cache();
  std::cout<<"Update MBs in cache: "<<binCache/(1024.0*1024.0)<<std::endl;

  bool foundDataType = false;
  std::string data_type,num_obs_type,options;
  std::vector<std::string> apps = scene->appearances();
  int appTypeSize;
  for (unsigned int i=0; i<apps.size(); ++i) {
    if ( apps[i] == boxm2_data_traits<BOXM2_NORMAL_ALBEDO_ARRAY>::prefix() )
    {
      data_type = apps[i];
      foundDataType = true;
      options=" -D BOXM2_NORMAL_ALBEDO_ARRAY ";
      appTypeSize = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_NORMAL_ALBEDO_ARRAY>::prefix());
    }
  }
  if (!foundDataType) {
    std::cout<<"BOXM2_OCL_UPDATE_AUX_PER_VIEW_NAA_PROCESS ERROR: scene doesn't have NORMAL_ALBEDO_ARRAY data type"<<std::endl;
    return false;
  }

  // create a command queue.
  int status=0;
  cl_command_queue queue = clCreateCommandQueue(device->context(),*(device->device_id()),CL_QUEUE_PROFILING_ENABLE,&status);
  if (status!=0) return false;

  std::string identifier=device->device_identifier()+options;
  // compile the kernel if not already compiled
  if (kernels.find(identifier)==kernels.end())
  {
    std::cout<<"===========Compiling kernels==========="<<std::endl;
    std::vector<bocl_kernel*> ks;
    if (!compile_kernel(device,ks,"")) {
      std::cerr << "ERROR: compile kernel returned false\n";
      return false;
    }
    kernels[identifier]=ks;
  }

  //grab input image, establish cl_ni, cl_nj (so global size is divisible by local size)
  vil_image_view_base_sptr float_img=boxm2_util::prepare_input_image(img);
  vil_image_view<float>* img_view = static_cast<vil_image_view<float>* >(float_img.ptr());
  unsigned cl_ni=(unsigned)RoundUp(img_view->ni(),(int)local_threads[0]);
  unsigned cl_nj=(unsigned)RoundUp(img_view->nj(),(int)local_threads[1]);
  global_threads[0]=cl_ni;
  global_threads[1]=cl_nj;

  // buffers for holding radiance scales per normal
  float* radiance_scales_buff = new float[num_normals];
  float* radiance_offsets_buff = new float[num_normals];
  float* radiance_var_scales_buff = new float[num_normals];
  float* radiance_var_offsets_buff = new float[num_normals];

  // compute offsets and scales for linear radiance model
  for (unsigned n=0; n < num_normals; ++n) {
     // compute offsets as radiance of surface with 0 reflectance
     double offset = brad_expected_radiance_chavez(0.0, normals[n], *metadata, *atm_params);
     radiance_offsets_buff[n] = offset;
     // use perfect reflector to compute radiance scale
     double radiance = brad_expected_radiance_chavez(1.0, normals[n], *metadata, *atm_params);
     radiance_scales_buff[n] = radiance - offset;
     // compute offset of radiance variance
     double var_offset = brad_radiance_variance_chavez(0.0, normals[n], *metadata, *atm_params, reflectance_var, optical_depth_var, skylight_var, airlight_var);
     radiance_var_offsets_buff[n] = var_offset;
     // compute scale
     double var = brad_radiance_variance_chavez(1.0, normals[n], *metadata, *atm_params, reflectance_var, optical_depth_var, skylight_var, airlight_var);
     radiance_var_scales_buff[n] = var - var_offset;
     std::cout << "---- normal = " << normals[n] << std::endl
              << "radiance scale = " << radiance << " offset = " << offset << std::endl
              << "radiance var scale = " << var << " variance offset = " << var_offset << std::endl;
  }

  //set generic cam
  cl_float* ray_origins = new cl_float[4*cl_ni*cl_nj];
  cl_float* ray_directions = new cl_float[4*cl_ni*cl_nj];
  std::cout << "allocating ray_o_buff: ni = " << cl_ni << ", nj = " << cl_nj << "  size = " << cl_ni*cl_nj*sizeof(cl_float4) << std::endl;
  bocl_mem_sptr ray_o_buff = opencl_cache->alloc_mem( cl_ni*cl_nj * sizeof(cl_float4) , ray_origins,"ray_origins buffer");
  std::cout << "allocating ray_d_buff: ni = " << cl_ni << ", nj = " << cl_nj << std::endl;
  bocl_mem_sptr ray_d_buff = opencl_cache->alloc_mem(cl_ni*cl_nj * sizeof(cl_float4), ray_directions,"ray_directions buffer");
  boxm2_ocl_camera_converter::compute_ray_image( device, queue, cam, cl_ni, cl_nj, ray_o_buff, ray_d_buff);

  //Visibility, Preinf, Norm, and input image buffers
  float* vis_buff = new float[cl_ni*cl_nj];
  float* pre_buff = new float[cl_ni*cl_nj];
  float* input_buff=new float[cl_ni*cl_nj];

  int count=0;
  for (unsigned int j=0;j<cl_nj;++j) {
    for (unsigned int i=0;i<cl_ni;++i) {
      if ( (i < img_view->ni()) && (j < img_view->nj()) ) {
        input_buff[count]=(*img_view)(i,j);
        vis_buff[count] = 1.0f - (*alt_prior)(i,j);
        pre_buff[count] = (*alt_prior)(i,j) * (*alt_density)(i,j);
      }
      else {
        input_buff[count] = 0.0f;
        vis_buff[count] = 0.0f;
        pre_buff[count] = 0.0f;
      }
      ++count;
    }
  }
  std::cout << "allocating input_buff: ni = " << cl_ni << ", nj = " << cl_nj << std::endl;
  bocl_mem_sptr in_image=opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float),input_buff,"input image buffer");
  in_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  std::cout << "allocating vis_buff: ni = " << cl_ni << ", nj = " << cl_nj << std::endl;
  bocl_mem_sptr vis_image=opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float),vis_buff,"vis image buffer");
  vis_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  std::cout << "allocating pre_buff: ni = " << cl_ni << ", nj = " << cl_nj << std::endl;
  bocl_mem_sptr pre_image = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float),pre_buff,"pre image buffer");
  pre_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  std::cout << "Done allocating images" << std::endl;

  // Allocate buffers for holding radiance scales and offsets
  bocl_mem_sptr radiance_scales = new bocl_mem(device->context(), radiance_scales_buff, sizeof(float)*num_normals,"radiance scales buffer");
  radiance_scales->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

  bocl_mem_sptr radiance_offsets = new bocl_mem(device->context(), radiance_offsets_buff, sizeof(float)*num_normals,"radiance offsets buffer");
  radiance_offsets->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

  bocl_mem_sptr radiance_var_scales = new bocl_mem(device->context(), radiance_var_scales_buff, sizeof(float)*num_normals,"radiance variance scales buffer");
  radiance_var_scales->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

  bocl_mem_sptr radiance_var_offsets = new bocl_mem(device->context(), radiance_var_offsets_buff, sizeof(float)*num_normals,"radiance variance offsets buffer");
  radiance_var_offsets->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

  // Image Dimensions
  int img_dim_buff[4];
  img_dim_buff[0] = 0;
  img_dim_buff[1] = 0;
  img_dim_buff[2] = img_view->ni();
  img_dim_buff[3] = img_view->nj();
  bocl_mem_sptr img_dim=new bocl_mem(device->context(), img_dim_buff, sizeof(int)*4, "image dims");
  img_dim->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  // Output Array
  float output_arr[100];
  for (int i=0; i<100; ++i) output_arr[i] = 0.0f;
  bocl_mem_sptr  cl_output=new bocl_mem(device->context(), output_arr, sizeof(float)*100, "output buffer");
  cl_output->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  // bit lookup buffer
  cl_uchar lookup_arr[256];
  boxm2_ocl_util::set_bit_lookup(lookup_arr);
  bocl_mem_sptr lookup=new bocl_mem(device->context(), lookup_arr, sizeof(cl_uchar)*256, "bit lookup buffer");
  lookup->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

  // app density used for proc_norm_image
  float app_buffer[4]={1.0,0.0,0.0,0.0};
  bocl_mem_sptr app_density = new bocl_mem(device->context(), app_buffer, sizeof(cl_float4), "app density buffer");
  app_density->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  // set arguments
  std::vector<boxm2_block_id> vis_order = scene->get_vis_blocks(cam);
  std::vector<boxm2_block_id>::iterator id;
  for (unsigned int i=0; i<kernels[identifier].size(); ++i)
  {
    for (id = vis_order.begin(); id != vis_order.end(); ++id)
    {
      //choose correct render kernel
      boxm2_block_metadata mdata = scene->get_block_metadata(*id);
      bocl_kernel* kern =  kernels[identifier][i];

      //write the image values to the buffer
      vul_timer transfer;
      bocl_mem* blk       = opencl_cache->get_block(scene,*id);
      bocl_mem* blk_info  = opencl_cache->loaded_block_info();
      bocl_mem* alpha     = opencl_cache->get_data<BOXM2_ALPHA>(scene,*id,0,false);
      boxm2_scene_info* info_buffer = (boxm2_scene_info*) blk_info->cpu_buffer();
      int alphaTypeSize = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_ALPHA>::prefix());
      unsigned int num_cells = alpha->num_bytes()/alphaTypeSize;
      info_buffer->data_buffer_length = (int)num_cells;
      blk_info->write_to_buffer((queue));
      // get appearance model
      bocl_mem* naa_apm   = opencl_cache->get_data(scene,*id,data_type,num_cells*appTypeSize, true);

      //grab an appropriately sized AUX data buffer
      int auxTypeSize  = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX0>::prefix());
      bocl_mem *aux0   = opencl_cache->get_data(scene,*id, boxm2_data_traits<BOXM2_AUX0>::prefix(suffix),info_buffer->data_buffer_length*auxTypeSize,false);
      auxTypeSize      = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX1>::prefix());
      bocl_mem *aux1   = opencl_cache->get_data(scene,*id, boxm2_data_traits<BOXM2_AUX1>::prefix(suffix),info_buffer->data_buffer_length*auxTypeSize,false);
      auxTypeSize      = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX2>::prefix());
      bocl_mem *aux2   = opencl_cache->get_data(scene,*id,boxm2_data_traits<BOXM2_AUX2>::prefix(suffix), info_buffer->data_buffer_length*auxTypeSize,false);
      auxTypeSize      = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX3>::prefix());
      bocl_mem *aux3   = opencl_cache->get_data(scene,*id,boxm2_data_traits<BOXM2_AUX3>::prefix(suffix), info_buffer->data_buffer_length*auxTypeSize,false);

      transfer_time += (float) transfer.all();
      if (i==UPDATE_SEGLEN)
      {
        aux0->zero_gpu_buffer(queue);
        aux1->zero_gpu_buffer(queue);
        kern->set_arg( blk_info );
        kern->set_arg( blk );
        kern->set_arg( alpha );
        kern->set_arg( aux0 );
        kern->set_arg( aux1 );
        kern->set_arg( lookup.ptr() );

        kern->set_arg( ray_o_buff.ptr() );
        kern->set_arg( ray_d_buff.ptr() );

        kern->set_arg( img_dim.ptr() );
        kern->set_arg( in_image.ptr() );
        kern->set_arg( cl_output.ptr() );
        kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar16) );//local tree,
        kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar4) ); //ray bundle,
        kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_int) );    //cell pointers,
        kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_float4) ); //cached aux,
        kern->set_local_arg( local_threads[0]*local_threads[1]*10*sizeof(cl_uchar) ); //cumsum buffer, imindex buffer

        //execute kernel
        kern->execute(queue, 2, local_threads, global_threads);
        int status = clFinish(queue);
        check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status));
        gpu_time += kern->exec_time();

        //clear render kernel args so it can reset em on next execution
        kern->clear_args();

        aux0->read_to_buffer(queue);
        aux1->read_to_buffer(queue);
      }
      else if (i==UPDATE_AUX_PREVIS)
      {
        //aux0->zero_gpu_buffer(queue);
        //aux1->zero_gpu_buffer(queue);
        aux2->zero_gpu_buffer(queue);
        aux3->zero_gpu_buffer(queue);

        kern->set_arg( blk_info );
        kern->set_arg( blk );
        kern->set_arg( alpha );
        kern->set_arg( radiance_scales.ptr() );
        kern->set_arg( radiance_offsets.ptr() );
        kern->set_arg( radiance_var_scales.ptr() );
        kern->set_arg( radiance_var_offsets.ptr() );
        kern->set_arg( naa_apm );
        kern->set_arg( aux0 );
        kern->set_arg( aux1 );
        kern->set_arg( aux2 );
        kern->set_arg( aux3 );
        kern->set_arg( lookup.ptr() );
        kern->set_arg( ray_o_buff.ptr() );
        kern->set_arg( ray_d_buff.ptr() );

        kern->set_arg( img_dim.ptr() );
        kern->set_arg( vis_image.ptr() );
        kern->set_arg( pre_image.ptr() );
        kern->set_arg( cl_output.ptr() );
        kern->set_local_arg( local_threads[0]*local_threads[1]   *sizeof(cl_uchar16) );//local tree,
        kern->set_local_arg( local_threads[0]*local_threads[1]   *sizeof(cl_short2) ); //ray bundle,
        kern->set_local_arg( local_threads[0]*local_threads[1]   *sizeof(cl_int) );    //cell pointers,
        kern->set_local_arg( local_threads[0]*local_threads[1]   *sizeof(cl_float) ); //cached aux,
        kern->set_local_arg( local_threads[0]*local_threads[1]*10*sizeof(cl_uchar) ); //cumsum buffer, imindex buffer
        //execute kernel
        kern->execute(queue, 2, local_threads, global_threads);
        int status = clFinish(queue);
        check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status));
        gpu_time += kern->exec_time();
        kern->clear_args();

        //clear render kernel args so it can reset em on next execution
        aux0->read_to_buffer(queue);
        aux1->read_to_buffer(queue);
        aux2->read_to_buffer(queue);
        aux3->read_to_buffer(queue);
      }
      else if (i==CONVERT_AUX_INT_FLOAT)
      {
        local_threads[0] = 64;
        local_threads[1] = 1 ;
        global_threads[0]=RoundUp(info_buffer->data_buffer_length,local_threads[0]);
        global_threads[1]=1;

        kern->set_arg( blk_info );
        kern->set_arg( aux0 );
        kern->set_arg( aux1 );
        kern->set_arg( aux2 );
        kern->set_arg( aux3 );

        //execute kernel
        kern->execute(queue, 2, local_threads, global_threads);
        int status = clFinish(queue);
        check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status));
        gpu_time += kern->exec_time();

        //clear render kernel args so it can reset em on next execution
        kern->clear_args();

        //write info to disk
        aux0->read_to_buffer(queue);
        aux1->read_to_buffer(queue);
        aux2->read_to_buffer(queue);
        aux3->read_to_buffer(queue);

        opencl_cache->deep_remove_data(scene,*id,boxm2_data_traits<BOXM2_AUX0>::prefix(suffix), true);
        opencl_cache->deep_remove_data(scene,*id,boxm2_data_traits<BOXM2_AUX1>::prefix(suffix), true);
        opencl_cache->deep_remove_data(scene,*id,boxm2_data_traits<BOXM2_AUX2>::prefix(suffix), true);
        opencl_cache->deep_remove_data(scene,*id,boxm2_data_traits<BOXM2_AUX3>::prefix(suffix), true);
      }
      //read image out to buffer (from gpu)
      //in_image->read_to_buffer(queue);
      vis_image->read_to_buffer(queue);
      pre_image->read_to_buffer(queue);
      cl_output->read_to_buffer(queue);
      clFinish(queue);
    }
  }

  opencl_cache->unref_mem(in_image.ptr());
  opencl_cache->unref_mem(vis_image.ptr());
  opencl_cache->unref_mem(pre_image.ptr());
  opencl_cache->unref_mem(ray_d_buff.ptr());
  opencl_cache->unref_mem(ray_o_buff.ptr());

  delete [] vis_buff;
  delete [] pre_buff;
  delete [] input_buff;
  delete [] ray_origins;
  delete [] ray_directions;
  delete [] radiance_scales_buff;
  delete [] radiance_offsets_buff;
  delete [] radiance_var_scales_buff;
  delete [] radiance_var_offsets_buff;

  std::cout<<"Gpu time "<<gpu_time<<" transfer time "<<transfer_time<<std::endl;
  clReleaseCommandQueue(queue);
  return true;
}
