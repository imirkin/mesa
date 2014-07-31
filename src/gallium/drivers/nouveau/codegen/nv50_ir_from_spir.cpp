/*
 * Copyright 2014 Ilia Mirkin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <unordered_map>

#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>

#include "codegen/nv50_ir.h"
#include "codegen/nv50_ir_util.h"
#include "codegen/nv50_ir_build_util.h"

namespace {

using namespace llvm;
namespace nv50 = nv50_ir;

class Converter : public nv50::BuildUtil
{
public:
   Converter(nv50_ir::Program *, struct nv50_ir_prog_info *);
   ~Converter();

   bool convert(const Instruction&);
   bool convert(const BasicBlock&);
   bool convert(const Function&);
   nv50_ir::Value* convert(const Value *);

   bool run();

private:
   LLVMContext ctx;
   nv50_ir_prog_info *info;
   std::unordered_map<const llvm::Value*, nv50::Value*> values;
   std::unordered_map<const llvm::BasicBlock*, nv50::BasicBlock*> blocks;
   std::unordered_map<const llvm::Function*, nv50::Function*> functions;

   std::unordered_multimap<const llvm::BasicBlock *, nv50::Value *> phi_consts;
};

Converter::Converter(nv50::Program *ir, struct nv50_ir_prog_info *info) :
      BuildUtil(ir), info(info)
{
}

Converter::~Converter()
{
}

/*
icmp
br
phi
add
getelementptr
store
ret
 */

template <typename T, typename U>
static T*
GetOrNull(const std::unordered_map<U, T*>& map, const U& key)
{
   auto it = map.find(key);
   if (it == map.end()) return NULL;
   return it->second;
}

// template <typename T, typename U>
// static T*
// GetOrNull(const std::unordered_map<const U*, T*>& map, U* key)
// {
//    auto it = map.find(key);
//    if (it == map.end()) return NULL;
//    return it->second;
// }

nv50::Value *
Converter::convert(const Value *val)
{
   nv50::Value *ret = GetOrNull(values, val);
   if (ret)
      return ret;
   // Is it a constant?
   if (Constant::classof(val)) {
      const Constant *c = (const Constant *)val;
      return loadImm(NULL, (uint32_t)c->getUniqueInteger().getSExtValue());
   }
   // XXX handle function arguments
   //assert(false);
   return ret;
}

bool
Converter::convert(const Instruction& i)
{
   const Value *lop[2] = {NULL};
   nv50::Value *op[2] = {NULL};
   for (unsigned c = 0; c < i.getNumOperands() && c < 2; c++) {
      lop[c] = i.getOperand(c);
      op[c] = convert(lop[c]);
   }

   nv50::Value *dst = values[&i];
   if (!dst) {
      dst = values[&i] = getScratch(nv50::TYPE_U32);
   }

   switch (i.getOpcode()) {
      case Instruction::Ret: {
         nv50::BasicBlock *leave = nv50::BasicBlock::get(func->cfgExit);
         assert(leave);

         mkFlow(nv50::OP_RET, NULL, nv50::CC_ALWAYS, NULL)->fixed = 1;
         bb->cfg.attach(&leave->cfg, nv50::Graph::Edge::TREE);

         /* XXX should this go elsewhere? */
         setPosition(leave, true);
         mkOp(nv50::OP_EXIT, nv50::TYPE_NONE, NULL)->terminator = 1;
         break;
      }
      case Instruction::Br: {
         assert(!bb->isTerminated());

         if (i.getNumOperands() == 1) {
            nv50::BasicBlock *dst = GetOrNull(blocks, dyn_cast<const BasicBlock>(i.getOperand(0)));
            mkFlow(nv50::OP_BRA, dst, nv50::CC_ALWAYS, NULL);
            bb->cfg.attach(&dst->cfg, nv50::Graph::Edge::TREE);
            break;
         }

         nv50::BasicBlock *ifBB = GetOrNull(blocks, dyn_cast<const BasicBlock>(i.getOperand(1)));
         nv50::BasicBlock *elseBB = GetOrNull(blocks, dyn_cast<const BasicBlock>(i.getOperand(2)));

         assert(ifBB);
         assert(elseBB);

         mkFlow(nv50::OP_BRA, elseBB, nv50::CC_NOT_P, op[0]);
         mkFlow(nv50::OP_BRA, ifBB, nv50::CC_ALWAYS, NULL);
         bb->cfg.attach(&ifBB->cfg, nv50::Graph::Edge::TREE);
         bb->cfg.attach(&elseBB->cfg, nv50::Graph::Edge::CROSS);
         break;
      }
      case Instruction::ICmp: {
         // XXX figure out the comparison operation
         // XXX convert the type
         mkCmp(nv50::OP_SET, nv50::CC_LT, nv50::TYPE_U32, dst, nv50::TYPE_F32, op[0], op[1]);
         break;
      }
      case Instruction::PHI: {
         /* We could just take the phi nodes directly and try to let things
          * line up with the SSA stage, but that's harder to
          * guarantee. Instead just coalesce all the phi'd values into one
          * register.
          *
          * If the value has not already been seen, just link it up to the
          * destination value. If the value *has* been seen, then replace the
          * existing defs/uses of that value with the new destination,
          * effectively coalescing it all into one variable.
          *
          * Unfortunately there is the additional case of a constant. In that
          * case, we just keep track of it, and at the end, will append it to
          * the start of the basic block from which it is supposed to have
          * come.
          */
         PHINode *node = (PHINode *)&i;
         for (unsigned c = 0; c < node->getNumIncomingValues(); c++) {
            const Value *val = node->getIncomingValue(c);
            nv50::Value *v = GetOrNull(values, val);
            if (!v) {
               values[val] = dst;
               continue;
            }
            const BasicBlock *bb = node->getIncomingBlock(c);
            nv50::BasicBlock *b = GetOrNull(blocks, bb);
            assert(b);
            nv50::Instruction *mov = new_Instruction(func, nv50::OP_MOV, nv50::TYPE_U32);
            mov->setDef(0, dst);
            mov->setSrc(0, v);
            phi_moves.insert(std::make_pair(bb, mov));
         }
         break;
      }
      case Instruction::Store: {
         // XXX address space fixing
         nv50::Symbol *sym = new_Symbol(prog, nv50::FILE_MEMORY_GLOBAL, 15);
         mkStore(nv50::OP_EXPORT, nv50::TYPE_U32, sym, NULL, op[0]);
         break;
      }
      case Instruction::GetElementPtr: {
         // XXX add the base address of the addrspace
         nv50::Symbol *sym = new_Symbol(prog, nv50::FILE_MEMORY_GLOBAL, 15);
         mkLoad(nv50::TYPE_U32, dst, sym, op[1]);
         break;
      }
      case Instruction::Add: {
         mkOp2(nv50::OP_ADD, nv50::TYPE_U32, dst, op[0], op[1]);
         break;
      }
      case Instruction::ZExt: {
         mkMov(dst, op[0]);
         break;
      }


      case Instruction::Switch:
      case Instruction::Unreachable:
      case Instruction::FAdd:
      case Instruction::FSub:
      case Instruction::FMul:
      case Instruction::UDiv:
      case Instruction::SDiv:
      case Instruction::FDiv:
      case Instruction::URem:
      case Instruction::SRem:
      case Instruction::FRem:
      case Instruction::Shl:
      case Instruction::LShr:
      case Instruction::AShr:
      case Instruction::And:
      case Instruction::Or:
      case Instruction::Xor:
      case Instruction::Alloca:
      case Instruction::Load:
      case Instruction::Trunc:
      case Instruction::SExt:
      case Instruction::FPToUI:
      case Instruction::FPToSI:
      case Instruction::UIToFP:
      case Instruction::SIToFP:
      case Instruction::FPTrunc:
      case Instruction::FPExt:
      case Instruction::PtrToInt:
      case Instruction::IntToPtr:
      case Instruction::BitCast:
      case Instruction::FCmp:
      case Instruction::Call:
      case Instruction::Select:
      case Instruction::ExtractElement:
      case Instruction::InsertElement:
      case Instruction::ShuffleVector:
      case Instruction::ExtractValue:
      case Instruction::InsertValue:
         debug_printf("%p = %s %p %p\n", &i, i.getOpcodeName(), lop[0], lop[1]);
         break;
      default:
         debug_printf("Unrecognized instruction: %d, %s; %p = %p %p\n", i.getOpcode(), i.getOpcodeName(), &i, i.getNumOperands() > 0 ? i.getOperand(0) : NULL, i.getNumOperands() > 1 ? i.getOperand(1) : NULL);
         return false;
   }
   return true;
}

bool
Converter::convert(const BasicBlock& bb)
{
   debug_printf("BB: %p\n", &bb);
   this->bb = GetOrNull(blocks, &bb);
   setPosition(this->bb, true);
   for (auto i = bb.begin(); i != bb.end(); ++i) {
      if (!convert(*i))
         return false;
   }

   return true;
}

bool
Converter::convert(const Function& function)
{
   debug_printf("Function: %s\n", function.getName().str().c_str());
   func = GetOrNull(functions, &function);
   assert(func);
   assert(values.size() == 0);
   // Add function parameters to the value map
   prog->main->call.attach(&func->call, nv50::Graph::Edge::TREE);

   // Pre-create all the BB's
   for (auto bb = function.begin(); bb != function.end(); ++bb) {
      blocks[&*bb] = new nv50::BasicBlock(func);
   }

   func->setEntry(blocks[&*function.begin()]);
   func->setExit(new nv50::BasicBlock(func));

   // Convert all the bb's
   for (auto bb = function.begin(); bb != function.end(); ++bb) {
      if (!convert(*bb))
         return false;
   }

   // Add all the phi moves to the ends of the respective bb's. Note that this
   // must be done before the various branches.
   for (auto it = phi_moves.begin(); it != phi_moves.end(); ++it) {
      nv50::BasicBlock *bb = blocks[it->first];
      assert(bb);
      nv50::Instruction *i = bb->getExit();
      while (i && i->op == nv50::OP_BRA)
         i = i->prev;
      if (i)
         bb->insertAfter(i, it->second);
      else
         bb->insertHead(it->second);
   }

   //func->cfg.classifyEdges();

   prog->calls.insert(&func->call);

   values.clear();
   blocks.clear();
   phi_moves.clear();
   return true;
}

bool
Converter::run()
{
   // load bitcode into context
   MemoryBuffer *buffer = MemoryBuffer::getMemBuffer(
         StringRef((const char *)info->bin.source, info->bin.sourceLength),
         StringRef("nouveau"), false);

#if HAVE_LLVM < 0x0305
   Module *module = ParseBitcodeFile(buffer, ctx);
   if (!module) {
      return false;
   }
#else
   auto parse = parseBitcodeFile(buffer, ctx);
   if (parse.getError()) {
      // print error message
      return false;
   }

   Module *module = *parse;
#endif

   // Pre-create all the functions
   for (auto f = module->begin(); f != module->end(); ++f) {
      // XXX figure out what label is useful for
      functions[&*f] = new nv50::Function(
            prog, f->getName().str().c_str(), ~0);
   }

   // Convert the code in each function
   for (auto f = module->begin(); f != module->end(); ++f) {
      if (!convert(*f))
         return false;
   }

   // hook up all of the functions in the call graph

   prog->main->setEntry(new nv50::BasicBlock(prog->main));
   prog->main->setExit(new nv50::BasicBlock(prog->main));

   prog->print();

   return true;
}

} // unnamed namespace

namespace nv50_ir {

bool
Program::makeFromSPIR(struct nv50_ir_prog_info *info)
{
   tlsSize = info->bin.tlsSpace;

   Converter builder(this, info);
   return builder.run();
}

} // namespace nv50_ir
