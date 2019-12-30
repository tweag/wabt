/*
 * Copyright 2019 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/decompiler.h"

#include "src/decompiler-ast.h"
#include "src/decompiler-ls.h"
#include "src/decompiler-naming.h"

#include "src/stream.h"

#define WABT_TRACING 0
#include "src/tracing.h"

#include <inttypes.h>

namespace wabt {

struct Decompiler {
  Decompiler(const Module& module, const DecompileOptions& options)
      : mc(module), options(options) {}

  struct Value {
    std::vector<std::string> v;
    // Lazily add bracketing only if the parent requires it.
    // TODO: replace with a system based on precedence?
    bool needs_bracketing;

    Value(std::vector<std::string>&& v, bool nb)
      : v(v), needs_bracketing(nb) {}

    size_t width() {
      size_t w = 0;
      for (auto &s : v) {
        w = std::max(w, s.size());
      }
      return w;
    }

    // This value should really never be copied, only moved.
    Value(Value&& rhs) = default;
    Value(const Value& rhs) = delete;
    Value& operator=(Value&& rhs) = default;
    Value& operator=(const Value& rhs) = delete;
  };

  std::string to_string(double d) {
    auto s = std::to_string(d);
    // Remove redundant trailing '0's that to_string produces.
    while (s.size() > 2 && s.back() == '0' && s[s.size() - 2] != '.')
      s.pop_back();
    return s;
  }

  std::string Indent(size_t amount) {
    return std::string(amount, ' ');
  }

  std::string OpcodeToToken(Opcode opcode) {
    std::string s = opcode.GetDecomp();
    std::replace(s.begin(), s.end(), '.', '_');
    return s;
  }

  void IndentValue(Value &val, size_t amount, string_view first_indent) {
    auto indent = Indent(amount);
    for (auto& stat : val.v) {
      auto is = (&stat != &val.v[0] || first_indent.empty())
                    ? string_view(indent)
                    : first_indent;
      stat.insert(0, is.data(), is.size());
    }
  }

  Value WrapChild(Value &child, string_view prefix, string_view postfix) {
    auto width = prefix.size() + postfix.size() + child.width();
    auto &v = child.v;
    if (width < target_exp_width ||
        (prefix.size() <= indent_amount && postfix.size() <= indent_amount)) {
      if (v.size() == 1) {
        // Fits in a single line.
        v[0].insert(0, prefix.data(), prefix.size());
        v[0].append(postfix.data(), postfix.size());
      } else {
        // Multiline, but with prefix on same line.
        IndentValue(child, prefix.size(), prefix);
        v.back().append(postfix.data(), postfix.size());
      }
    } else {
      // Multiline with prefix on its own line.
      IndentValue(child, indent_amount, {});
      v.insert(v.begin(), std::string(prefix));
      v.back().append(postfix.data(), postfix.size());
    }
    return std::move(child);
  }

  void BracketIfNeeded(Value &val) {
    if (!val.needs_bracketing) return;
    val = WrapChild(val, "(", ")");
    val.needs_bracketing = false;
  }

  Value WrapBinary(std::vector<Value>& args, string_view infix, bool indent_right) {
    assert(args.size() == 2);
    auto& left = args[0];
    auto& right = args[1];
    BracketIfNeeded(left);
    BracketIfNeeded(right);
    auto width = infix.size() + left.width() + right.width();
    if (width < target_exp_width && left.v.size() == 1 && right.v.size() == 1) {
      return Value{{left.v[0] + infix + right.v[0]}, true};
    } else {
      Value bin { {}, true };
      std::move(left.v.begin(), left.v.end(), std::back_inserter(bin.v));
      bin.v.back().append(infix.data(), infix.size());
      if (indent_right) IndentValue(right, indent_amount, {});
      std::move(right.v.begin(), right.v.end(), std::back_inserter(bin.v));
      return bin;
    }
  }

  Value WrapNAry(std::vector<Value>& args, string_view prefix, string_view postfix) {
    size_t total_width = 0;
    size_t max_width = 0;
    bool multiline = false;
    for (auto& child : args) {
      auto w = child.width();
      max_width = std::max(max_width, w);
      total_width += w;
      multiline = multiline || child.v.size() > 1;
    }
    if (!multiline &&
        (total_width + prefix.size() + postfix.size() < target_exp_width ||
         args.empty())) {
      // Single line.
      auto s = std::string(prefix);
      for (auto& child : args) {
        if (&child != &args[0])
          s += ", ";
        s += child.v[0];
      }
      s += postfix;
      return Value{{std::move(s)}, false};
    } else {
      // Multi-line.
      Value ml { {}, false };
      auto ident_with_name = max_width + prefix.size() < target_exp_width;
      size_t i = 0;
      for (auto& child : args) {
        IndentValue(child, ident_with_name ? prefix.size() : indent_amount,
                    !i && ident_with_name ? prefix : string_view {});
        if (i < args.size() - 1) child.v.back() += ",";
        std::move(child.v.begin(), child.v.end(),
                  std::back_inserter(ml.v));
        i++;
      }
      if (!ident_with_name) ml.v.insert(ml.v.begin(), std::string(prefix));
      ml.v.back() += postfix;
      return ml;
    }
  }

  template<ExprType T> Value Get(const VarExpr<T>& ve) {
    return Value{{ve.var.name()}, false};
  }

  template<ExprType T> Value Set(Value& child, const VarExpr<T>& ve) {
    child.needs_bracketing = true;
    return WrapChild(child, ve.var.name() + " = ", "");
  }

  Value Block(Value &val, const Block& block, LabelType label, const char *name) {
    IndentValue(val, indent_amount, {});
    val.v.insert(val.v.begin(), cat(name, " ", block.label, " {"));
    val.v.push_back("}");
    return std::move(val);
  }

  std::string TempVarName(Index n) {
    // FIXME: this needs much better variable naming. Problem is, the code
    // in generate-names.cc has allready run, its dictionaries deleted, so it
    // is not easy to integrate with it.
    return "t" + std::to_string(n);
  }

  std::string LocalDecl(const std::string& name, Type t) {
    auto struc = lst.GenStruct(name);
    return cat(name, ":", struc.empty() ? GetDecompTypeName(t) : struc);
  }

  void LoadStore(Value &val, const Node& addr_exp, uint32_t offset,
                  Opcode opc, Address align, Type op_type) {
    BracketIfNeeded(val);
    auto access = lst.GenAccess(offset, addr_exp);
    val.v.back() +=
        !access.empty()
            ? "." + access
            : cat("[", std::to_string(offset),
                  "]:", GetDecompTypeName(GetMemoryType(op_type, opc)),
                  !opc.IsNaturallyAligned(align)
                      ? cat("@" + std::to_string(align))
                      : "");
  }

  Value DecompileExpr(const Node &n) {
    std::vector<Value> args;
    for (auto &c : n.children) {
      args.push_back(DecompileExpr(c));
    }
    // First deal with the specialized node types.
    switch (n.ntype) {
      case NodeType::FlushToVars: {
        std::string decls = "let ";
        for (Index i = 0; i < n.u.var_count; i++) {
          if (i) decls += ", ";
          decls += TempVarName(n.u.var_start + i);
        }
        decls += " = ";
        return WrapNAry(args, decls, "");
      }
      case NodeType::FlushedVar: {
        return Value { { TempVarName(n.u.var_start) }, false };
      }
      case NodeType::Statements: {
        Value stats { {}, false };
        for (size_t i = 0; i < n.children.size(); i++) {
          auto& s = args[i].v.back();
          if (s.back() != '}') s += ';';
          std::move(args[i].v.begin(), args[i].v.end(),
                    std::back_inserter(stats.v));
        }
        return stats;
      }
      case NodeType::EndReturn: {
        return WrapNAry(args, "return ", "");
      }
      case NodeType::Decl: {
        return Value{
            {"var " + LocalDecl(n.u.var->name(),
                                cur_func->GetLocalType(*n.u.var))},
            false};
      }
      case NodeType::DeclInit: {
        return WrapChild(
            args[0],
            cat("var ",
                LocalDecl(n.u.var->name(), cur_func->GetLocalType(*n.u.var)),
                " = "),
            "");
      }
      case NodeType::Expr:
        // We're going to fall thru to the second switch to deal with ExprType.
        break;
      case NodeType::Uninitialized:
        assert(false);
        break;
    }
    switch (n.etype) {
      case ExprType::Const: {
        auto& c = cast<ConstExpr>(n.e)->const_;
        switch (c.type) {
          case Type::I32:
            return Value{{std::to_string(static_cast<int32_t>(c.u32))}, false};
          case Type::I64:
            return Value{{std::to_string(static_cast<int64_t>(c.u64)) + "L"},
                         false};
          case Type::F32: {
            float f;
            memcpy(&f, &c.f32_bits, sizeof(float));
            return Value{{to_string(f) + "f"}, false};
          }
          case Type::F64: {
            double d;
            memcpy(&d, &c.f64_bits, sizeof(double));
            return Value{{to_string(d)}, false};
          }
          case Type::V128:
            return Value{{"V128"}, false};  // FIXME
          default:
            WABT_UNREACHABLE;
        }
        return Value{{}, false};
      }
      case ExprType::LocalGet: {
        return Get(*cast<LocalGetExpr>(n.e));
      }
      case ExprType::GlobalGet: {
        return Get(*cast<GlobalGetExpr>(n.e));
      }
      case ExprType::LocalSet: {
        return Set(args[0], *cast<LocalSetExpr>(n.e));
      }
      case ExprType::GlobalSet: {
        return Set(args[0], *cast<GlobalSetExpr>(n.e));
      }
      case ExprType::LocalTee: {
        auto& te = *cast<LocalTeeExpr>(n.e);
        return args.empty() ? Get(te) : Set(args[0], te);
      }
      case ExprType::Binary: {
        auto& be = *cast<BinaryExpr>(n.e);
        return WrapBinary(args, cat(" ", OpcodeToToken(be.opcode), " "), false);
      }
      case ExprType::Compare: {
        auto& ce = *cast<CompareExpr>(n.e);
        return WrapBinary(args, cat(" ", OpcodeToToken(ce.opcode), " "), false);
      }
      case ExprType::Unary: {
        auto& ue = *cast<UnaryExpr>(n.e);
        //BracketIfNeeded(stack.back());
        // TODO: also version without () depending on precedence.
        return WrapChild(args[0], OpcodeToToken(ue.opcode) + "(", ")");
      }
      case ExprType::Load: {
        auto& le = *cast<LoadExpr>(n.e);
        LoadStore(args[0], n.children[0], le.offset, le.opcode, le.align,
                  le.opcode.GetResultType());
        return std::move(args[0]);
      }
      case ExprType::Store: {
        auto& se = *cast<StoreExpr>(n.e);
        LoadStore(args[0], n.children[0], se.offset, se.opcode, se.align,
                  se.opcode.GetParamType2());
        return WrapBinary(args, " = ", true);
      }
      case ExprType::If: {
        auto ife = cast<IfExpr>(n.e);
        Value *elsep = nullptr;
        if (!ife->false_.empty()) {
          elsep = &args[2];
        }
        auto& thenp = args[1];
        auto& ifs = args[0];
        bool multiline = ifs.v.size() > 1 || thenp.v.size() > 1;
        size_t width = ifs.width() + thenp.width();
        if (elsep) {
          width += elsep->width();
          multiline = multiline || elsep->v.size() > 1;
        }
        multiline = multiline || width > target_exp_width;
        if (multiline) {
          auto if_start = string_view("if (");
          ifs.v[0].insert(0, if_start.data(), if_start.size());
          ifs.v.back() += ") {";
          IndentValue(thenp, indent_amount, {});
          std::move(thenp.v.begin(), thenp.v.end(), std::back_inserter(ifs.v));
          if (elsep) {
            ifs.v.push_back("} else {");
            IndentValue(*elsep, indent_amount, {});
            std::move(elsep->v.begin(), elsep->v.end(), std::back_inserter(ifs.v));
          }
          ifs.v.push_back("}");
          return std::move(ifs);
        } else {
          auto s = cat("if (", ifs.v[0], ") { ", thenp.v[0], " }");
          if (elsep)
            s += cat(" else { ", elsep->v[0], " }");
          return Value{{std::move(s)}, false};
        }
      }
      case ExprType::Block: {
        return Block(args[0], cast<BlockExpr>(n.e)->block, LabelType::Block, "block");
      }
      case ExprType::Loop: {
        return Block(args[0], cast<LoopExpr>(n.e)->block, LabelType::Loop, "loop");
      }
      case ExprType::Br: {
        auto be = cast<BrExpr>(n.e);
        return Value{{(n.u.lt == LabelType::Loop ? "continue " : "break ") +
                      be->var.name()},
                     false};
      }
      case ExprType::BrIf: {
        auto bie = cast<BrIfExpr>(n.e);
        auto jmp = n.u.lt == LabelType::Loop ? "continue" : "break";
        return WrapChild(args[0], "if (", cat(") ", jmp, " ", bie->var.name()));
      }
      case ExprType::Return: {
        return WrapNAry(args, "return ", "");
      }
      case ExprType::Drop: {
        // Silent dropping of return values is very common, so currently
        // don't output this.
        return std::move(args[0]);
      }
      default: {
        std::string name;
        switch (n.etype) {
          case ExprType::Call:
            name = cast<CallExpr>(n.e)->var.name();
            break;
          case ExprType::Convert:
            name = std::string(OpcodeToToken(cast<ConvertExpr>(n.e)->opcode));
            break;
          default:
            name = GetExprTypeName(n.etype);
            break;
        }
        return WrapNAry(args, name + "(", ")");
      }
    }
  }

  bool CheckImportExport(std::string& s,
                         ExternalKind kind,
                         Index index,
                         string_view name) {
    // Figure out if this thing is imported, exported, or neither.
    auto is_import = mc.module.IsImport(kind, Var(index));
    // TODO: is this the best way to check for export?
    // FIXME: this doesn't work for functions that get renamed in some way,
    // as the export has the original name..
    auto xport = mc.module.GetExport(name);
    auto is_export = xport && xport->kind == kind;
    if (is_export)
      s += "export ";
    if (is_import)
      s += "import ";
    return is_import;
  }

  std::string InitExp(const ExprList &el) {
    assert(!el.empty());
    AST ast(mc, nullptr);
    ast.Construct(el, 1, false);
    auto val = DecompileExpr(ast.exp_stack[0]);
    assert(ast.exp_stack.size() == 1 && val.v.size() == 1);
    return std::move(val.v[0]);
  }

  // FIXME: Merge with WatWriter::WriteQuotedData somehow.
  std::string BinaryToString(const std::vector<uint8_t> &in) {
    std::string s = "\"";
    static const char s_hexdigits[] = "0123456789abcdef";
    for (auto c : in) {
      if (c >= ' ' && c <= '~') {
        s += c;
      } else {
        s += '\\';
        s += s_hexdigits[c >> 4];
        s += s_hexdigits[c & 0xf];
      }
    }
    s += '\"';
    return s;
  }

  std::string Decompile() {
    std::string s;
    // Memories.
    Index memory_index = 0;
    for (auto m : mc.module.memories) {
      auto is_import =
          CheckImportExport(s, ExternalKind::Memory, memory_index, m->name);
      s += cat("memory ", m->name);
      if (!is_import) {
        s += cat("(initial: ", std::to_string(m->page_limits.initial),
                 ", max: ", std::to_string(m->page_limits.max), ")");
      }
      s += ";\n";
      memory_index++;
    }
    if (!mc.module.memories.empty())
      s += "\n";

    // Globals.
    Index global_index = 0;
    for (auto g : mc.module.globals) {
      auto is_import =
          CheckImportExport(s, ExternalKind::Global, global_index, g->name);
      s += cat("global ", g->name, ":", GetDecompTypeName(g->type));
      if (!is_import) {
        s += cat(" = ", InitExp(g->init_expr));
      }
      s += ";\n";
      global_index++;
    }
    if (!mc.module.globals.empty())
      s += "\n";

    // Tables.
    Index table_index = 0;
    for (auto tab : mc.module.tables) {
      auto is_import =
          CheckImportExport(s, ExternalKind::Table, table_index, tab->name);
      s += cat("table ", tab->name, ":", GetDecompTypeName(tab->elem_type));
      if (!is_import) {
        s += cat("(min: ", std::to_string(tab->elem_limits.initial),
                 ", max: ", std::to_string(tab->elem_limits.max), ")");
      }
      s += ";\n";
      table_index++;
    }
    if (!mc.module.tables.empty())
      s += "\n";

    // Data.
    for (auto dat : mc.module.data_segments) {
      s += cat("data ", dat->name, "(offset: ", InitExp(dat->offset),
               ") = ", BinaryToString(dat->data), ";\n");
    }
    if (!mc.module.data_segments.empty())
      s += "\n";

    // Code.
    Index func_index = 0;
    for (auto f : mc.module.funcs) {
      cur_func = f;
      auto is_import =
          CheckImportExport(s, ExternalKind::Func, func_index, f->name);
      AST ast(mc, f);
      if (!is_import) {
        ast.Construct(f->exprs, f->GetNumResults(), true);
        lst.Track(ast.exp_stack[0]);
        lst.CheckLayouts();
      }
      s += cat("function ", f->name, "(");
      for (Index i = 0; i < f->GetNumParams(); i++) {
        if (i)
          s += ", ";
        auto t = f->GetParamType(i);
        auto name = IndexToAlphaName(i);
        s += LocalDecl(name, t);
      }
      s += ")";
      if (f->GetNumResults()) {
        if (f->GetNumResults() == 1) {
          s += cat(":", GetDecompTypeName(f->GetResultType(0)));
        } else {
          s += ":(";
          for (Index i = 0; i < f->GetNumResults(); i++) {
            if (i)
              s += ", ";
            s += GetDecompTypeName(f->GetResultType(i));
          }
          s += ")";
        }
      }
      if (is_import) {
        s += ";";
      } else {
        s += " {\n";
        auto val = DecompileExpr(ast.exp_stack[0]);
        IndentValue(val, indent_amount, {});
        for (auto& stat : val.v) {
          s += stat;
          s += "\n";
        }
        s += "}";
      }
      s += "\n\n";
      mc.EndFunc();
      lst.Clear();
      func_index++;
    }
    return s;
  }

  ModuleContext mc;
  const DecompileOptions& options;
  size_t indent_amount = 2;
  size_t target_exp_width = 70;
  const Func* cur_func = nullptr;
  LoadStoreTracking lst;
};

std::string Decompile(const Module& module, const DecompileOptions& options) {
  Decompiler decompiler(module, options);
  return decompiler.Decompile();
}

}  // namespace wabt
