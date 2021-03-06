/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "LocalDcePass.h"
#include "RedexTest.h"
#include "ScopedCFG.h"

#include "InstructionSequenceOutliner.h"

DexMethodRef* find_invoked_method(const cfg::ControlFlowGraph& cfg,
                                  const std::string& name) {
  for (auto& mie : InstructionIterable(cfg)) {
    if (mie.insn->has_method()) {
      DexMethodRef* m = mie.insn->get_method();
      if (m->get_name()->str().find(name) != std::string::npos) {
        return m;
      }
    }
  }
  return nullptr;
}

size_t count_invokes(const cfg::ControlFlowGraph& cfg, DexMethodRef* m) {
  int count = 0;
  for (auto& mie : InstructionIterable(cfg)) {
    if (mie.insn->has_method() && mie.insn->get_method() == m) {
      count++;
    }
  }
  return count;
}

size_t count_invokes(const cfg::ControlFlowGraph& cfg,
                     const std::string& name) {
  return count_invokes(cfg, find_invoked_method(cfg, name));
}

class InstructionSequenceOutlinerTest : public RedexIntegrationTest {};

TEST_F(InstructionSequenceOutlinerTest, basic) {
  // Testing basic outlining, regardless of whether the outlined instruction
  // sequence is surrounded by some distractions
  std::vector<DexMethodRef*> println_methods;
  std::vector<DexMethod*> basic_methods;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str().find("basic") != std::string::npos) {
        IRCode* code = m->get_code();
        cfg::ScopedCFG scoped_cfg(code);
        auto println_method = find_invoked_method(*scoped_cfg, "println");
        EXPECT_NE(println_method, nullptr);
        println_methods.push_back(println_method);
        EXPECT_EQ(count_invokes(code->cfg(), println_method), 5);
        basic_methods.push_back(m);
      }
    }
  }
  sort_unique(println_methods);
  EXPECT_EQ(println_methods.size(), 1);
  auto println_method = println_methods.front();
  EXPECT_EQ(basic_methods.size(), 4);

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  std::vector<DexMethod*> outlined_methods;
  for (auto m : basic_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 0);
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_NE(outlined_method, nullptr);
    // outlined method should reside in the same class, as the outlined
    // code sequence is not used by any other classs
    EXPECT_EQ(outlined_method->get_class(), m->get_class());
    EXPECT_EQ(count_invokes(*scoped_cfg, outlined_method), 1);
    outlined_methods.push_back(outlined_method->as_def());
  }
  sort_unique(outlined_methods);
  EXPECT_EQ(outlined_methods.size(), 1);
  for (auto m : outlined_methods) {
    EXPECT_TRUE(is_static(m));
    auto proto = m->get_proto();
    EXPECT_EQ(proto->get_rtype(), type::_void());
    EXPECT_EQ(proto->get_args()->size(), 0);
    cfg::ScopedCFG scoped_cfg(m->get_code());
    EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 5);
  }
}

TEST_F(InstructionSequenceOutlinerTest, twice) {
  // Testing that there can be multiple outlined locations within a method.
  std::vector<DexMethod*> twice_methods;
  DexMethodRef* println_method = nullptr;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str().find("twice") != std::string::npos) {
        cfg::ScopedCFG scoped_cfg(m->get_code());
        println_method = find_invoked_method(*scoped_cfg, "println");
        EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 10);
        twice_methods.push_back(m);
      }
    }
  }
  EXPECT_NE(println_method, nullptr);

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  for (auto m : twice_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 0);
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_NE(outlined_method, nullptr);
    EXPECT_EQ(count_invokes(*scoped_cfg, outlined_method), 2);
  }
}

TEST_F(InstructionSequenceOutlinerTest, in_try) {
  // Testing that we can outlined across a big block (consisting of several
  // individual blocks) surrounded by a try-catch.
  std::vector<DexMethod*> in_try_methods;
  DexMethodRef* println_method = nullptr;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str() == "in_try") {
        cfg::ScopedCFG scoped_cfg(m->get_code());
        println_method = find_invoked_method(*scoped_cfg, "println");
        EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 5);
        in_try_methods.push_back(m);
      }
    }
  }
  EXPECT_NE(println_method, nullptr);
  EXPECT_EQ(in_try_methods.size(), 1);

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  for (auto m : in_try_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 0);
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_NE(outlined_method, nullptr);
    EXPECT_EQ(count_invokes(*scoped_cfg, outlined_method), 1);
  }
}

TEST_F(InstructionSequenceOutlinerTest, in_try_ineligible_) {
  // Big blocks don't kick in when...
  // - there are different catches
  //   (in_try_ineligible_due_to_different_catches), or
  // - there is a conditional branch
  //   (in_try_ineligible_due_to_conditional_branch)
  std::vector<DexMethod*> in_try_ineligible_methods;
  DexMethodRef* println_method = nullptr;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str().find("in_try_ineligible_") !=
          std::string::npos) {
        cfg::ScopedCFG scoped_cfg(m->get_code());
        println_method = find_invoked_method(*scoped_cfg, "println");
        EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 5);
        in_try_ineligible_methods.push_back(m);
      }
    }
  }
  EXPECT_NE(println_method, nullptr);
  EXPECT_EQ(in_try_ineligible_methods.size(), 2);

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  for (auto m : in_try_ineligible_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 5);
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_EQ(outlined_method, nullptr);
  }
}

TEST_F(InstructionSequenceOutlinerTest, param) {
  // Testing outlining of code into a method that takes a parameter
  std::vector<DexMethod*> param_methods;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str().find("param") != std::string::npos) {
        param_methods.push_back(m);
      }
    }
  }
  EXPECT_EQ(param_methods.size(), 2);

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  std::vector<DexMethod*> outlined_methods;
  for (auto m : param_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_NE(outlined_method, nullptr);
    EXPECT_EQ(count_invokes(*scoped_cfg, outlined_method), 1);
    outlined_methods.push_back(outlined_method->as_def());
  }
  sort_unique(outlined_methods);
  EXPECT_EQ(outlined_methods.size(), 1);
  for (auto m : outlined_methods) {
    EXPECT_TRUE(is_static(m));
    auto proto = m->get_proto();
    EXPECT_EQ(proto->get_rtype(), type::_void());
    EXPECT_EQ(proto->get_args()->size(), 1);
    EXPECT_EQ(proto->get_args()->at(0), type::java_lang_String());
  }
}

TEST_F(InstructionSequenceOutlinerTest, result) {
  // Testing outlining of code that has a live-out value that needs to be
  // returned by the outlined method
  std::vector<DexMethod*> result_methods;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str().find("result") != std::string::npos) {
        result_methods.push_back(m);
      }
    }
  }
  EXPECT_EQ(result_methods.size(), 2);

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  std::vector<DexMethod*> outlined_methods;
  for (auto m : result_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_NE(outlined_method, nullptr);
    EXPECT_EQ(count_invokes(*scoped_cfg, outlined_method), 1);
    outlined_methods.push_back(outlined_method->as_def());
  }
  sort_unique(outlined_methods);
  EXPECT_EQ(outlined_methods.size(), 1);
  for (auto m : outlined_methods) {
    EXPECT_TRUE(is_static(m));
    auto proto = m->get_proto();
    EXPECT_EQ(proto->get_rtype(), type::_int());
    EXPECT_EQ(proto->get_args()->size(), 0);
  }
}

TEST_F(InstructionSequenceOutlinerTest, normalization) {
  // Testing that outlining happens modulo register naming
  std::vector<DexMethod*> param_methods;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str().find("normalization") != std::string::npos) {
        param_methods.push_back(m);
      }
    }
  }
  EXPECT_EQ(param_methods.size(), 2);

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  std::vector<DexMethod*> outlined_methods;
  for (auto m : param_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_NE(outlined_method, nullptr);
    EXPECT_EQ(count_invokes(*scoped_cfg, outlined_method), 1);
    outlined_methods.push_back(outlined_method->as_def());
  }
  sort_unique(outlined_methods);
  EXPECT_EQ(outlined_methods.size(), 1);
  for (auto m : outlined_methods) {
    EXPECT_TRUE(is_static(m));
    auto proto = m->get_proto();
    EXPECT_EQ(proto->get_rtype(), type::_int());
    EXPECT_EQ(proto->get_args()->size(), 1);
    EXPECT_EQ(proto->get_args()->at(0), type::_int());
  }
}

TEST_F(InstructionSequenceOutlinerTest, defined_reg_escapes_to_catch) {
  // We cannot outline when a defined register escapes to a throw block
  std::vector<DexMethod*> defined_reg_escapes_to_catch_methods;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str() == "defined_reg_escapes_to_catch") {
        defined_reg_escapes_to_catch_methods.push_back(m);
      }
    }
  }
  EXPECT_EQ(defined_reg_escapes_to_catch_methods.size(), 1);

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  for (auto m : defined_reg_escapes_to_catch_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_EQ(outlined_method, nullptr);
  }
}

TEST_F(InstructionSequenceOutlinerTest, big_block_can_end_with_no_tries) {
  // Test that a sequence becomes beneficial to outline because a big block can
  // have throwing code followed by non-throwing code.
  std::vector<DexMethod*> big_block_can_end_with_no_tries_methods;
  DexMethodRef* println_method;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str().find("big_block_can_end_with_no_tries") !=
          std::string::npos) {
        cfg::ScopedCFG scoped_cfg(m->get_code());
        println_method = find_invoked_method(*scoped_cfg, "println");
        big_block_can_end_with_no_tries_methods.push_back(m);
      }
    }
  }
  EXPECT_EQ(big_block_can_end_with_no_tries_methods.size(), 2);
  EXPECT_NE(println_method, nullptr);

  std::vector<Pass*> passes = {
      new LocalDcePass(),
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  for (auto m : big_block_can_end_with_no_tries_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_NE(outlined_method, nullptr);
    auto println_method_invokes = count_invokes(*scoped_cfg, println_method);
    EXPECT_EQ(println_method_invokes, 0);
  }
}

TEST_F(InstructionSequenceOutlinerTest, two_out_regs) {
  // We cannot outline when there are two defined live-out regs
  std::vector<DexMethod*> two_out_regs_methods;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str() == "defined_reg_escapes_to_catch") {
        two_out_regs_methods.push_back(m);
      }
    }
  }
  EXPECT_EQ(two_out_regs_methods.size(), 1);

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  for (auto m : two_out_regs_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_EQ(outlined_method, nullptr);
  }
}

TEST_F(InstructionSequenceOutlinerTest, type_demand) {
  // The arguments of the outlined methods are as weak as allowed by the
  // demands placed on them in the outlined instruction sequence.
  // In particular, here, the argument is of type Object, not String, as the
  // outlined instruction sequence starts with a cast, which only has the
  // weaked type demand of Object.
  std::vector<DexMethod*> param_methods;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (m->get_name()->str().find("type_demand") != std::string::npos) {
        param_methods.push_back(m);
      }
    }
  }
  EXPECT_EQ(param_methods.size(), 2);

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  std::vector<DexMethod*> outlined_methods;
  for (auto m : param_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_NE(outlined_method, nullptr);
    EXPECT_EQ(count_invokes(*scoped_cfg, outlined_method), 1);
    outlined_methods.push_back(outlined_method->as_def());
  }
  sort_unique(outlined_methods);
  EXPECT_EQ(outlined_methods.size(), 1);
  for (auto m : outlined_methods) {
    EXPECT_TRUE(is_static(m));
    auto proto = m->get_proto();
    EXPECT_EQ(proto->get_rtype(), type::_void());
    EXPECT_EQ(proto->get_args()->size(), 1);
    EXPECT_EQ(proto->get_args()->at(0), type::java_lang_Object());
  }
}

TEST_F(InstructionSequenceOutlinerTest, distributed) {
  // When outlined sequence occur in unrelated classes, the outlined method
  // it put into a generated helper class
  std::vector<DexMethod*> distributed_methods;
  std::vector<DexMethodRef*> println_methods;
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_dmethods()) {
      if (m->get_name()->str() == "distributed") {
        cfg::ScopedCFG scoped_cfg(m->get_code());
        auto println_method = find_invoked_method(*scoped_cfg, "println");
        EXPECT_NE(println_method, nullptr);
        println_methods.push_back(println_method);
        EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 5);
        distributed_methods.push_back(m);
      }
    }
  }
  EXPECT_EQ(distributed_methods.size(), 2);
  sort_unique(println_methods);
  EXPECT_EQ(println_methods.size(), 1);
  auto println_method = println_methods.front();

  std::vector<Pass*> passes = {
      new InstructionSequenceOutliner(),
  };

  run_passes(passes);

  std::vector<DexMethod*> outlined_methods;
  for (const auto& m : distributed_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 0);
    auto outlined_method = find_invoked_method(*scoped_cfg, "$outline");
    EXPECT_NE(outlined_method, nullptr);
    EXPECT_NE(outlined_method->get_class(), m->get_class());
    EXPECT_EQ(count_invokes(*scoped_cfg, outlined_method), 1);
    outlined_methods.push_back(outlined_method->as_def());
  }
  sort_unique(outlined_methods);
  EXPECT_EQ(outlined_methods.size(), 1);
  for (auto m : outlined_methods) {
    cfg::ScopedCFG scoped_cfg(m->get_code());
    EXPECT_EQ(count_invokes(*scoped_cfg, println_method), 5);
  }
}
