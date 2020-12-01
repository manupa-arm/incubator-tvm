/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file source_module.cc
 * \brief Source code module, only for viewing
 */
#include <tvm/runtime/ndarray.h>
#include <tvm/runtime/packed_func.h>
#include <tvm/runtime/registry.h>

#include "../../runtime/file_utils.h"
#include "../../runtime/meta_data.h"
#include "../../support/str_escape.h"
#include "../func_registry_generator.h"
#include "codegen_source_base.h"

namespace tvm {
namespace codegen {

using runtime::PackedFunc;
using runtime::TVMArgs;
using runtime::TVMRetValue;

using runtime::FunctionInfo;
using runtime::GetFileFormat;
using runtime::GetMetaFilePath;
using runtime::SaveBinaryToFile;

/*!
 * \brief Create a metadata module wrapper. The helper is used by different
 *        codegens, such as graph runtime codegen and the vm compiler.
 *
 * \param params The metadata for initialization of all modules.
 * \param modules All the modules that needs to be imported inside the metadata module(s).
 * \param target The target that all the modules are compiled for
 * \return The created metadata module that manages initialization of metadata.
 */
runtime::Module CreateMetadataModule(
    const std::unordered_map<std::string, runtime::NDArray>& params,
    const Array<runtime::Module>& modules, Target target) {
  Array<tvm::runtime::Module> csource_metadata_modules;
  Array<tvm::runtime::Module> binary_metadata_modules;

  auto DSOExportable = [](tvm::runtime::Module& mod) {
    return !std::strcmp(mod->type_key(), "llvm") || !std::strcmp(mod->type_key(), "c");
  };

  // Wrap all submodules in the initialization wrapper.
  std::unordered_map<std::string, std::vector<std::string>> sym_metadata;
  for (tvm::runtime::Module mod : modules) {
    auto pf_sym = mod.GetFunction("get_symbol");
    auto pf_var = mod.GetFunction("get_const_vars");
    if (pf_sym != nullptr && pf_var != nullptr) {
      String symbol = pf_sym();
      Array<String> variables = pf_var();
      std::vector<std::string> arrays;
      for (size_t i = 0; i < variables.size(); i++) {
        arrays.push_back(variables[i].operator std::string());
      }
      ICHECK_EQ(sym_metadata.count(symbol), 0U) << "Found duplicated symbol: " << symbol;
      sym_metadata[symbol] = arrays;
      // We only need loading of serialized constant data
      // if there are constants present and required by the
      // runtime module to be initialized by the binary
      // metadata module. If not rest of the modules are
      // wrapped in c-source metadata module.

      // TODO(@manupa-arm) : we should be able to use csource_metadata
      // if the variables are empty
      if (!variables.empty() || !DSOExportable(mod)) {
        binary_metadata_modules.push_back(mod);
      } else {
        csource_metadata_modules.push_back(mod);
      }
    } else {
      csource_metadata_modules.push_back(mod);
    }
  }
  auto c_meta_mod = CreateCSourceMetadataModule(csource_metadata_modules, target);
  // Wrap the modules.
  if (!binary_metadata_modules.empty()) {
    runtime::Module binary_meta_mod = runtime::MetadataModuleCreate(params, sym_metadata);
    binary_meta_mod.Import(c_meta_mod);
    for (const auto& it : binary_metadata_modules) {
      binary_meta_mod.Import(it);
    }
    return binary_meta_mod;
  }
  return c_meta_mod;
}

// Simulator function
class SourceModuleNode : public runtime::ModuleNode {
 public:
  SourceModuleNode(std::string code, std::string fmt) : code_(code), fmt_(fmt) {}
  const char* type_key() const { return "source"; }

  PackedFunc GetFunction(const std::string& name, const ObjectPtr<Object>& sptr_to_self) final {
    LOG(FATAL) << "Source module cannot execute, to get executable module"
               << " build TVM with \'" << fmt_ << "\' runtime support";
    return PackedFunc();
  }

  std::string GetSource(const std::string& format) final { return code_; }

 protected:
  std::string code_;
  std::string fmt_;
};

runtime::Module SourceModuleCreate(std::string code, std::string fmt) {
  auto n = make_object<SourceModuleNode>(code, fmt);
  return runtime::Module(n);
}

// Simulator function
class CSourceModuleNode : public runtime::ModuleNode {
 public:
  CSourceModuleNode(const std::string& code, const std::string& fmt,
                    const Array<String>& func_names, const std::string& symbol,
                    const Array<String>& const_vars)
      : code_(code), fmt_(fmt), symbol_(symbol), const_vars_(const_vars), func_names_(func_names) {}
  const char* type_key() const { return "c"; }

  PackedFunc GetFunction(const std::string& name, const ObjectPtr<Object>& sptr_to_self) final {
    if (name == "get_symbol") {
      return PackedFunc(
          [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->symbol_; });
    } else if (name == "get_const_vars") {
      return PackedFunc(
          [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->const_vars_; });
    } else if (name == "get_func_names") {
      return PackedFunc(
          [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->func_names_; });
    } else {
      return PackedFunc(nullptr);
    }
  }

  std::string GetSource(const std::string& format) final { return code_; }

  void SaveToFile(const std::string& file_name, const std::string& format) final {
    std::string fmt = GetFileFormat(file_name, format);
    std::string meta_file = GetMetaFilePath(file_name);
    if (fmt == "cc") {
      ICHECK_NE(code_.length(), 0);
      SaveBinaryToFile(file_name, code_);
    } else {
      ICHECK_EQ(fmt, fmt_) << "Can only save to format=" << fmt_;
    }
  }

 protected:
  std::string code_;
  std::string fmt_;
  std::string symbol_;
  Array<String> const_vars_;
  Array<String> func_names_;
};

runtime::Module CSourceModuleCreate(const String& code, const String& fmt,
                                    const Array<String>& func_names, const String& symbol,
                                    const Array<String>& const_vars) {
  auto n = make_object<CSourceModuleNode>(code.operator std::string(), fmt.operator std::string(),
                                          func_names, symbol.operator std::string(), const_vars);
  return runtime::Module(n);
}

class CSourceMetadataModuleNode : public runtime::ModuleNode {
 public:
  CSourceMetadataModuleNode(const Array<String>& func_names, const std::string& fmt, Target target)
      : fmt_(fmt), func_names_(func_names), target_(target) {
    CreateSource();
  }
  const char* type_key() const { return "c"; }

  std::string GetSource(const std::string& format) final { return code_.str(); }

  PackedFunc GetFunction(const std::string& name, const ObjectPtr<Object>& sptr_to_self) final {
    return PackedFunc(nullptr);
  }

  void SaveToFile(const std::string& file_name, const std::string& format) final {
    std::string fmt = GetFileFormat(file_name, format);
    std::string meta_file = GetMetaFilePath(file_name);
    if (fmt == "cc") {
      auto code_str = code_.str();
      ICHECK_NE(code_str.length(), 0);
      SaveBinaryToFile(file_name, code_str);
    } else {
      ICHECK_EQ(fmt, fmt_) << "Can only save to format=" << fmt_;
    }
  }

 protected:
  std::stringstream code_;
  std::string fmt_;
  Array<String> func_names_;
  Target target_;

  void CreateFuncRegistry() {
    code_ << "#include <tvm/runtime/crt/module.h>\n";
    for (const auto& fname : func_names_) {
      code_ << "extern \"C\" TVM_DLL int32_t " << fname.data();
      code_ << "(TVMValue* args, int* type_code, int num_args, TVMValue* out_value, int* "
               "out_type_code);\n";
    }
    code_ << "static TVMBackendPackedCFunc _tvm_func_array[] = {\n";
    for (auto f : func_names_) {
      code_ << "    (TVMBackendPackedCFunc)" << f << ",\n";
    }
    code_ << "};\n";
    auto registry = target::GenerateFuncRegistryNames(func_names_);
    code_ << "static const TVMFuncRegistry _tvm_func_registry = {\n"
          << "    \"" << ::tvm::support::StrEscape(registry.data(), registry.size(), true) << "\","
          << "    _tvm_func_array,\n"
          << "};\n";
  }

  void GenerateCrtSystemLib() {
    code_ << "static const TVMModule _tvm_system_lib = {\n"
          << "    &_tvm_func_registry,\n"
          << "};\n"
          << "const TVMModule* TVMSystemLibEntryPoint(void) {\n"
          << "    return &_tvm_system_lib;\n"
          << "}\n";
  }

  void CreateSource() {
    if (target_->GetAttr<Bool>("system-lib").value_or(Bool(false)) && !func_names_.empty()) {
      CreateFuncRegistry();
      GenerateCrtSystemLib();
    }
    code_ << ";";
  }
};

runtime::Module CreateCSourceMetadataModule(const Array<runtime::Module>& modules, Target target) {
  Array<String> func_names;
  for (runtime::Module mod : modules) {
    auto pf_funcs = mod.GetFunction("get_func_names");
    if (pf_funcs != nullptr) {
      Array<String> func_names_ = pf_funcs();
      for (const auto& fname : func_names_) {
        func_names.push_back(fname);
      }
    }
  }
  auto n = make_object<CSourceMetadataModuleNode>(func_names, "cc", target);
  auto csrc_metadata_module = runtime::Module(n);
  for (const auto& mod : modules) {
    csrc_metadata_module.Import(mod);
  }
  return std::move(csrc_metadata_module);
}

// supports limited save without cross compile
class DeviceSourceModuleNode final : public runtime::ModuleNode {
 public:
  DeviceSourceModuleNode(std::string data, std::string fmt,
                         std::unordered_map<std::string, FunctionInfo> fmap, std::string type_key,
                         std::function<std::string(const std::string&)> fget_source)
      : data_(data), fmt_(fmt), fmap_(fmap), type_key_(type_key), fget_source_(fget_source) {}

  PackedFunc GetFunction(const std::string& name, const ObjectPtr<Object>& sptr_to_self) final {
    LOG(FATAL) << "Source module cannot execute, to get executable module"
               << " build TVM with \'" << fmt_ << "\' runtime support";
    return PackedFunc();
  }

  std::string GetSource(const std::string& format) final {
    if (fget_source_ != nullptr) {
      return fget_source_(format);
    } else {
      return data_;
    }
  }

  const char* type_key() const { return type_key_.c_str(); }

  void SaveToFile(const std::string& file_name, const std::string& format) final {
    std::string fmt = GetFileFormat(file_name, format);
    ICHECK_EQ(fmt, fmt_) << "Can only save to format=" << fmt_;
    std::string meta_file = GetMetaFilePath(file_name);
    SaveMetaDataToFile(meta_file, fmap_);
    SaveBinaryToFile(file_name, data_);
  }

  void SaveToBinary(dmlc::Stream* stream) final {
    stream->Write(fmt_);
    stream->Write(fmap_);
    stream->Write(data_);
  }

 private:
  std::string data_;
  std::string fmt_;
  std::unordered_map<std::string, FunctionInfo> fmap_;
  std::string type_key_;
  std::function<std::string(const std::string&)> fget_source_;
};

runtime::Module DeviceSourceModuleCreate(
    std::string data, std::string fmt, std::unordered_map<std::string, FunctionInfo> fmap,
    std::string type_key, std::function<std::string(const std::string&)> fget_source) {
  auto n = make_object<DeviceSourceModuleNode>(data, fmt, fmap, type_key, fget_source);
  return runtime::Module(n);
}

TVM_REGISTER_GLOBAL("runtime.SourceModuleCreate").set_body_typed(SourceModuleCreate);

TVM_REGISTER_GLOBAL("runtime.CSourceModuleCreate")
    .set_body_typed([](String code, String fmt, Array<String> func_names, String symbol,
                       Array<String> const_vars) {
      return CSourceModuleCreate(code, fmt, func_names, symbol, const_vars);
    });

TVM_REGISTER_GLOBAL("runtime.CreateCSourceMetadataModule")
    .set_body_typed([](const Array<runtime::Module>& modules, Target target) {
      return CreateCSourceMetadataModule(modules, target);
    });

}  // namespace codegen
}  // namespace tvm
