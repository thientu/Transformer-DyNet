#include "dynet/nodes-arith-cwise.h"

#include "dynet/nodes-macros.h"

using namespace std;

namespace dynet {

// ************* CwiseSum*************
#ifndef __CUDACC__

string CwiseSum::as_string(const vector<string>& arg_names) const {
  ostringstream s;
  s << arg_names[0];
  for (unsigned i = 1; i < arg_names.size(); ++i)
    s << " + " << arg_names[i];
  return s.str();
}

Dim CwiseSum::dim_forward(const vector<Dim>& xs) const {
  DYNET_ARG_CHECK(xs.size() == 2, "Failed input count check in CwiseSum");
  std::vector<long> dims({});
  for(unsigned int i = 0; i < min(xs[0].nd, xs[1].nd); i++){
    DYNET_ARG_CHECK(xs[0].d[i]==xs[1].d[i] || min(xs[0].d[i], xs[1].d[i])==1, "CwiseSum: For each dimension, the dim size needs to match or equal 1.");
  }
  DYNET_ARG_CHECK(xs[0].bd==xs[1].bd || min(xs[0].bd, xs[1].bd)==1, "CwiseSum: batch size must match or equal 1");
  for(unsigned int i = 0; i < max(xs[0].nd, xs[1].nd); i++){
    if(i < min(xs[0].nd, xs[1].nd)) dims.push_back(max(xs[0].d[i], xs[1].d[i]));
    else if(i < xs[0].nd) dims.push_back(xs[0].d[i]);
    else dims.push_back(xs[1].d[i]);
  }
  Dim d(dims, max(xs[0].bd, xs[1].bd));
  return d;
}

int CwiseSum::autobatch_sig(const ComputationGraph & cg, SigMap &sm) const {
  // TODO: This does not handle the case where dimensions differ
  Sig s(nt::csum);
  return cg.nodes[args[0]]->dim == cg.nodes[args[1]]->dim ? sm.get_idx(s) : 0;
}

std::vector<int> CwiseSum::autobatch_concat(const ComputationGraph & cg) const {
  return vector<int>(2, 1);
}

#endif


template<class MyDevice>
void CwiseSum::forward_dev_impl(const MyDevice & dev, const vector<const Tensor*>& xs, Tensor& fx) const {
  DYNET_ASSERT(xs.size() == 2, "Failed dimension check in CwiseMultiply::forward (cmult)");
  size_t i;
  for(i = 0; i < fx.d.nd && xs[0]->d[i] == xs[1]->d[i]; ++i);
  // No broadcasting over dims, just batches
  if(i == fx.d.nd) {
    if(xs[0]->d.bd == xs[1]->d.bd) {
      fx.tvec().device(*dev.edevice) = xs[0]->tvec() + xs[1]->tvec();
    } else {
      int greater = xs[0]->d.bd > xs[1]->d.bd ? 0 : 1;
#ifdef __CUDACC__
      Eigen::array<int, 2> bcast = {1,(int)xs[greater]->d.bd};
      fx.tbvec().device(*dev.edevice) = xs[1-greater]->tbvec().broadcast(bcast) + xs[greater]->tbvec();
#else
      for(size_t b = 0; b < fx.d.bd; ++b)
        fx.tbvec().chip<1>(b).device(*dev.edevice) = xs[1-greater]->tvec() + xs[greater]->tbvec().chip<1>(b);
#endif
    }
  // Broadcasting over dims as well
  } else {
    Eigen::array<int, 5> bcast_left = {1,1,1,1,1}, bcast_right = {1,1,1,1,1};
    bool has_left = false, has_right = false;
    for(; i < fx.d.nd; ++i){
      if(xs[0]->d[i] > xs[1]->d[i]) {
        has_right = true;
        bcast_right[i] = xs[0]->d[i];
      } else if (xs[0]->d[i] < xs[1]->d[i]) {
        has_left = true;
        bcast_left[i] = xs[1]->d[i];
      }
    }
    if(xs[0]->d.bd > xs[1]->d.bd) {
      has_right = true;
      bcast_right[4] =  xs[0]->d.bd;
    } else if(xs[0]->d.bd < xs[1]->d.bd) {
      has_left = true;
      bcast_left[4] =  xs[1]->d.bd;
    }
    if(has_right && has_left) {
      fx.tb<4>().device(*dev.edevice) = xs[0]->tb<4>().broadcast(bcast_left) + xs[1]->tb<4>().broadcast(bcast_right);
    } else if(has_right) {
      fx.tb<4>().device(*dev.edevice) = xs[0]->tb<4>() + xs[1]->tb<4>().broadcast(bcast_right);
    } else {
      fx.tb<4>().device(*dev.edevice) = xs[0]->tb<4>().broadcast(bcast_left) + xs[1]->tb<4>();
    }
  }
}

template<class MyDevice>
void CwiseSum::backward_dev_impl(const MyDevice & dev,
                             const vector<const Tensor*>& xs,
                             const Tensor& fx,
                             const Tensor& dEdf,
                             unsigned i,
                             Tensor& dEdxi) const {
  DYNET_ASSERT(i < 2, "Failed dimension check in CwiseSum::backward (+)");
  // Find out whether we're broadcasting and if so how much
  int n_red = 0;
  for(unsigned int j = 0; j < fx.d.nd; j++)
    n_red += xs[i]->d[j] != fx.d[j] ? 1 : 0;
  // If dimensions are the same, just add over the whole vector
  if(!n_red) {
    if(dEdxi.d.bd == dEdf.d.bd) {
      dEdxi.tvec().device(*dev.edevice) += dEdf.tvec();
    } else {
#ifdef __CUDACC__
      Eigen::array<int, 1> red_axis = {1};
      dEdxi.tvec().device(*dev.edevice) += dEdf.tbvec().sum(red_axis);
#else
      for(size_t b = 0; b < dEdf.d.bd; ++b)
        dEdxi.tvec().device(*dev.edevice) += dEdf.tbvec().chip<1>(b);
#endif
    }
  // Otherwise work with broadcasting, etc.
  } else {
    n_red += xs[i]->d.bd!=fx.d.bd?1:0;
    DYNET_ASSERT(n_red < 5 && n_red > 0, "Unsupported number of reductions check in CwiseSum::backward (+)");
    if(n_red==1) backward_helper<MyDevice, 1>(dev, xs, fx, dEdf, i, dEdxi);
    else if(n_red==2) backward_helper<MyDevice, 2>(dev, xs, fx, dEdf, i, dEdxi);
    else if(n_red==3) backward_helper<MyDevice, 3>(dev, xs, fx, dEdf, i, dEdxi);
    else if(n_red==4) backward_helper<MyDevice, 4>(dev, xs, fx, dEdf, i, dEdxi);
  }
}
DYNET_NODE_INST_DEV_IMPL(CwiseSum)

template<class MyDevice, int ReductionOrder>
void CwiseSum::backward_helper(const MyDevice & dev,
		     const std::vector<const Tensor*>& xs,
		     const Tensor& fx,
		     const Tensor& dEdf,
		     unsigned i,
		     Tensor& dEdxi) const {
  Eigen::array<int, ReductionOrder> red_axis;
  if(ReductionOrder>0) red_axis[ReductionOrder-1] = 4;
  int curr_red_axis = 0;
  Eigen::array<int, 5> morph = {1,1,1,1,(int)xs[i]->d.bd};
  for(unsigned int di = 0; di < fx.d.nd; di++) {
    if((di >= xs[i]->d.nd && fx.d[di]>1) || xs[i]->d[di] != fx.d[di]) {
      red_axis[curr_red_axis] = di;
      curr_red_axis++;
    }
    morph[di] = xs[i]->d[di];
  }

  dEdxi.tb<4>().device(*dev.edevice) += dEdf.tb<4>().sum(red_axis).reshape(morph);
}

// ************* CwiseMultiply *************

#ifndef __CUDACC__

string CwiseMultiply::as_string(const vector<string>& arg_names) const {
  ostringstream s;
  s << arg_names[0] << " \\cdot " << arg_names[1];
  return s.str();
}

Dim CwiseMultiply::dim_forward(const vector<Dim>& xs) const {
  DYNET_ARG_CHECK(xs.size() == 2, "Failed input count check in CwiseMultiply");
  std::vector<long> dims({});
  for(unsigned int i = 0; i < min(xs[0].nd, xs[1].nd); i++){
    DYNET_ARG_CHECK(xs[0].d[i]==xs[1].d[i] || min(xs[0].d[i], xs[1].d[i])==1, "CwiseMultiply: For each dimension, the dim size needs to match or equal 1.");
  }
  DYNET_ARG_CHECK(xs[0].bd==xs[1].bd || min(xs[0].bd, xs[1].bd)==1, "CwiseMultiply: batch size must match or equal 1");
  for(unsigned int i = 0; i < max(xs[0].nd, xs[1].nd); i++){
    if(i < min(xs[0].nd, xs[1].nd)) dims.push_back(max(xs[0].d[i], xs[1].d[i]));
    else if(i < xs[0].nd) dims.push_back(xs[0].d[i]);
    else dims.push_back(xs[1].d[i]);
  }
  Dim d(dims, max(xs[0].bd, xs[1].bd));
  return d;
}

int CwiseMultiply::autobatch_sig(const ComputationGraph & cg, SigMap &sm) const {
  // TODO: This does not handle the case where dimensions differ
  Sig s(nt::cmult);
  return cg.nodes[args[0]]->dim == cg.nodes[args[1]]->dim ? sm.get_idx(s) : 0;
}

std::vector<int> CwiseMultiply::autobatch_concat(const ComputationGraph & cg) const {
  return vector<int>(2, 1);
}

#endif

template<class MyDevice>
void CwiseMultiply::forward_dev_impl(const MyDevice & dev, const vector<const Tensor*>& xs, Tensor& fx) const {
  DYNET_ASSERT(xs.size() == 2, "Failed dimension check in CwiseMultiply::forward (cmult)");
  size_t i;
  for(i = 0; i < fx.d.nd && xs[0]->d[i] == xs[1]->d[i]; ++i);
  // No broadcasting over dims, just batches
  if(i == fx.d.nd) {
    if(xs[0]->d.bd == xs[1]->d.bd) {
      fx.tvec().device(*dev.edevice) = xs[0]->tvec() * xs[1]->tvec(); 
    } else {
      int greater = xs[0]->d.bd > xs[1]->d.bd ? 0 : 1;
#ifdef __CUDACC__
      Eigen::array<int, 2> bcast = {1,(int)xs[greater]->d.bd};
      fx.tbvec().device(*dev.edevice) = xs[1-greater]->tbvec().broadcast(bcast) * xs[greater]->tbvec();
#else
      for(size_t b = 0; b < fx.d.bd; ++b)
        fx.tbvec().chip<1>(b).device(*dev.edevice) = xs[1-greater]->tvec() * xs[greater]->tbvec().chip<1>(b);
#endif
    }
  // Broadcasting over dims as well
  } else {
    Eigen::array<int, 5> bcast_left = {1,1,1,1,1}, bcast_right = {1,1,1,1,1};
    bool has_left = false, has_right = false;
    for(; i < fx.d.nd; ++i){
      if(xs[0]->d[i] > xs[1]->d[i]) {
        has_right = true;
        bcast_right[i] = xs[0]->d[i];
      } else if (xs[0]->d[i] < xs[1]->d[i]) {
        has_left = true;
        bcast_left[i] = xs[1]->d[i];
      }
    }
    if(xs[0]->d.bd > xs[1]->d.bd) {
      has_right = true;
      bcast_right[4] =  xs[0]->d.bd;
    } else if(xs[0]->d.bd < xs[1]->d.bd) {
      has_left = true;
      bcast_left[4] =  xs[1]->d.bd;
    }
    if(has_right && has_left) {
      fx.tb<4>().device(*dev.edevice) = xs[0]->tb<4>().broadcast(bcast_left) * xs[1]->tb<4>().broadcast(bcast_right);
    } else if(has_right) {
      fx.tb<4>().device(*dev.edevice) = xs[0]->tb<4>() * xs[1]->tb<4>().broadcast(bcast_right);
    } else {
      fx.tb<4>().device(*dev.edevice) = xs[0]->tb<4>().broadcast(bcast_left) * xs[1]->tb<4>();
    }
  }
}

template<class MyDevice>
void CwiseMultiply::backward_dev_impl(const MyDevice & dev,
                             const vector<const Tensor*>& xs,
                             const Tensor& fx,
                             const Tensor& dEdf,
                             unsigned i,
                             Tensor& dEdxi) const {
  DYNET_ASSERT(i < 2, "Failed dimension check in CwiseMultiply::backward (cmult)");
  // Find out whether we're broadcasting and if so how much
  int n_red = xs[i]->d.bd!=fx.d.bd?1:0;
  bool must_red = false;
  for(unsigned int j = 0; j < fx.d.nd; j++) {
    n_red += xs[i]->d[j] != fx.d[j] ? 1 : 0;
    must_red = must_red || xs[0]->d[j] != xs[1]->d[j];
  }
  // If dimensions are the same, just add over the whole vector
  if(!must_red) {
    if(xs[0]->d.bd == xs[1]->d.bd) {
      dEdxi.tvec().device(*dev.edevice) += dEdf.tvec() * xs[1-i]->tvec();
    } else if(xs[1-i]->d.bd == 1) {
      // TODO: Make alternative code path for CPU?
      Eigen::array<int, 2> bcast; bcast[0] = 1; bcast[1] = fx.d.bd;
      dEdxi.tbvec().device(*dev.edevice) += dEdf.tbvec() * xs[1-i]->tbvec().broadcast(bcast);
    } else {
      Eigen::array<int, 1> red_axis; red_axis[0] = 1;
      dEdxi.tvec().device(*dev.edevice) += (dEdf.tbvec() * xs[1-i]->tbvec()).sum(red_axis);
    }
  // Otherwise work with broadcasting, etc.
  } else {
    DYNET_ASSERT(n_red < 5 && n_red > 0, "Unsupported number of reductions check in CwiseMultiply::backward (cmult)");
    if(n_red==0) backward_helper<MyDevice, 0>(dev, xs, fx, dEdf, i, dEdxi);
    else if(n_red==1) backward_helper<MyDevice, 1>(dev, xs, fx, dEdf, i, dEdxi);
    else if(n_red==2) backward_helper<MyDevice, 2>(dev, xs, fx, dEdf, i, dEdxi);
    else if(n_red==3) backward_helper<MyDevice, 3>(dev, xs, fx, dEdf, i, dEdxi);
    else if(n_red==4) backward_helper<MyDevice, 4>(dev, xs, fx, dEdf, i, dEdxi);
  }
}
DYNET_NODE_INST_DEV_IMPL(CwiseMultiply)

template<class MyDevice, int ReductionOrder>
void CwiseMultiply::backward_helper(const MyDevice & dev,
	                             const vector<const Tensor*>& xs,
	                             const Tensor& fx,
	                             const Tensor& dEdf,
	                             unsigned i,
	                             Tensor& dEdxi) const {
  Eigen::array<int, ReductionOrder> red_axis;
  Eigen::array<int, 5> morph = {1,1,1,1,(int)xs[i]->d.bd}, bcast_other = {1,1,1,1,1};
  if(ReductionOrder>0) red_axis[ReductionOrder-1] = 4;
  int curr_red_axis = 0;
  for(unsigned int di = 0; di < fx.d.nd; di++){
    if(xs[i]->d[di] != fx.d[di]) {
      red_axis[curr_red_axis] = di;
      curr_red_axis++;
    }
    morph[di] = xs[i]->d[di];
    if(xs[1-i]->d[di]==1)
      bcast_other[di] = fx.d[di];
  }
  if(xs[1-i]->d.bd == 1) bcast_other[4] = dim.bd;

  dEdxi.tb<4>().device(*dev.edevice) += (dEdf.tb<4>() * xs[1-i]->tb<4>().broadcast(bcast_other)).sum(red_axis).reshape(morph);
}

// ************* CwiseQuotient *************

#ifndef __CUDACC__

string CwiseQuotient::as_string(const vector<string>& arg_names) const {
  ostringstream s;
  s << arg_names[0] << " / " << arg_names[1];
  return s.str();
}

Dim CwiseQuotient::dim_forward(const vector<Dim>& xs) const {
  DYNET_ARG_CHECK(xs.size() == 2, "Failed input count check in CwiseQuotient");
  std::vector<long> dims({});
  for(unsigned int i = 0; i < min(xs[0].nd, xs[1].nd); i++){
    DYNET_ARG_CHECK(xs[0].d[i]==xs[1].d[i] ||  xs[1].d[i]==1, "CwiseQuotient: For each dimension, the dim size needs to match or the right side needs to equal 1, but got dimensions: " << xs[0] << " and " << xs[1]);
  }
  DYNET_ARG_CHECK(xs[0].bd==xs[1].bd || xs[1].bd==1, "CwiseQuotient: batch size must match or right side must equal 1");
  for(unsigned int i = 0; i < max(xs[0].nd, xs[1].nd); i++){
    if(i < min(xs[0].nd, xs[1].nd)) dims.push_back(max(xs[0].d[i], xs[1].d[i]));
    else if(i < xs[0].nd) dims.push_back(xs[0].d[i]);
    else dims.push_back(xs[1].d[i]);
  }
  Dim d(dims, max(xs[0].bd, xs[1].bd));
  return d;
}


#endif

template<class MyDevice>
void CwiseQuotient::forward_dev_impl(const MyDevice & dev, const vector<const Tensor*>& xs, Tensor& fx) const {
  DYNET_ASSERT(xs.size() == 2, "Failed dimension check in CwiseQuotient::forward (cdiv)");
  if(xs[0]->d.size() == xs[1]->d.size()){
    fx.tb<4>().device(*dev.edevice) = xs[0]->tb<4>() / xs[1]->tb<4>();
  } else {
    Eigen::array<int, 5> bcast = {1,1,1,1,1};
    for(unsigned int di = 0; di<xs[0]->d.nd; di++){
      if(xs[1]->d[di]==1) bcast[di] = xs[0]->d[di];
    }
    if(xs[1]->d.bd == 1) bcast[4] = xs[0]->d.bd;
    fx.tb<4>().device(*dev.edevice) = xs[0]->tb<4>() / xs[1]->tb<4>().broadcast(bcast);
  }
}

template<class MyDevice>
void CwiseQuotient::backward_dev_impl(const MyDevice & dev,
                             const vector<const Tensor*>& xs,
                             const Tensor& fx,
                             const Tensor& dEdf,
                             unsigned i,
                             Tensor& dEdxi) const {
  DYNET_ASSERT(i < 2, "Failed dimension check in CwiseQuotient::backward (cdiv)");
  if (i == 0) {
    if(xs[0]->d.size() == xs[1]->d.size()){
      dEdxi.tb<4>().device(*dev.edevice) += dEdf.tb<4>() / xs[1]->tb<4>();
    } else {
      Eigen::array<int, 5> bcast = {1,1,1,1,1};
      for(unsigned int di = 0; di<xs[0]->d.nd; di++){
        if(xs[0]->d[di]!=xs[1]->d[di]) bcast[di] = xs[0]->d[di];
      }
      if(xs[0]->d.bd!=xs[1]->d.bd) bcast[4] = xs[0]->d.bd;
      dEdxi.tb<4>().device(*dev.edevice) += dEdf.tb<4>() / xs[1]->tb<4>().broadcast(bcast);
    }
  } else { // i = 1
    if(xs[0]->d.size() == xs[1]->d.size()){
      dEdxi.tb<4>().device(*dev.edevice) -= (dEdf.tb<4>() / xs[1]->tb<4>().square() * xs[0]->tb<4>());
    } else {
      int n_red = xs[0]->d.bd!=xs[1]->d.bd?1:0;
      for(unsigned int di = 0; di < xs[0]->d.nd; di++) if(xs[0]->d[di]!=xs[1]->d[di]) n_red++;
      DYNET_ASSERT(n_red < 5, "Unsupported number of reductions check in CwiseQuotient::backward (cdiv)");
      if(n_red==0)      backward_helper<MyDevice, 0>(dev, xs, fx, dEdf, i, dEdxi);
      else if(n_red==1) backward_helper<MyDevice, 1>(dev, xs, fx, dEdf, i, dEdxi);
      else if(n_red==2) backward_helper<MyDevice, 2>(dev, xs, fx, dEdf, i, dEdxi);
      else if(n_red==3) backward_helper<MyDevice, 3>(dev, xs, fx, dEdf, i, dEdxi);
      else if(n_red==4) backward_helper<MyDevice, 4>(dev, xs, fx, dEdf, i, dEdxi);
    }
  }
}
DYNET_NODE_INST_DEV_IMPL(CwiseQuotient)

template<class MyDevice, int ReductionOrder>
void CwiseQuotient::backward_helper(const MyDevice & dev,
		       const std::vector<const Tensor*>& xs,
		       const Tensor& fx,
		       const Tensor& dEdf,
		       unsigned i,
		       Tensor& dEdxi) const {
  Eigen::array<int, ReductionOrder> red_axis;
  if(ReductionOrder>0) red_axis[ReductionOrder-1] = 4;
  int curr_red_axis = 0;
  for(unsigned int di = 0; di < xs[0]->d.nd; di++){
    if(xs[0]->d[di]!=xs[1]->d[di]){
      red_axis[curr_red_axis] = di;
      curr_red_axis++;
    }
  }
  Eigen::array<int, 5> morph = {1,1,1,1,1};
  for(unsigned int di = 0; di < xs[0]->d.nd; di++){
    morph[di] = xs[i]->d[di];
  }
  morph[4] = xs[i]->d.bd;
    Eigen::array<int, 5> bcast = {1,1,1,1,1};
    for(unsigned int di = 0; di < xs[0]->d.nd; di++){
      if(xs[0]->d[di]!=xs[1]->d[di]) bcast[di] = xs[0]->d[di];
    }
    if(xs[0]->d.bd!=xs[1]->d.bd) bcast[4] = xs[0]->d.bd;
    AlignedMemoryPool* scratch_allocator = fx.device->pools[(int)DeviceMempool::SCS];
    Tensor xs1_squared(xs[1]->d, nullptr, fx.device, fx.mem_pool);
    xs1_squared.v = static_cast<float*>(scratch_allocator->allocate(xs1_squared.d.size() * sizeof(float)));
    xs1_squared.tb<4>().device(*dev.edevice) = xs[1]->tb<4>().square();
    dEdxi.tb<4>().device(*dev.edevice) -= (dEdf.tb<4>() / xs1_squared.tb<4>().broadcast(bcast) * xs[0]->tb<4>()).sum(red_axis).reshape(morph);
    scratch_allocator->free();
}

// ************* Pow *************

#ifndef __CUDACC__

string Pow::as_string(const vector<string>& arg_names) const {
  ostringstream s;
  s << arg_names[0] << " ** " << arg_names[1];
  return s.str();
}

Dim Pow::dim_forward(const vector<Dim>& xs) const {
  DYNET_ARG_CHECK(xs.size() == 2, "Failed input count check in Pow")
  Dim d = xs[0].truncate();
  DYNET_ARG_CHECK(xs[1].truncate().single_batch().size() == 1, "Bad input dimensions in Pow: " << xs);
  return d;
}

#endif

template<class MyDevice>
void Pow::forward_dev_impl(const MyDevice & dev, const vector<const Tensor*>& xs, Tensor& fx) const {
  DYNET_ARG_CHECK(xs.size() == 2, "Failed dimension check in Pow::forward");
  fx.tvec().device(*dev.edevice) = xs[0]->tvec().pow(as_scalar(*xs[1]));
}

template<class MyDevice>
void Pow::backward_dev_impl(const MyDevice & dev,
                            const vector<const Tensor*>& xs,
                            const Tensor& fx,
                            const Tensor& dEdf,
                            unsigned i,
                            Tensor& dEdxi) const {
  DYNET_ARG_CHECK(xs.size() == 2, "Failed dimension check in Pow::backward");
  real x2 = as_scalar(*xs[1]);
  if (i == 0) {
    dEdxi.tvec().device(*dev.edevice) += xs[0]->tvec().pow(x2 - 1) * dEdf.tvec() * x2;
  } else {
#if defined(__CUDACC__) && defined(EIGEN_NO_MALLOC)
    DYNET_RUNTIME_ERR("CUDA memory allocation in Pow");
#endif
    // y = a^x
    // dy/dx = a^x * log(a)
    dEdxi.t<0>().device(*dev.edevice) += (fx.tvec() * xs[0]->tvec().log() * dEdf.tvec()).sum();
  }
}
DYNET_NODE_INST_DEV_IMPL(Pow)

}
