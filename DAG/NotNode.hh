/*
 * d4
 * Copyright (C) 2020  Univ. Artois & CNRS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef DAG_NotNode_h
#define DAG_NotNode_h

#include "DAG.hh"

template <class T> class DAG;

/**
 * NOT node for d4-dual-negation algorithm.
 * Represents the logical negation of its child node.
 *
 * Output format:
 *   n <node_id> 0           // NOT node declaration
 *   <not_id> <child_id> 0   // Edge to child
 */
template <class T> class notNode : public DAG<T> {
  using DAG<T>::nbEdges;
  using DAG<T>::globalStamp;
  using DAG<T>::idxOutputStruct;
  using DAG<T>::stamp;

public:
  DAG<T> *child;
  T nbModels;
  int numFreeVars; // Number of free variables for complement calculation

  notNode() : child(nullptr), numFreeVars(0) {}

  notNode(DAG<T> *c) : child(c), numFreeVars(0) { nbEdges++; }

  notNode(DAG<T> *c, int nFreeVars) : child(c), numFreeVars(nFreeVars) {
    nbEdges++;
  }

  inline int getSize_() {
    if (stamp == globalStamp)
      return 0;
    stamp = globalStamp;
    return 1 + (child ? child->getSize_() : 0);
  }

  inline void printNNF(std::ostream &out, bool certif) {
    if (stamp >= globalStamp)
      return;
    stamp = globalStamp + idxOutputStruct + 1;
    int idxCurrent = ++idxOutputStruct;

    // First print the NOT node declaration
    out << "n " << idxCurrent << " 0" << std::endl;

    // Print child node
    if (child)
      child->printNNF(out, certif);

    // Print edge from NOT node to child
    if (child)
      out << idxCurrent << " " << child->getIdx() << " 0" << std::endl;
  }

  inline bool isSAT(vec<Lit> &unitsLitBranches) {
    // NOT node is SAT if child is UNSAT
    if (child)
      return !child->isSAT(unitsLitBranches);
    return true; // NOT of nothing is true
  }

  inline T computeNbModels() {
    if (stamp == globalStamp)
      return nbModels;
    stamp = globalStamp;

    // Simple pass-through (model counting needs separate work)
    if (child)
      nbModels = child->computeNbModels();
    else
      nbModels = 0;

    return nbModels;
  }
};
#endif
