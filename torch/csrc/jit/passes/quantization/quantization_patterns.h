#pragma once

#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/ir/subgraph_matcher.h>
#include <torch/csrc/jit/passes/subgraph_rewrite.h>
#include <string>
#include <unordered_map>

namespace torch {
namespace jit {

struct QuantFusionInfo {
  std::string quantized_op_name;
  std::string pattern;
  std::string replacement;
  std::function<
      bool(const Match&, const std::unordered_map<std::string, Value*>&)>
      filter =
          [](const Match&, const std::unordered_map<std::string, Value*>&) {
            return true;
          };
};

namespace {
std::string getExtraArgList(std::vector<std::string> extra_args) {
  return std::accumulate(
      extra_args.begin(),
      extra_args.end(),
      std::string(),
      [](std::string acc, const std::string& arg) { return acc + ", " + arg; });
}

// Get the pattern we want to replace the match with
std::string getAtenOpPattern(
    const std::string& graph_header,
    const std::string& op_name,
    const std::string& extra_arg_list) {
  std::string aten_op_pattern = graph_header;
  aten_op_pattern += R"(
          %r = )";
  aten_op_pattern += op_name + "(" + "%a_quant" + extra_arg_list + ")";
  aten_op_pattern += R"(
          return (%r) )";
  return aten_op_pattern;
}

// Patterns for the ops that inherit parameters from input
QuantFusionInfo getInputTensorQParamOpFusionInfo(
    const std::string& op_name,
    const std::vector<std::string>& extra_args) {
  const auto& extra_arg_list = getExtraArgList(extra_args);
  std::string graph_header = "graph(%a_quant" + extra_arg_list + "):";
  std::string op_pattern = graph_header;
  op_pattern += R"(
          %a_dequant = aten::dequantize(%a_quant)
          %r = )";
  op_pattern += op_name + "(" + "%a_dequant" + extra_arg_list + ")";
  // IR pattern common to all ops that inherit qparam from input
  op_pattern += R"(
          %r_scale : float = aten::q_scale(%a_quant)
          %r_zero_point : int = aten::q_zero_point(%a_quant)
          %r_dtype : int = prim::dtype(%a_quant)
          %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
          return (%r_quant) )";

  std::string aten_op_pattern =
      getAtenOpPattern(graph_header, op_name, extra_arg_list);

  return {op_name, op_pattern, aten_op_pattern};
}

// Patterns for the ops that has fixed quantization parameters
QuantFusionInfo getFixedQParamOpFusionInfo(
    const std::string& op_name,
    const std::vector<std::string>& extra_args,
    bool is_symmetric) {
  const auto& extra_arg_list = getExtraArgList(extra_args);
  std::string graph_header = "graph(%a_quant" + extra_arg_list + "):";
  std::string op_pattern = graph_header;
  op_pattern += R"(
          %a_dequant = aten::dequantize(%a_quant)
          %r = )";
  op_pattern += op_name + "(" + "%a_dequant" + extra_arg_list + ")";
  // IR pattern common to all ops with fixed quantization parameters for
  // asymetric quantization
  std::string asym_fixed_qparam_op_suffix = R"(
          %r_scale : float = prim::Constant[value=0.00390625]()
          %r_zero_point : int = prim::Constant[value=0]()
          %r_dtype : int = prim::Constant[value=13]()
          %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
          return (%r_quant) )";

  std::string sym_fixed_qparam_op_suffix = R"(
          %r_scale : float = prim::Constant[value=0.0078125]()
          %r_zero_point : int = prim::Constant[value=128]()
          %r_dtype : int = prim::Constant[value=13]()
          %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
          return (%r_quant) )";
  op_pattern +=
      is_symmetric ? sym_fixed_qparam_op_suffix : asym_fixed_qparam_op_suffix;

  std::string aten_op_pattern =
      getAtenOpPattern(graph_header, op_name, extra_arg_list);

  return {op_name, op_pattern, aten_op_pattern};
}

} // namespace

std::vector<QuantFusionInfo> quant_fusion_pattern_and_replacements() {
  // aten::conv1d
  std::string conv1d = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::conv1d_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %r = aten::conv1d(%a_dequant, %w_dequant, %b, %stride, %padding, %dilation, %groups)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  // aten::conv1d - aten::relu
  std::string conv1d_relu = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::conv1d_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %conv_out = aten::conv1d(%a_dequant, %w_dequant, %b, %stride, %padding, %dilation, %groups)
        %r = aten::relu(%conv_out)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  // aten::conv1d - aten::relu_
  std::string conv1d_inplace_relu = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::conv1d_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %conv_out = aten::conv1d(%a_dequant, %w_dequant, %b, %stride, %padding, %dilation, %groups)
        %r = aten::relu_(%conv_out)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  // quantized::conv1d
  std::string quantized_conv1d = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %r_quant = quantized::conv1d(%a_quant, %packed_params, %r_scale, %r_zero_point)
        return (%r_quant) )";

  // quantized::conv1d_relu
  std::string quantized_conv1d_relu = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %r_quant = quantized::conv1d_relu(%a_quant, %packed_params, %r_scale, %r_zero_point)
        return (%r_quant) )";

  // aten::conv2d
  std::string conv2d = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::conv2d_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %r = aten::conv2d(%a_dequant, %w_dequant, %b, %stride, %padding, %dilation, %groups)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  // aten::conv2d - aten::relu
  std::string conv2d_relu = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::conv2d_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %conv_out = aten::conv2d(%a_dequant, %w_dequant, %b, %stride, %padding, %dilation, %groups)
        %r = aten::relu(%conv_out)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  // aten::conv2d - aten::relu_
  std::string conv2d_inplace_relu = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::conv2d_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %conv_out = aten::conv2d(%a_dequant, %w_dequant, %b, %stride, %padding, %dilation, %groups)
        %r = aten::relu_(%conv_out)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  // quantized::conv2d
  std::string quantized_conv2d = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %r_quant = quantized::conv2d(%a_quant, %packed_params, %r_scale, %r_zero_point)
        return (%r_quant) )";

  // quantized::conv2d_relu
  std::string quantized_conv2d_relu = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %r_quant = quantized::conv2d_relu(%a_quant, %packed_params, %r_scale, %r_zero_point)
        return (%r_quant) )";

  // aten::conv3d
  std::string conv3d = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::conv3d_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %r = aten::conv3d(%a_dequant, %w_dequant, %b, %stride, %padding, %dilation, %groups)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  // aten::conv3d - aten::relu
  std::string conv3d_relu = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::conv3d_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %conv_out = aten::conv3d(%a_dequant, %w_dequant, %b, %stride, %padding, %dilation, %groups)
        %r = aten::relu(%conv_out)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  // aten::conv3d - aten::relu_
  std::string conv3d_inplace_relu = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::conv3d_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %conv_out = aten::conv3d(%a_dequant, %w_dequant, %b, %stride, %padding, %dilation, %groups)
        %r = aten::relu_(%conv_out)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  // quantized::conv3d
  std::string quantized_conv3d = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %r_quant = quantized::conv3d(%a_quant, %packed_params, %r_scale, %r_zero_point)
        return (%r_quant) )";

  // quantized::conv3d_relu
  std::string quantized_conv3d_relu = R"(
graph(%a_quant, %packed_params, %r_scale, %r_zero_point, %r_dtype, %stride, %padding, %dilation, %groups):
        %r_quant = quantized::conv3d_relu(%a_quant, %packed_params, %r_scale, %r_zero_point)
        return (%r_quant) )";

  std::string add_relu = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %alpha = prim::Constant[value=1]()
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_add = aten::add(%a_dequant, %b_dequant, %alpha)
         %r_relu = aten::relu(%r_add)
         %r = aten::quantize_per_tensor(%r_relu, %scale, %zero_point, %dtype)
         return (%r) )";

  std::string add_inplace_relu = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %alpha = prim::Constant[value=1]()
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_add = aten::add(%a_dequant, %b_dequant, %alpha)
         %r_relu = aten::relu_(%r_add)
         %r = aten::quantize_per_tensor(%r_relu, %scale, %zero_point, %dtype)
         return (%r) )";

  std::string inplace_add_relu = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %alpha = prim::Constant[value=1]()
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_add = aten::add_(%a_dequant, %b_dequant, %alpha)
         %r_relu = aten::relu(%r_add)
         %r = aten::quantize_per_tensor(%r_relu, %scale, %zero_point, %dtype)
         return (%r) )";

  std::string inplace_add_inplace_relu = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %alpha = prim::Constant[value=1]()
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_add = aten::add_(%a_dequant, %b_dequant, %alpha)
         %r_relu = aten::relu_(%r_add)
         %r = aten::quantize_per_tensor(%r_relu, %scale, %zero_point, %dtype)
         return (%r) )";

  std::string quantized_add_relu = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %r = quantized::add_relu(%a_quant, %b_quant, %scale, %zero_point)
         return (%r) )";

  // aten::linear
  std::string linear = R"(
graph(%packed_params, %a_quant, %r_scale, %r_zero_point, %r_dtype):
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::linear_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %r = aten::linear(%a_dequant, %w_dequant, %b)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  // quantized::linear
  std::string quantized_linear = R"(
graph(%packed_params, %a_quant, %r_scale, %r_zero_point, %r_dtype):
        %r = quantized::linear(%a_quant, %packed_params, %r_scale, %r_zero_point)
        return (%r) )";

  std::string cat = R"(
graph(%input_quant, %dim, %r_scale, %r_zero_point, %r_dtype):
        %input_dequant = aten::dequantize(%input_quant)
        %r = aten::cat(%input_dequant, %dim)
        %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
        return (%r_quant) )";

  std::string quantized_cat = R"(
graph(%input_quant, %dim, %r_scale, %r_zero_point, %r_dtype):
         %r_quant = quantized::cat(%input_quant, %dim, %r_scale, %r_zero_point)
         return (%r_quant) )";

  // aten::add
  std::string add = R"(
graph(%a_quant, %b_quant, %alpha, %scale, %zero_point, %dtype):
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_add = aten::add(%a_dequant, %b_dequant, %alpha)
         %r = aten::quantize_per_tensor(%r_add, %scale, %zero_point, %dtype)
         return (%r) )";

  // TODO: add %dtype after when https://github.com/pytorch/pytorch/issues/34351
  // is fixed
  // quantized::add
  std::string quantized_add = R"(
graph(%a_quant, %b_quant, %alpha, %scale, %zero_point, %dtype):
         %r = quantized::add(%a_quant, %b_quant, %scale, %zero_point)
         return (%r) )";

  auto add_filter = [](const Match& match,
                       const std::unordered_map<std::string, Value*>& vmap) {
    const auto& match_vmap = match.values_map;
    auto alpha = toIValue(match_vmap.at(vmap.at("alpha")));
    return alpha && alpha->isInt() && alpha->toInt() == 1;
  };

  // aten::add_
  std::string inplace_add = R"(
graph(%a_quant, %b_quant, %alpha, %scale, %zero_point, %dtype):
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_add = aten::add_(%a_dequant, %b_dequant, %alpha)
         %r = aten::quantize_per_tensor(%r_add, %scale, %zero_point, %dtype)
         return (%r) )";

  // quantized::add_scalar
  std::string add_scalar = R"(
graph(%a_quant, %b_scalar, %alpha):
         %a_dequant = aten::dequantize(%a_quant)
         %r = aten::add(%a_dequant, %b_scalar, %alpha)
         return (%r) )";

  std::string quantized_add_scalar = R"(
graph(%a_quant, %b_scalar, %alpha):
         %r = quantized::add_scalar(%a_quant, %b_scalar)
         return (%r) )";

  // filter that checks %alpha is constant 1 and %b_scalar is a scalar
  auto add_scalar_filter =
      [](const Match& match,
         const std::unordered_map<std::string, Value*>& vmap) {
        const auto& match_vmap = match.values_map;
        auto alpha = toIValue(match_vmap.at(vmap.at("alpha")));
        auto b_scalar = match_vmap.at(vmap.at("b_scalar"));
        return alpha && alpha->isInt() && alpha->toInt() == 1 &&
            b_scalar->type()->isSubtypeOf(NumberType::get());
      };

  // quantized::add_scalar_out
  std::string inplace_add_scalar = R"(
graph(%a_quant, %b_scalar, %alpha):
         %a_dequant = aten::dequantize(%a_quant)
         %r = aten::add_(%a_dequant, %b_scalar, %alpha)
         return (%r) )";

  std::string quantized_add_scalar_out = R"(
graph(%a_quant, %b_scalar, %alpha):
         %r = quantized::add_scalar_out(%a_quant, %b_scalar, %a_quant)
         return (%r) )";

  // quantized::add_scalar_relu
  std::string add_scalar_relu = R"(
graph(%a_quant, %b_scalar, %alpha):
         %a_dequant = aten::dequantize(%a_quant)
         %r_add = aten::add(%a_dequant, %b_scalar, %alpha)
         %r = aten::relu(%r_add)
         return (%r) )";

  std::string add_scalar_inplace_relu = R"(
graph(%a_quant, %b_scalar, %alpha):
         %a_dequant = aten::dequantize(%a_quant)
         %r_add = aten::add(%a_dequant, %b_scalar, %alpha)
         %r = aten::relu_(%r_add)
         return (%r) )";

  std::string quantized_add_scalar_relu = R"(
graph(%a_quant, %b_scalar, %alpha):
         %r = quantized::add_scalar_relu(%a_quant, %b_scalar)
         return (%r) )";

  // quantized::add_scalar_relu_out
  std::string inplace_add_scalar_relu = R"(
graph(%a_quant, %b_scalar, %alpha):
         %a_dequant = aten::dequantize(%a_quant)
         %r_add = aten::add_(%a_dequant, %b_scalar, %alpha)
         %r = aten::relu(%r_add)
         return (%r) )";

  std::string inplace_add_scalar_inplace_relu = R"(
graph(%a_quant, %b_scalar, %alpha):
         %a_dequant = aten::dequantize(%a_quant)
         %r_add = aten::add_(%a_dequant, %b_scalar, %alpha)
         %r = aten::relu_(%r_add)
         return (%r) )";

  std::string quantized_add_scalar_relu_out = R"(
graph(%a_quant, %b_scalar, %alpha):
         %r = quantized::add_scalar_relu_out(%a_quant, %b_scalar, %a_quant)
         return (%r) )";

  // quantized::batch_norm
  std::string batch_norm2d = R"(
graph(%a_quant, %weight, %bias, %mean, %var, %training, %eaf, %eps, %7, %scale, %zero_point, %scalar_type):
         %a_dequant = aten::dequantize(%a_quant)
         %r_bn = aten::batch_norm(%a_dequant, %weight, %bias, %mean, %var, %training, %eaf, %eps, %7)
         %r = aten::quantize_per_tensor(%r_bn, %scale, %zero_point, %scalar_type)
         return (%r) )";
  std::string quantized_batch_norm2d = R"(
graph(%a_quant, %weight, %bias, %mean, %var, %training, %eaf, %eps, %7, %scale, %zero_point, %scalar_type):
         %r = quantized::batch_norm2d(%a_quant, %weight, %bias, %mean, %var, %eps, %scale, %zero_point)
         return (%r) )";

  std::string batch_norm2d_relu = R"(
graph(%a_quant, %weight, %bias, %mean, %var, %training, %eaf, %eps, %7, %scale, %zero_point, %scalar_type):
         %a_dequant = aten::dequantize(%a_quant)
         %bn_out = aten::batch_norm(%a_dequant, %weight, %bias, %mean, %var, %training, %eaf, %eps, %7)
         %relu = aten::relu(%bn_out)
         %r = aten::quantize_per_tensor(%relu, %scale, %zero_point, %scalar_type)
         return (%r) )";
  std::string batch_norm2d_inplace_relu = R"(
graph(%a_quant, %weight, %bias, %mean, %var, %training, %eaf, %eps, %7, %scale, %zero_point, %scalar_type):
         %a_dequant = aten::dequantize(%a_quant)
         %bn_out = aten::batch_norm(%a_dequant, %weight, %bias, %mean, %var, %training, %eaf, %eps, %7)
         %relu = aten::relu_(%bn_out)
         %r = aten::quantize_per_tensor(%relu, %scale, %zero_point, %scalar_type)
         return (%r) )";

  std::string quantized_batch_norm2d_relu = R"(
graph(%a_quant, %weight, %bias, %mean, %var, %training, %eaf, %eps, %7, %scale, %zero_point, %scalar_type):
         %r = quantized::batch_norm2d_relu(%a_quant, %weight, %bias, %mean, %var, %eps, %scale, %zero_point)
         return (%r) )";

  // aten::mul
  std::string mul = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_mul = aten::mul(%a_dequant, %b_dequant)
         %r = aten::quantize_per_tensor(%r_mul, %scale, %zero_point, %dtype)
         return (%r) )";

  // aten::mul_
  std::string inplace_mul = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_mul = aten::mul_(%a_dequant, %b_dequant)
         %r = aten::quantize_per_tensor(%r_mul, %scale, %zero_point, %dtype)
         return (%r) )";

  // quantized::mul
  std::string quantized_mul = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %r = quantized::mul(%a_quant, %b_quant, %scale, %zero_point)
         return (%r) )";

  // quantized::mul_scalar
  std::string mul_scalar = R"(
graph(%a_quant, %b_scalar):
         %a_dequant = aten::dequantize(%a_quant)
         %r = aten::mul(%a_dequant, %b_scalar)
         return (%r) )";

  std::string inplace_mul_scalar = R"(
graph(%a_quant, %b_scalar):
         %a_dequant = aten::dequantize(%a_quant)
         %r = aten::mul_(%a_dequant, %b_scalar)
         return (%r) )";

  std::string quantized_mul_scalar = R"(
graph(%a_quant, %b_scalar):
         %r = quantized::mul_scalar(%a_quant, %b_scalar)
         return (%r) )";

  std::string quantized_mul_scalar_out = R"(
graph(%a_quant, %b_scalar):
         %r = quantized::mul_scalar_out(%a_quant, %b_scalar, %a_quant)
         return (%r) )";

  // filter that checks %b_scalar is a scalar
  auto mul_scalar_filter =
      [](const Match& match,
         const std::unordered_map<std::string, Value*>& vmap) {
        const auto& match_vmap = match.values_map;
        auto b_scalar = match_vmap.at(vmap.at("b_scalar"));
        return b_scalar->type()->isSubtypeOf(NumberType::get());
      };

  // quantized::mul_relu
  std::string mul_relu = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_mul = aten::mul(%a_dequant, %b_dequant)
         %r_relu = aten::relu(%r_mul)
         %r = aten::quantize_per_tensor(%r_relu, %scale, %zero_point, %dtype)
         return (%r) )";

  std::string mul_inplace_relu = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_mul = aten::mul(%a_dequant, %b_dequant)
         %r_relu = aten::relu_(%r_mul)
         %r = aten::quantize_per_tensor(%r_relu, %scale, %zero_point, %dtype)
         return (%r) )";

  std::string inplace_mul_relu = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_mul = aten::mul_(%a_dequant, %b_dequant)
         %r_relu = aten::relu(%r_mul)
         %r = aten::quantize_per_tensor(%r_relu, %scale, %zero_point, %dtype)
         return (%r) )";

  std::string inplace_mul_inplace_relu = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %a_dequant = aten::dequantize(%a_quant)
         %b_dequant = aten::dequantize(%b_quant)
         %r_mul = aten::mul_(%a_dequant, %b_dequant)
         %r_relu = aten::relu_(%r_mul)
         %r = aten::quantize_per_tensor(%r_relu, %scale, %zero_point, %dtype)
         return (%r) )";

  std::string quantized_mul_relu = R"(
graph(%a_quant, %b_quant, %scale, %zero_point, %dtype):
         %r = quantized::mul_relu(%a_quant, %b_quant, %scale, %zero_point)
         return (%r) )";

  // quantized::mul_scalar_relu
  std::string mul_scalar_relu = R"(
graph(%a_quant, %b_scalar):
         %a_dequant = aten::dequantize(%a_quant)
         %r_mul = aten::mul(%a_dequant, %b_scalar)
         %r = aten::relu(%r_mul)
         return (%r) )";

  std::string mul_scalar_inplace_relu = R"(
graph(%a_quant, %b_scalar):
         %a_dequant = aten::dequantize(%a_quant)
         %r_mul = aten::mul(%a_dequant, %b_scalar)
         %r = aten::relu_(%r_mul)
         return (%r) )";

  std::string quantized_mul_scalar_relu = R"(
graph(%a_quant, %b_scalar):
         %r = quantized::mul_scalar_relu(%a_quant, %b_scalar)
         return (%r) )";

  // quantized::mul_scalar_relu_out
  std::string inplace_mul_scalar_relu = R"(
graph(%a_quant, %b_scalar):
         %a_dequant = aten::dequantize(%a_quant)
         %r_mul = aten::mul_(%a_dequant, %b_scalar)
         %r = aten::relu(%r_mul)
         return (%r) )";

  std::string inplace_mul_scalar_inplace_relu = R"(
graph(%a_quant, %b_scalar):
         %a_dequant = aten::dequantize(%a_quant)
         %r_mul = aten::mul_(%a_dequant, %b_scalar)
         %r = aten::relu_(%r_mul)
         return (%r) )";

  std::string quantized_mul_scalar_relu_out = R"(
graph(%a_quant, %b_scalar):
         %r = quantized::mul_scalar_relu_out(%a_quant, %b_scalar, %a_quant)
         return (%r) )";

  // quantized::hardswish
  std::string hardswish = R"(
graph(%a_quant, %r_scale, %r_zero_point, %r_dtype):
         %a_dequant = aten::dequantize(%a_quant)
         %r = aten::hardswish(%a_dequant)
         %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
         return (%r_quant) )";

  std::string quantized_hardswish = R"(
graph(%a_quant, %r_scale, %r_zero_point, %r_dtype):
         %r_quant = quantized::hardswish(%a_quant, %r_scale, %r_zero_point)
         return (%r_quant) )";

  // quantized::layer_norm
  std::string layer_norm = R"(
graph(%a_quant, %normalized_shape, %weight, %bias, %eps, %cudnn_enabled, %output_scale, %output_zero_point, %scalar_type):
         %a_dequant = aten::dequantize(%a_quant)
         %r_ln = aten::layer_norm(%a_dequant, %normalized_shape, %weight, %bias, %eps, %cudnn_enabled)
         %r = aten::quantize_per_tensor(%r_ln, %output_scale, %output_zero_point, %scalar_type)
         return (%r) )";

  std::string quantized_layer_norm = R"(
graph(%a_quant, %normalized_shape, %weight, %bias, %eps, %cudnn_enabled, %output_scale, %output_zero_point, %scalar_type):
         %r = quantized::layer_norm(%a_quant, %normalized_shape, %weight, %bias, %eps, %output_scale, %output_zero_point)
         return (%r) )";

  // ============= General Ops that inherit quantization paramters from input
  // tensor =============
  auto avg_pool1d = getInputTensorQParamOpFusionInfo(
      "aten::avg_pool1d",
      {"%kernel_size",
       "%stride",
       "%padding",
       "%ceil_mode",
       "%count_include_pad"});

  auto avg_pool2d = getInputTensorQParamOpFusionInfo(
      "aten::avg_pool2d",
      {"%kernel_size",
       "%stride",
       "%padding",
       "%ceil_mode",
       "%count_include_pad",
       "%divisor_override"});

  std::string common_general_value_op = R"(
          %r_scale : float = aten::q_scale(%a_quant)
          %r_zero_point : int = aten::q_zero_point(%a_quant)
          %r_dtype : int = prim::dtype(%a_quant)
          %r_quant = aten::quantize_per_tensor(%r, %r_scale, %r_zero_point, %r_dtype)
          return (%r_quant) )";

  auto avg_pool3d = getInputTensorQParamOpFusionInfo(
      "aten::avg_pool3d",
      {"%kernel_size",
       "%stride",
       "%padding",
       "%ceil_mode",
       "%count_include_pad",
       "%divisor_override"});

  auto adaptive_avg_pool1d = getInputTensorQParamOpFusionInfo(
      "aten::adaptive_avg_pool1d", {"%output_size"});

  auto adaptive_avg_pool2d = getInputTensorQParamOpFusionInfo(
      "aten::adaptive_avg_pool2d", {"%output_size"});

  auto adaptive_avg_pool3d = getInputTensorQParamOpFusionInfo(
      "aten::adaptive_avg_pool3d", {"%output_size"});

  auto mean = getInputTensorQParamOpFusionInfo("aten::mean", {"%dim"});

  auto upsample_nearest1d = getInputTensorQParamOpFusionInfo(
      "aten::upsample_nearest1d", {"%output_size", "%scales"});

  auto upsample_nearest2d = getInputTensorQParamOpFusionInfo(
      "aten::upsample_nearest2d", {"%output_size", "%scale_h", "%scale_w"});

  auto upsample_nearest3d = getInputTensorQParamOpFusionInfo(
      "aten::upsample_nearest3d",
      {"%output_size", "%scale_d", "%scale_h", "%scale_w"});

  auto upsample_linear1d = getInputTensorQParamOpFusionInfo(
      "aten::upsample_linear1d", {"%output_size", "%align_corners", "%scales"});

  auto upsample_bilinear2d = getInputTensorQParamOpFusionInfo(
      "aten::upsample_bilinear2d",
      {"%output_size", "%align_corners", "%scale_h", "%scale_w"});

  auto upsample_trilinear3d = getInputTensorQParamOpFusionInfo(
      "aten::upsample_trilinear3d",
      {"%output_size", "%align_corners", "%scale_d", "%scale_h", "%scale_w"});

  auto clamp =
      getInputTensorQParamOpFusionInfo("aten::clamp", {"%min", "%max"});

  auto clamp_ =
      getInputTensorQParamOpFusionInfo("aten::clamp_", {"%min", "%max"});

  auto hardtanh =
      getInputTensorQParamOpFusionInfo("aten::hardtanh", {"%min", "%max"});

  auto hardtanh_ =
      getInputTensorQParamOpFusionInfo("aten::hardtanh_", {"%min", "%max"});

  auto elu = getInputTensorQParamOpFusionInfo(
      "aten::elu", {"%alpha", "%scale", "%input_scale"});

  auto elu_ = getInputTensorQParamOpFusionInfo(
      "aten::elu_", {"%alpha", "%scale", "%input_scale"});

  auto leaky_relu =
      getInputTensorQParamOpFusionInfo("aten::leaky_relu", {"%negative_slope"});

  auto leaky_relu_ = getInputTensorQParamOpFusionInfo(
      "aten::leaky_relu_", {"%negative_slope"});

  // Ops with fixed quantization parameters
  auto hardsigmoid = getFixedQParamOpFusionInfo("aten::hardsigmoid", {}, false);

  auto hardsigmoid_ =
      getFixedQParamOpFusionInfo("aten::hardsigmoid_", {}, false);

  auto sigmoid = getFixedQParamOpFusionInfo("aten::sigmoid", {}, false);

  auto sigmoid_ = getFixedQParamOpFusionInfo("aten::sigmoid_", {}, false);

  auto tanh = getFixedQParamOpFusionInfo("aten::tanh", {}, true);

  auto tanh_ = getFixedQParamOpFusionInfo("aten::tanh_", {}, true);

  return {
      {"quantized::conv1d", conv1d, quantized_conv1d},
      {"quantized::conv1d_relu", conv1d_relu, quantized_conv1d_relu},
      {"quantized::conv1d_relu", conv1d_inplace_relu, quantized_conv1d_relu},
      {"quantized::conv2d", conv2d, quantized_conv2d},
      {"quantized::conv2d_relu", conv2d_relu, quantized_conv2d_relu},
      {"quantized::conv2d_relu", conv2d_inplace_relu, quantized_conv2d_relu},
      {"quantized::conv3d", conv3d, quantized_conv3d},
      {"quantized::conv3d_relu", conv3d_relu, quantized_conv3d_relu},
      {"quantized::conv3d_relu", conv3d_inplace_relu, quantized_conv3d_relu},
      {"quantized::linear", linear, quantized_linear},
      {"quantized::add_relu", add_relu, quantized_add_relu, add_filter},
      {"quantized::add_relu", add_inplace_relu, quantized_add_relu, add_filter},
      {"quantized::add_relu", inplace_add_relu, quantized_add_relu, add_filter},
      {"quantized::add_relu",
       inplace_add_inplace_relu,
       quantized_add_relu,
       add_filter},
      // note that this must come before quantized::add_scalar
      {"quantized::add_scalar_relu",
       add_scalar_relu,
       quantized_add_scalar_relu,
       add_scalar_filter},
      {"quantized::add_scalar_relu",
       add_scalar_inplace_relu,
       quantized_add_scalar_relu,
       add_scalar_filter},
      {"quantized::add_scalar_relu_out",
       inplace_add_scalar_relu,
       quantized_add_scalar_relu_out,
       add_scalar_filter},
      {"quantized::add_scalar_relu_out",
       inplace_add_scalar_inplace_relu,
       quantized_add_scalar_relu_out,
       add_scalar_filter},
      {"quantized::add_scalar",
       add_scalar,
       quantized_add_scalar,
       add_scalar_filter},
      {"quantized::add_scalar_out",
       inplace_add_scalar,
       quantized_add_scalar_out,
       add_scalar_filter},
      {"quantized::add", add, quantized_add, add_filter},
      {"quantized::add", inplace_add, quantized_add, add_filter},
      {"quantized::cat", cat, quantized_cat},
      {"quantized::batch_norm2d", batch_norm2d, quantized_batch_norm2d},
      {"quantized::batch_norm2d_relu",
       batch_norm2d_relu,
       quantized_batch_norm2d_relu},
      {"quantized::batch_norm2d_relu",
       batch_norm2d_inplace_relu,
       quantized_batch_norm2d_relu},
      {"quantized::mul_scalar_relu",
       mul_scalar_relu,
       quantized_mul_scalar_relu,
       mul_scalar_filter},
      {"quantized::mul_scalar_relu",
       mul_scalar_inplace_relu,
       quantized_mul_scalar_relu,
       mul_scalar_filter},
      {"quantized::mul_scalar_relu_out",
       inplace_mul_scalar_relu,
       quantized_mul_scalar_relu_out,
       mul_scalar_filter},
      {"quantized::mul_scalar_relu_out",
       inplace_mul_scalar_inplace_relu,
       quantized_mul_scalar_relu_out,
       mul_scalar_filter},
      {"quantized::mul_scalar",
       mul_scalar,
       quantized_mul_scalar,
       mul_scalar_filter},
      {"quantized::mul_scalar",
       inplace_mul_scalar,
       quantized_mul_scalar_out,
       mul_scalar_filter},
      {"quantized::mul_relu", mul_relu, quantized_mul_relu},
      {"quantized::mul_relu", mul_inplace_relu, quantized_mul_relu},
      {"quantized::mul_relu", inplace_mul_relu, quantized_mul_relu},
      {"quantized::mul_relu", inplace_mul_inplace_relu, quantized_mul_relu},
      {"quantized::mul", mul, quantized_mul},
      {"quantized::mul", inplace_mul, quantized_mul},
      {"quantized::hardswish", hardswish, quantized_hardswish},
      {"quantized::layer_norm", layer_norm, quantized_layer_norm},
      avg_pool1d,
      avg_pool2d,
      avg_pool3d,
      adaptive_avg_pool1d,
      adaptive_avg_pool2d,
      adaptive_avg_pool3d,
      mean,
      upsample_nearest1d,
      upsample_nearest2d,
      upsample_nearest3d,
      upsample_linear1d,
      upsample_bilinear2d,
      upsample_trilinear3d,
      clamp,
      clamp_,
      hardtanh,
      hardtanh_,
      elu,
      elu_,
      leaky_relu,
      leaky_relu_,
      // fixed qparam ops
      hardsigmoid,
      hardsigmoid_,
      sigmoid,
      sigmoid_,
      tanh,
      tanh_,
  };
}

std::vector<QuantFusionInfo> dynamic_quant_fusion_pattern_and_replacements() {
  std::string linear_dynamic = R"(
graph(%packed_params, %a, %reduce_range, %a_dtype):
        %a_scale : float, %a_zero_point : int = aten::_choose_qparams_per_tensor(%a, %reduce_range)
        %a_quant = aten::quantize_per_tensor(%a, %a_scale, %a_zero_point, %a_dtype)
        %a_dequant = aten::dequantize(%a_quant)
        %w_quant : Tensor, %b : Tensor? = quantized::linear_unpack(%packed_params)
        %w_dequant = aten::dequantize(%w_quant)
        %r = aten::linear(%a_dequant, %w_dequant, %b)
        return (%r) )";

  std::string quantized_linear_dynamic = R"(
graph(%packed_params, %a, %reduce_range, %a_dtype):
        %r = quantized::linear_dynamic(%a, %packed_params)
        return (%r) )";
  return {
      {"quantized::linear_dynamic", linear_dynamic, quantized_linear_dynamic},
  };
}

} // namespace jit
} // namespace torch
