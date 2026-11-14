#include "nodeseq/seq_node.h"

namespace wax::nodeseq {

const SeqNodeType* findSeqType(const std::string& type) {
  static const TypeTable types = [] {
    TypeTable t;
    addTimeTypes(t);
    addValueTypes(t);
    addChordTypes(t);
    addOutputTypes(t);
    addLogicTypes(t);
    return t;
  }();
  const auto found = types.find(type);
  return found == types.end() ? nullptr : &found->second;
}

}  // namespace wax::nodeseq
