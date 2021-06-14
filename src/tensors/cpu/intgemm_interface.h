#pragma once

#include "graph/node.h"
#include "graph/node_operators_unary.h"
#include "integer_common.h"

namespace marian {

namespace cpu {
namespace integer {

#if COMPILE_CPU
/*
 * Prepare an activation matrix into intgemm8/16 format. For now the activation matrix is just quantized.
 * Expr input: The input tensor
 * bool shifted: whether we use the shifted codepath to deal with unsigned \times signed 
 */
template<Type vtype>
static inline Expr prepareA(Expr a, bool shifted=false, std::string bname="") { // @TODO check if bname is necessary
  auto nodeOp = [shifted, bname](Expr out, const std::vector<Expr>& children) {
    Expr in = children[0];
    auto quantMult = computeQuantMult<vtype>(in->val(), bname + "_quantMultA");
    typedef typename intgemm_<vtype>::type Integer;
    if (shifted)  {
      intgemm::Int8Shift::PrepareA(in->val()->data(), /*input*/
                                      out->val()->data<int8_t>(), /*output*/
                                      quantMult, /*Quant Mult*/
                                      rows(in->val()),
                                      cols(in->val()));
    } else {
      intgemm_<vtype>::width::PrepareA(in->val()->data(), /*input*/
                                      out->val()->data<Integer>(), /*output*/
                                      quantMult, /*Quant Mult*/
                                      rows(in->val()),
                                      cols(in->val()));
    }
    getQuantMult<vtype>(out->val()) = quantMult;
  };

  return lambda({a}, a->shape(), vtype, nodeOp);
}
#endif

// @TODO this is not memoised so we have the longer version further down
/*
#if COMPILE_CPU
template<Type vtype>
static inline Expr prepareB(Expr b) {
  auto nodeOp = [](Expr out, const std::vector<Expr>& children) {
    Expr in = children[0];
    typedef typename intgemm_<vtype>::type Integer;
    if (isIntgemm(in->value_type())) { // @ Does this ever get triggered?
      out->val() = in->val();
    } else {
      auto quantMult = computeQuantMult<vtype>(in->val());
      intgemm_<vtype>::width::PrepareB(in->val()->data(),
                                      out->val()->data<Integer>(),
                                      rows(in->val()),
                                      cols(in->val()));
      getQuantMult<vtype>(out->val()) = quantMult;
    }
  };
  return lambda({b}, b->shape(), vtype, nodeOp);
}
#endif
*/

template<Type vtype>
struct PrepareBNodeOp : public UnaryNodeOp {
  bool transpose_;

  PrepareBNodeOp(Expr input, bool transpose)
      : UnaryNodeOp(input, newShape(input, transpose), vtype), transpose_(transpose) {

    set_name(input->name());
    if (!transpose_) {
      ABORT_IF(input->shape()[-1] %8 != 0, "Columns of matrix: " + input->type() + " must be multiple of 8.");
    } else {
      ABORT_IF((input->shape().elements()/input->shape()[-1]) %8 != 0, "Rows of matrix: " + input->type() + " must be multiple of 8.");
    }
  }

  NodeOps forwardOps() override {
   return {NodeOp(
      typedef typename intgemm_<vtype>::type Integer;
      if (isIntgemm(child(0)->value_type())) {
        val_ = child(0)->val();
      } else if (!transpose_) {
        auto quantMult = computeQuantMult<vtype>(child(0)->val(), name());
        intgemm_<vtype>::width::PrepareB(child(0)->val()->data(), /*input*/
                                      val_->data<Integer>(), /*output*/
                                      quantMult, /*Quant Mult*/
                                      rows(child(0)->val()),
                                      cols(child(0)->val()));
        getQuantMult<vtype>(val_) = quantMult;
      } else {
        auto quantMult = computeQuantMult<vtype>(child(0)->val(), name());
        intgemm_<vtype>::width::PrepareBTransposed(child(0)->val()->data(), /*input*/
                                      val_->data<Integer>(), /*output*/
                                      quantMult,
                                      cols(child(0)->val()), /*Cols and rows need to be swapped*/
                                      rows(child(0)->val())); /*Cols and rows need to be swapped*/
        getQuantMult<vtype>(val_) = quantMult;
      }
    )};
  }

  NodeOps backwardOps() override {
    ABORT("Only used for inference");
    return {NodeOp(0)};
  }

  static Shape newShape(Expr input, bool transposed) {
    Shape ret = input->shape();
    if (transposed) {
      ret.set(0, input->shape()[-1]);
      ret.set(1, input->shape()[0]);
    } else {
      ret = input->shape();
    }
    return ret;
  }

  const std::string type() override { return "intgemmPrepareB"; }
};

template<Type vtype>
struct SelectColumnsBNodeOp : public UnaryNodeOp {
public:
  SelectColumnsBNodeOp(Expr input, const std::vector<uint_least32_t>  &indices)
      : UnaryNodeOp(input, newShape(input, indices), vtype), indices_(indices) {

    set_name(input->name());
    setMemoize(false); // Enabling memoization leads to a massive memory leak. Instead use special "midterm" memory.
                       // Still, I don't understand why setMemoize(true) still leaks.
    // Check if arguments are not null
    ABORT_IF(child(0) == nullptr, "B cannot be null");

    // Check if intgemm
    ABORT_IF(!isIntgemm(input->value_type()), "We need to prepareB before getting the indices here.");

    // Check number of selected columns
    ABORT_IF(indices.size() % 8 != 0, "Shortlist selected vocabulary must be a multiple of 8.");
  }

  NodeOps forwardOps() override {
    return {NodeOp(
      //We get the quantization multiplier from a PrepareB or directly from the input
      float quantMult = getQuantMult<vtype>(child(0)->val());
      auto input = child(0)->val();
      typedef typename intgemm_<vtype>::type Integer;
      intgemm_<vtype>::width::SelectColumnsB(
                    reinterpret_cast<Integer *>(input->data()),
                    val_->data<Integer>(),
                    rows(input),
                    &*indices_.begin(),
                    &*indices_.end());
      // Store quant mult on the node
      getQuantMult<vtype>(val_) = quantMult;
      // @TODO Store AQuantMult here as well, if precomputed alphas
    )};
  }

  const std::string type() override { return "intgemmSelectColumnsB"; }

  size_t hash() override {
    if (!hash_) {
      hash_ = NaryNodeOp::hash();
      for(auto i : indices_)
        util::hash_combine(hash_, i);
    }
    return hash_;
  }

  bool equal(Expr node) override {
    if(!NaryNodeOp::equal(node)) return false;
    auto cnode = std::dynamic_pointer_cast<SelectColumnsBNodeOp<vtype>>(node);
    if (!cnode) return false;
    return indices_ == cnode->indices_;
  }

private:
  static Shape newShape(Expr a, const std::vector<uint_least32_t>& indices) {
    Shape ret = a->shape();
    ret.set(1, indices.size());
    return ret;
  }

  std::vector<uint_least32_t> indices_;
};

// Temporary placeholder for QuantMultA for when not using precomputed alphas
template<Type vtype>
struct QuantMultANodeOp : public UnaryNodeOp {
  QuantMultANodeOp(Expr input, std::string& bname) : UnaryNodeOp(input, Shape({1}), Type::float32){
      set_name(input->name() + "_QuantMultB");
  }

  NodeOps forwardOps() override {
    return {NodeOp(
        *val_->data() = getQuantMult<vtype>(child(0));
    )};
  }

  NodeOps backwardOps() override {
    ABORT("Only used for inference");
    return {NodeOp(0)};
  }

  const std::string type() override {
      return "intgemmQuantMultA";
  }

};

template<Type vtype> // Without the template marian thinks this is an instrusive ptr, I'm not sure why.
struct PrepareBiasForBNodeOp : public NaryNodeOp {
//private:
//  ENABLE_INTRUSIVE_PTR(PrepareBiasForBNodeOp)
public:
  PrepareBiasForBNodeOp(Expr bias, Expr inputB_preppd, Expr inputA_preppd)
      : NaryNodeOp({bias, inputB_preppd, inputA_preppd}, bias->shape(), Type::float32) {

    set_name(bias->name() + "_Prepared");
    if (bias->type() == "cols" && bias->graph()->getBackend()->isPrecomputedAlpha()) {
      ABORT("We shouldn't ever be here. The bias would have been prepared by prior running select columns b");
    } else if (!bias->graph()->getBackend()->isPrecomputedAlpha()){
      setMemoize(false);
    }
  }

  PrepareBiasForBNodeOp(Expr bias, Expr inputB_preppd)
      : NaryNodeOp({bias, inputB_preppd}, bias->shape(), Type::float32) {

    set_name(bias->name() + "_Prepared");
    if (bias->type() == "cols" && bias->graph()->getBackend()->isPrecomputedAlpha()) {
      ABORT("We shouldn't ever be here. The bias would have been prepared by prior running select columns b");
    } else if (!bias->graph()->getBackend()->isPrecomputedAlpha()){
      ABORT("We can only use this codepath with precomputed alphas, as they are attached to the B node.");
    }
  }

  NodeOps forwardOps() override {
    return {NodeOp(
      auto bias = this->child(0)->val();
      auto b = this->child(1)->val();
      float quant_mult_b = getQuantMult<vtype>(child(1)->val());
      float quant_mult_a;
      if (children().size() == 3) { // Not precomputed alphas, we get the quantMult from the nodeA prepared
        quant_mult_a = getQuantMult<vtype>(child(2)->val());
      } else {
        quant_mult_a = getQuantMultA<vtype>(child(1)->val());
      }
      float unquant_mult = (-1)*((127.0f / quant_mult_a)*(127.0f / quant_mult_b))/(127.0f); //Minus one to invert add_ps later on
      intgemm::Int8Shift::PrepareBias((const int8_t *)b->data(), rows(b), cols(b), intgemm::callbacks::UnquantizeAndAddBiasAndWrite(unquant_mult, bias->data(), val_->data()));
    )};
  }

  NodeOps backwardOps() override {
    ABORT("Only used for inference");
    return {NodeOp(0)};
  }

  const std::string type() override { return "prepareBias"; }
};

template<Type vtype> // Without the template marian thinks this is an instrusive ptr, I'm not sure why.
class PrepareFakeBiasForBNodeOp : public NaryNodeOp {
public:
  PrepareFakeBiasForBNodeOp(Expr inputB_preppd, Expr inputA_preppd)
      : NaryNodeOp({inputB_preppd, inputA_preppd}, {1, inputB_preppd->shape()[-1]}, Type::float32) {

    set_name(inputB_preppd->name() + "_FakeBias");
    if (!inputB_preppd->graph()->getBackend()->isPrecomputedAlpha()) {
      setMemoize(false);
    }
  }

  PrepareFakeBiasForBNodeOp(Expr inputB_preppd)
      : NaryNodeOp({inputB_preppd}, {1, inputB_preppd->shape()[-1]}, Type::float32) {

    set_name(inputB_preppd->name() + "_FakeBias");
    if (!inputB_preppd->graph()->getBackend()->isPrecomputedAlpha()) {
      ABORT("We can only use this codepath with precomputed alphas, as they are attached to the B node.");
    }
  }

  NodeOps forwardOps() override {
    return {NodeOp(
    auto b = this->child(0)->val();
    float quant_mult_b = getQuantMult<vtype>(child(0)->val());
    float quant_mult_a;
    if (children().size() == 2) { // Not precomputed alphas
      quant_mult_a = getQuantMult<vtype>(child(1)->val());
    } else {
      quant_mult_a = getQuantMultA<vtype>(child(0)->val());
    }

    float unquant_mult = (-1)*((127.0f / quant_mult_a)*(127.0f / quant_mult_b))/(127.0f); //Minus one to invert add_ps later on
    intgemm::Int8Shift::PrepareBias((const int8_t *)b->data(), rows(b), cols(b), intgemm::callbacks::UnquantizeAndWrite(unquant_mult, val_->data()));
    )};
  }

  const std::string type() override { return "prepareFakeBias"; }
};

static Expr SelectColumnsBTyped(Expr input, const std::vector<uint_least32_t>  &indices) {
  static const Type intgemmType = cpu::integer::getIntgemmType(input->graph()->getBackend()->getGemmType());
  static const bool pass = cpu::integer::passOrAbort(intgemmType);
  pass; // We declare this variable as static so that passOrAbort is only ever run once during the initialization.
  switch(intgemmType) {
    case Type::intgemm8ssse3 :
      return Expression<SelectColumnsBNodeOp<Type::intgemm8ssse3> >(input, indices);
    case Type::intgemm8avx2 :
      return Expression<SelectColumnsBNodeOp<Type::intgemm8avx2> > (input, indices);
    case Type::intgemm8avx512 :
      return Expression<SelectColumnsBNodeOp<Type::intgemm8avx512> >(input, indices);
    case Type::intgemm8avx512vnni :
      return Expression<SelectColumnsBNodeOp<Type::intgemm8avx512vnni> > (input, indices);
    case Type::intgemm16sse2 :
      return Expression<SelectColumnsBNodeOp<Type::intgemm16sse2> >(input, indices);
    case Type::intgemm16avx2 :
      return Expression<SelectColumnsBNodeOp<Type::intgemm16avx2> > (input, indices);
    case Type::intgemm16avx512 :
      return Expression<SelectColumnsBNodeOp<Type::intgemm16avx512> > (input, indices);
    default:
      ABORT("Unsupported type {} for Intgemm type??", intgemmType);
  }
}

static Expr prepareBTyped(Expr input, bool transpose=false) {
  static const Type intgemmType = cpu::integer::getIntgemmType(input->graph()->getBackend()->getGemmType());
  static const bool pass = cpu::integer::passOrAbort(intgemmType);
  pass; // We declare this variable as static so that passOrAbort is only ever run once during the initialization.
  // Get the intgemm type the first time we run into a function, as in the future we will have the same type invocation.
  switch(intgemmType) {
    case Type::intgemm8ssse3 :
      return Expression<PrepareBNodeOp<Type::intgemm8ssse3> >(input, transpose);
    case Type::intgemm8avx2 :
      return Expression<PrepareBNodeOp<Type::intgemm8avx2> > (input, transpose);
    case Type::intgemm8avx512 :
      return Expression<PrepareBNodeOp<Type::intgemm8avx512> >(input, transpose);
    case Type::intgemm8avx512vnni :
      return Expression<PrepareBNodeOp<Type::intgemm8avx512vnni> > (input, transpose);
    case Type::intgemm16sse2 :
      return Expression<PrepareBNodeOp<Type::intgemm16sse2> >(input, transpose);
    case Type::intgemm16avx2 :
      return Expression<PrepareBNodeOp<Type::intgemm16avx2> > (input, transpose);
    case Type::intgemm16avx512 :
      return Expression<PrepareBNodeOp<Type::intgemm16avx512> > (input, transpose);
    default:
      ABORT("Unsupported type {} for Intgemm type??", intgemmType);
  }
}


static Expr PrepareTrueBiasForBTyped(Expr bias, Expr inputB_preppd, Expr inputA_preppd=nullptr) {
  static const Type intgemmType = inputB_preppd->value_type();
  if (inputA_preppd) {
    switch(intgemmType) {
      case Type::intgemm8ssse3 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm8ssse3> >(bias, inputB_preppd, inputA_preppd);
      case Type::intgemm8avx2 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm8avx2> > (bias, inputB_preppd, inputA_preppd);
      case Type::intgemm8avx512 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm8avx512> >(bias, inputB_preppd, inputA_preppd);
      case Type::intgemm8avx512vnni :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm8avx512vnni> > (bias, inputB_preppd, inputA_preppd);
      case Type::intgemm16sse2 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm16sse2> >(bias, inputB_preppd, inputA_preppd);
      case Type::intgemm16avx2 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm16avx2> > (bias, inputB_preppd, inputA_preppd);
      case Type::intgemm16avx512 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm16avx512> > (bias, inputB_preppd, inputA_preppd);
      default:
        ABORT("Unsupported type {} for Intgemm type??", intgemmType);
    }
  } else {
    switch(intgemmType) {
      case Type::intgemm8ssse3 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm8ssse3> >(bias, inputB_preppd);
      case Type::intgemm8avx2 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm8avx2> > (bias, inputB_preppd);
      case Type::intgemm8avx512 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm8avx512> >(bias, inputB_preppd);
      case Type::intgemm8avx512vnni :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm8avx512vnni> > (bias, inputB_preppd);
      case Type::intgemm16sse2 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm16sse2> >(bias, inputB_preppd);
      case Type::intgemm16avx2 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm16avx2> > (bias, inputB_preppd);
      case Type::intgemm16avx512 :
        return Expression<PrepareBiasForBNodeOp<Type::intgemm16avx512> > (bias, inputB_preppd);
      default:
        ABORT("Unsupported type {} for Intgemm type??", intgemmType);
    }
  }
}

static Expr PrepareFakeBiasForBTyped(Expr inputB_preppd, Expr inputA_preppd=nullptr) {
  static const Type intgemmType = inputB_preppd->value_type();
  if (inputA_preppd) {
    switch(intgemmType) {
      case Type::intgemm8ssse3 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm8ssse3> >(inputB_preppd, inputA_preppd);
      case Type::intgemm8avx2 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm8avx2> > (inputB_preppd, inputA_preppd);
      case Type::intgemm8avx512 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm8avx512> >(inputB_preppd, inputA_preppd);
      case Type::intgemm8avx512vnni :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm8avx512vnni> > (inputB_preppd, inputA_preppd);
      case Type::intgemm16sse2 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm16sse2> >(inputB_preppd, inputA_preppd);
      case Type::intgemm16avx2 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm16avx2> > (inputB_preppd, inputA_preppd);
      case Type::intgemm16avx512 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm16avx512> > (inputB_preppd, inputA_preppd);
      default:
        ABORT("Unsupported type {} for Intgemm type??", intgemmType);
    }
  } else {
    switch(intgemmType) {
      case Type::intgemm8ssse3 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm8ssse3> >(inputB_preppd);
      case Type::intgemm8avx2 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm8avx2> > (inputB_preppd);
      case Type::intgemm8avx512 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm8avx512> >(inputB_preppd);
      case Type::intgemm8avx512vnni :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm8avx512vnni> > (inputB_preppd);
      case Type::intgemm16sse2 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm16sse2> >(inputB_preppd);
      case Type::intgemm16avx2 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm16avx2> > (inputB_preppd);
      case Type::intgemm16avx512 :
        return Expression<PrepareFakeBiasForBNodeOp<Type::intgemm16avx512> > (inputB_preppd);
      default:
        ABORT("Unsupported type {} for Intgemm type??", intgemmType);
    }
  }
}

static Expr PrepareBiasForBTyped(Expr bias, Expr inputB_preppd, Expr inputA_preppd=nullptr) {
  if (bias) {
    return PrepareTrueBiasForBTyped(bias, inputB_preppd, inputA_preppd);
  } else {
    return PrepareFakeBiasForBTyped(inputB_preppd, inputA_preppd);
  }
}


/*	
 * This computes A*B (+ bias if available) in intgemm.	
 * Expr a: The activation matrix in intgemm format	
 * Expr b: The parameter matrix in intgemm fromat	
 * Expr bias: The bias	
 * bool transA - tranpose input A if true
 * bool transB - unused here (@TODO remove?)
 * float scale - scale the output by `scale`
 * the template argument controls whether we're doing 16bit integers or 8bit integers. 
 * It can be Type::intgemm8 or Type::intgemm16 and all hardware-specific variants	
 */
template<Type vtype>
static inline Expr affineOrDotTyped(Expr a, Expr bQuant, Expr bias, bool transA, bool /*transB*/, float scale) {
#if COMPILE_CPU
  ABORT_IF(!isFloat(a->value_type()), "Intgemm expects type of A to be float32 not {}", a->value_type());
  ABORT_IF(!isIntgemm(bQuant->value_type()), "Intgemm expects type of B to be a variant of intgemm not {}", bQuant->value_type());

  bool shifted = (a->graph()->getBackend()->isShifted() && bias) || a->graph()->getBackend()->isShiftedAll(); // We use the shifted codepath when we have a bias or shifted-all is enabled
  //static bool precomputedAlpha = a->graph()->getBackend()->isPrecomputedAlpha(); // Detect if we have precomputed alphas or not
  auto aQuant = prepareA<vtype>(transA ? transpose(a) : a, shifted, bQuant->name()); // A should not be quantized yet as seen above, hence quantize here
  
  // determine the output shape m x n for A: m x k and B: k x n
  // since we transpose A beforehand we don't need to take care of transposed shapes here 
  Shape outShape = aQuant->shape();
  outShape.set(-1, bQuant->shape()[-1]);

  if (shifted) {
    bias = PrepareBiasForBTyped(bias, bQuant, aQuant);
  }

  // wrap the multiply finctions to be executed in the forward step of a Lambda node
  auto dotOrAffineNodeOp = [=](Expr out, const std::vector<Expr>& children) {
    Expr aQuant = children[0];
    Expr bQuant = children[1];
    Expr bias   = children.size() > 2 ? children[2] : nullptr;

    // when we arrive here, A and B are already quantized, so just get the multipliers
    float aQuantMult = getQuantMult<vtype>(aQuant->val());
    float bQuantMult = getQuantMult<vtype>(bQuant->val());
        
    float unquant_mult = 1.0f / (aQuantMult * bQuantMult);
    unquant_mult = unquant_mult * scale;

    typedef typename intgemm_<vtype>::type Integer;
    if(bias) { // dispatch a multiply with integrated bias addition i.e affine(...)
      if (shifted) { // @TODO only architecture agnostic format supported for shift
        intgemm::Int8Shift::Multiply(/*A=*/aQuant->val()->data<int8_t>(),
                                     /*B=*/bQuant->val()->data<int8_t>(),
                                     rows(aQuant->val()),
                                     cols(aQuant->val()),
                                     cols(bQuant->val()),
                                     intgemm::callbacks::UnquantizeAndAddBiasAndWrite(unquant_mult, /*bias=*/bias->val()->data(), /*output=*/out->val()->data()));
      } else {
        intgemm_<vtype>::width::Multiply(/*A=*/aQuant->val()->data<Integer>(),
                                         /*B=*/bQuant->val()->data<Integer>(),
                                         rows(aQuant->val()),
                                         cols(aQuant->val()),
                                         cols(bQuant->val()),
                                         intgemm::callbacks::UnquantizeAndAddBiasAndWrite(unquant_mult, /*bias=*/bias->val()->data(), /*output=*/out->val()->data()));
      }
    } else { // dispatch a multiply without bias addition i.e dot(...)
      intgemm_<vtype>::width::Multiply(/*A=*/aQuant->val()->data<Integer>(),
                                       /*B=*/bQuant->val()->data<Integer>(),
                                       rows(aQuant->val()),
                                       cols(aQuant->val()),
                                       cols(bQuant->val()),
                                       intgemm::callbacks::UnquantizeAndWrite(unquant_mult, /*output=*/out->val()->data()));
    }
  };

  std::vector<Expr> children = {aQuant, bQuant};
  if(bias)
    children.push_back(bias);

  return lambda(children, outShape, Type::float32, dotOrAffineNodeOp); // inference-only Lambda node
#else
  a, bQuant, bias, transA, scale;
  ABORT("You need to enable CPU compilation to use this feature. Use cmake .. -DCOMPILE_CPU=ON");
#endif
}

// Dispatch correct hardware-agnostic or hardware-specific matrix multiplies
static inline Expr affineOrDot(Expr a, Expr bQuant, Expr bias, bool transA, bool transB, float scale) {
  Type bQuantElementType = bQuant->value_type();
  static const bool pass = cpu::integer::passOrAbort(bQuantElementType);
  pass; // We declare this variable as static so that passOrAbort is only ever run once during the initialization.
  switch(bQuantElementType) {
    //case Type::intgemm8 :  // The generic case selects CPU automatically, but we set all the types manually anyways.
    //  return cpu::integer::affineOrDotTyped<Type::intgemm8>(a, bQuant, bias, transA, transB, scale);    
    case Type::intgemm8ssse3 :
      return cpu::integer::affineOrDotTyped<Type::intgemm8ssse3>(a, bQuant, bias, transA, transB, scale);
    case Type::intgemm8avx2 :
      return cpu::integer::affineOrDotTyped<Type::intgemm8avx2>(a, bQuant, bias, transA, transB, scale);
    case Type::intgemm8avx512 :
      return cpu::integer::affineOrDotTyped<Type::intgemm8avx512>(a, bQuant, bias, transA, transB, scale);
    case Type::intgemm8avx512vnni :
      return cpu::integer::affineOrDotTyped<Type::intgemm8avx512vnni>(a, bQuant, bias, transA, transB, scale);
    //case Type::intgemm16 :  // The generic case selects CPU automatically, but we set all the types manually anyways.
    //  return cpu::integer::affineOrDotTyped<Type::intgemm16>(a, bQuant, bias, transA, transB, scale);
    case Type::intgemm16sse2 :
      return cpu::integer::affineOrDotTyped<Type::intgemm16sse2>(a, bQuant, bias, transA, transB, scale);
    case Type::intgemm16avx2 :
      return cpu::integer::affineOrDotTyped<Type::intgemm16avx2>(a, bQuant, bias, transA, transB, scale);
    case Type::intgemm16avx512 :
      return cpu::integer::affineOrDotTyped<Type::intgemm16avx512>(a, bQuant, bias, transA, transB, scale);
    default:
      ABORT("Unsupported type {} for Intgemm type??", bQuantElementType);
  }
}

}  // namespace integer
}  // namespace cpu
}  // namespace marian
