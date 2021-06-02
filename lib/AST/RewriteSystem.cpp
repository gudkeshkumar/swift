//===--- RewriteSystem.cpp - Generics with term rewriting -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/RewriteSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <deque>
#include <vector>

using namespace swift;
using namespace rewriting;

int Atom::compare(Atom other, const ProtocolGraph &graph) const {
  auto kind = getKind();
  auto otherKind = other.getKind();

  if (kind != otherKind)
    return int(kind) < int(otherKind) ? -1 : 1;

  switch (kind) {
  case Kind::Name:
    return getName().compare(other.getName());

  case Kind::Protocol:
    return graph.compareProtocols(getProtocol(), other.getProtocol());

  case Kind::AssociatedType: {
    auto protos = getProtocols();
    auto otherProtos = other.getProtocols();

    // Atoms with more protocols are 'smaller' than those with fewer.
    if (protos.size() != otherProtos.size())
      return protos.size() > otherProtos.size() ? -1 : 1;

    for (unsigned i : indices(protos)) {
      int result = graph.compareProtocols(protos[i], otherProtos[i]);
      if (result)
        return result;
    }

    return getName().compare(other.getName());
  }

  case Kind::GenericParam: {
    auto *param = getGenericParam();
    auto *otherParam = other.getGenericParam();

    if (param->getDepth() != otherParam->getDepth())
      return param->getDepth() < otherParam->getDepth() ? -1 : 1;

    if (param->getIndex() != otherParam->getIndex())
      return param->getIndex() < otherParam->getIndex() ? -1 : 1;

    return 0;
  }

  case Kind::Layout: {
    return getLayoutConstraint().compare(other.getLayoutConstraint());
  }
  }

  llvm_unreachable("Bad atom kind");
}

void Atom::dump(llvm::raw_ostream &out) const {
  switch (getKind()) {
  case Kind::Name:
    out << getName();
    return;

  case Kind::Protocol:
    out << "[" << getProtocol()->getName() << "]";
    return;

  case Kind::AssociatedType: {
    out << "[";
    bool first = true;
    for (const auto *proto : getProtocols()) {
      if (first) {
        first = false;
      } else {
        out << "&";
      }
      out << proto->getName();
    }
    out << ":" << getName() << "]";
    return;
  }

  case Kind::GenericParam:
    out << Type(getGenericParam());
    return;

  case Kind::Layout:
    out << "[layout: ";
    getLayoutConstraint()->print(out);
    out << "]";
  }

  llvm_unreachable("Bad atom kind");
}

int Term::compare(const Term &other, const ProtocolGraph &graph) const {
  if (size() != other.size())
    return size() < other.size() ? -1 : 1;

  for (unsigned i = 0, e = size(); i < e; ++i) {
    auto lhs = (*this)[i];
    auto rhs = other[i];

    int result = lhs.compare(rhs, graph);
    if (result != 0) {
      assert(lhs != rhs);
      return result;
    }

    assert(lhs == rhs);
  }

  return 0;
}

decltype(Term::Atoms)::const_iterator
Term::findSubTerm(const Term &other) const {
  if (other.size() > size())
    return end();

  return std::search(begin(), end(), other.begin(), other.end());
}

decltype(Term::Atoms)::iterator
Term::findSubTerm(const Term &other) {
  if (other.size() > size())
    return end();

  return std::search(begin(), end(), other.begin(), other.end());
}

bool Term::rewriteSubTerm(const Term &lhs, const Term &rhs) {
  auto found = findSubTerm(lhs);
  if (found == end())
    return false;

  auto oldSize = size();

  assert(rhs.size() <= lhs.size());

  auto newIter = std::copy(rhs.begin(), rhs.end(), found);
  auto oldIter = found + lhs.size();
  if (newIter != oldIter) {
    auto newEnd = std::copy(oldIter, end(), newIter);
    Atoms.erase(newEnd, end());
  }

  assert(size() == oldSize - lhs.size() + rhs.size());
  return true;
}

bool Term::checkForOverlap(const Term &other, Term &result) const {
  assert(result.size() == 0);

  if (other.size() > size())
    return false;

  auto first1 = begin();
  auto last1 = end();
  auto first2 = other.begin();
  auto last2 = other.end();

  while (last2 - first2 <= last1 - first1) {
    if (std::equal(first2, last2, first1)) {
      result = *this;
      return true;
    }

    ++first1;
  }

  while (first1 != last1) {
    --last2;

    if (std::equal(first1, last1, first2)) {
      std::copy(begin(), first1,
                std::back_inserter(result.Atoms));
      std::copy(other.begin(), other.end(),
                std::back_inserter(result.Atoms));
      return true;
    }

    ++first1;
  }

  return false;
}

void Term::dump(llvm::raw_ostream &out) const {
  bool first = true;

  for (auto atom : Atoms) {
    if (!first)
      out << ".";
    else
      first = false;

    atom.dump(out);
  }
}

void Rule::dump(llvm::raw_ostream &out) const {
  LHS.dump(out);
  out << " => ";
  RHS.dump(out);
  if (deleted)
    out << " [deleted]";
}

void RewriteSystem::initialize(std::vector<std::pair<Term, Term>> &&rules,
                               ProtocolGraph &&graph) {
  Protos = graph;

  // FIXME: Probably this sort is not necessary
  std::sort(rules.begin(), rules.end(),
            [&](std::pair<Term, Term> lhs,
                std::pair<Term, Term> rhs) -> int {
              return lhs.first.compare(rhs.first, graph) < 0;
            });
  for (const auto &rule : rules)
    addRule(rule.first, rule.second);
}

bool RewriteSystem::addRule(Term lhs, Term rhs) {
  simplify(lhs);
  simplify(rhs);

  int result = lhs.compare(rhs, Protos);
  if (result == 0)
    return false;
  if (result < 0)
    std::swap(lhs, rhs);

  assert(lhs.compare(rhs, Protos) > 0);

  if (DebugAdd) {
    llvm::dbgs() << "# Adding rule ";
    lhs.dump(llvm::dbgs());
    llvm::dbgs() << " => ";
    rhs.dump(llvm::dbgs());
    llvm::dbgs() << "\n";
  }

  unsigned i = Rules.size();
  Rules.emplace_back(lhs, rhs);

  if (lhs.size() == rhs.size() &&
      std::equal(lhs.begin(), lhs.end() - 1, rhs.begin()) &&
      lhs.back().getKind() == Atom::Kind::AssociatedType &&
      rhs.back().getKind() == Atom::Kind::AssociatedType &&
      lhs.back().getName() == rhs.back().getName()) {
    MergedAssociatedTypes.emplace_back(lhs, rhs);
  }

  for (unsigned j : indices(Rules)) {
    if (i == j)
      continue;

    Worklist.emplace_back(i, j);
    Worklist.emplace_back(j, i);
  }

  return true;
}

bool RewriteSystem::simplify(Term &term) const {
  bool changed = false;

  if (DebugSimplify) {
    llvm::dbgs() << "= Term ";
    term.dump(llvm::dbgs());
    llvm::dbgs() << "\n";
  }

  while (true) {
    bool tryAgain = false;
    for (const auto &rule : Rules) {
      if (rule.isDeleted())
        continue;

      if (DebugSimplify) {
        llvm::dbgs() << "== Rule ";
        rule.dump(llvm::dbgs());
        llvm::dbgs() << "\n";
      }

      if (rule.apply(term)) {
        if (DebugSimplify) {
          llvm::dbgs() << "=== Result ";
          term.dump(llvm::dbgs());
          llvm::dbgs() << "\n";
        }

        changed = true;
        tryAgain = true;
      }
    }

    if (!tryAgain)
      break;
  }

  return changed;
}

Atom RewriteSystem::mergeAssociatedTypes(Atom lhs, Atom rhs) const {
  assert(lhs.getKind() == Atom::Kind::AssociatedType);
  assert(rhs.getKind() == Atom::Kind::AssociatedType);
  assert(lhs.getName() == rhs.getName());
  assert(lhs.compare(rhs, Protos) > 0);

  auto protos = lhs.getProtocols();
  auto otherProtos = rhs.getProtocols();

  // Follows from lhs > rhs
  assert(protos.size() <= otherProtos.size());

  llvm::TinyPtrVector<const ProtocolDecl *> newProtos;
  std::merge(protos.begin(), protos.end(),
             otherProtos.begin(), otherProtos.end(),
             std::back_inserter(newProtos),
             [&](const ProtocolDecl *lhs,
                 const ProtocolDecl *rhs) -> int {
               return Protos.compareProtocols(lhs, rhs) < 0;
             });

  llvm::TinyPtrVector<const ProtocolDecl *> minimalProtos;
  for (const auto *newProto : newProtos) {
    auto inheritsFrom = [&](const ProtocolDecl *thisProto) {
      return (thisProto == newProto ||
              Protos.inheritsFrom(thisProto, newProto));
    };

    if (std::find_if(protos.begin(), protos.end(), inheritsFrom)
        == protos.end()) {
      minimalProtos.push_back(newProto);
    }
  }

  assert(minimalProtos.size() >= protos.size());
  assert(minimalProtos.size() >= otherProtos.size());

  return Atom::forAssociatedType(minimalProtos, lhs.getName());
}

void RewriteSystem::processMergedAssociatedTypes() {
  if (MergedAssociatedTypes.empty())
    return;

  unsigned i = 0;
  while (i < MergedAssociatedTypes.size()) {
    auto pair = MergedAssociatedTypes[i++];
    const auto &lhs = pair.first;
    const auto &rhs = pair.second;

    // If we have ...[P1:T] => ...[P2:T], add a new pair of rules:
    // ...[P1:T] => ...[P1&P2:T]
    // ...[P2:T] => ...[P1&P2:T]
    if (DebugMerge) {
      llvm::dbgs() << "## Associated type merge candidate ";
      lhs.dump(llvm::dbgs());
      llvm::dbgs() << " => ";
      rhs.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }

    auto mergedAtom = mergeAssociatedTypes(lhs.back(), rhs.back());
    if (DebugMerge) {
      llvm::dbgs() << "### Merged atom ";
      mergedAtom.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }

    Term mergedTerm = lhs;
    mergedTerm.back() = mergedAtom;

    addRule(lhs, mergedTerm);
    addRule(rhs, mergedTerm);

    for (const auto &otherRule : Rules) {
      const auto &otherLHS = otherRule.getLHS();
      if (otherLHS.size() == 2 &&
          otherLHS[1].getKind() == Atom::Kind::Protocol) {
        if (otherLHS[0] == lhs.back() ||
            otherLHS[0] == rhs.back()) {
          if (DebugMerge) {
            llvm::dbgs() << "### Lifting conformance rule ";
            otherRule.dump(llvm::dbgs());
            llvm::dbgs() << "\n";
          }

          auto otherRHS = otherRule.getRHS();
          assert(otherRHS.size() == 1);
          assert(otherRHS[0] == otherLHS[0]);

          otherRHS.back() = mergedAtom;

          auto newLHS = otherRHS;
          newLHS.add(Atom::forProtocol(otherLHS[1].getProtocol()));

          addRule(newLHS, otherRHS);
        }
      }
    }
  }

  MergedAssociatedTypes.clear();
}

RewriteSystem::CompletionResult
RewriteSystem::computeConfluentCompletion(unsigned maxIterations,
                                          unsigned maxDepth) {
  while (!Worklist.empty()) {
    auto pair = Worklist.front();
    Worklist.pop_front();

    Term first;

    const auto &lhs = Rules[pair.first];
    const auto &rhs = Rules[pair.second];

    if (lhs.isDeleted() || rhs.isDeleted())
      continue;

    if (!lhs.checkForOverlap(rhs, first))
      continue;

    assert(first.size() > 0);

    Term second = first;

    lhs.apply(first);
    rhs.apply(second);

    unsigned i = Rules.size();

    if (!addRule(first, second))
      continue;

    if (maxIterations == 0)
      return CompletionResult::MaxIterations;

    maxIterations--;

    const auto &newRule = Rules[i];
    if (newRule.getDepth() > maxDepth)
      return CompletionResult::MaxDepth;

    for (unsigned j : indices(Rules)) {
      if (i == j)
        continue;

      auto &rule = Rules[j];

      if (rule.isDeleted())
        continue;

      if (rule.canReduceLeftHandSide(newRule))
        rule.markDeleted();
    }

    processMergedAssociatedTypes();
  }

  // This isn't necessary for correctness, it's just an optimization.
  for (auto &rule : Rules) {
    auto rhs = rule.getRHS();
    simplify(rhs);
    rule = Rule(rule.getLHS(), rhs);
  }

  // Just for aesthetics in dump().
  std::sort(Rules.begin(), Rules.end(),
            [&](Rule lhs, Rule rhs) -> int {
              return lhs.getLHS().compare(rhs.getLHS(), Protos) < 0;
            });

  return CompletionResult::Success;
}

void RewriteSystem::dump(llvm::raw_ostream &out) const {
  out << "Rewrite system: {\n";
  for (const auto &rule : Rules) {
    out << "- ";
    rule.dump(out);
    out << "\n";
  }
  out << "}\n";
}
