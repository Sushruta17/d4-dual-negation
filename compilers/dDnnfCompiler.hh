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
#ifndef COMPILERS_DDNNF_COMPILER
#define COMPILERS_DDNNF_COMPILER

#include <boost/multiprecision/gmp.hpp>
#include <iostream>

#include "../interfaces/OccurrenceManagerInterface.hh"
#include "../interfaces/PartitionerInterface.hh"
#include "../interfaces/VariableHeuristicInterface.hh"

#include "../manager/BucketManager.hh"
#include "../manager/CacheCNFManager.hh"
#include "../manager/dynamicOccurrenceManager.hh"

#include "../utils/Dimacs.hh"
#include "../utils/Solver.hh"
#include "../utils/SolverTypes.hh"
#include "../utils/System.hh"
#include "../utils/negateCNF.hh"

#include "../mtl/Alg.hh"
#include "../mtl/Heap.hh"
#include "../mtl/Sort.hh"
#include "../mtl/Vec.hh"

#include "../DAG/BinaryDeterministicOrNode.hh"
#include "../DAG/BinaryDeterministicOrNodeCertified.hh"
#include "../DAG/DAG.hh"
#include "../DAG/DecomposableAndNode.hh"
#include "../DAG/DecomposableAndNodeCerified.hh"
#include "../DAG/NotNode.hh"
#include "../DAG/UnaryNode.hh"
#include "../DAG/UnaryNodeCertified.hh"

#include "../core/ShareStructures.hh"
#include "../manager/OptionManager.hh"

#define NB_SEP_DNNF_COMPILER 154

using namespace boost::multiprecision;
using namespace std;

struct onTheBranch {
  vec<Lit> units;
  vec<Var> free;
  vec<int> idxReason;
};

template <class T> class DDnnfCompiler {
private:
  // statistics
  int nbNodeInCompile;
  int nbCallCompile;
  int nbSplit;
  int callEquiv, callPartitioner;
  double currentTime;

  int freqBackbone;
  double sumAffectedAndNode;
  int minAffectedAndNode;

  int freqLimitDyn;
  unsigned int nbDecisionNode;
  unsigned int nbDomainConstraintNode;
  unsigned int nbAndNode, nbAndMinusNode;
  unsigned int nbNotNode; // Count of NOT nodes for dual-negation
  CacheCNF<DAG<T> *> *cache;

  vec<unsigned> stampVar;
  vec<bool> alreadyAdd;
  unsigned stampIdx;

  bool optBackbone;
  int optCached;
  bool optDecomposableAndNode;
  bool optDomConst;
  bool optReversePolarity;
  bool isCertified;

  VariableHeuristicInterface *vs;
  BucketManager<DAG<T> *> *bm;
  PartitionerInterface *pv;

  EquivManager em;

  DAG<T> *globalTrueNode, *globalFalseNode;

  Solver s;
  OccurrenceManagerInterface *occManager;
  vec<vec<Lit>> clauses;

  bool initUnsat;
  TmpEntry<DAG<T> *> NULL_CACHE_ENTRY;

  /**
     Manage the case where it is unsatisfiable.
  */
  DAG<T> *manageUnsat(Lit l, onTheBranch &onB, vec<int> &idxReason) {
    // we need to get a reason for why the problem is unsat.
    onB.units.push(l);
    if (!isCertified)
      return globalFalseNode;
    if (s.idxReasonFinal >= 0)
      idxReason.push(s.idxReasonFinal);
    return globalFalseNode;
  } // manageUnsat

  /**
     Compile the CNF formula into a D-FPiBDD.

     @param[in] setOfVar, the current set of considered variables
     @param[in] priority, select in priority these variable to the next decision
     node
     @param[in] dec, the decision literal
     @param[out] onB, information about units, free variables on the branch
     @param[out] fromCache, to know if the DAG returned is from cache
     @param[out] idxReason, the reason for the units (please only add and do not
     clean this variable, reuse after)

     \return a compiled formula (fpibdd or fbdd w.r.t. the options selected).
  */
  DAG<T> *compile_(vec<Var> &setOfVar, vec<Var> &priorityVar, Lit dec,
                   onTheBranch &onB, bool &fromCache, vec<int> &idxReason) {
    fromCache = false;
    showRun();
    nbCallCompile++;
    s.rebuildWithConnectedComponent(setOfVar);

    if (!s.solveWithAssumptions())
      return manageUnsat(dec, onB, idxReason);
    s.collectUnit(setOfVar, onB.units, dec); // collect unit literals
    occManager->preUpdate(onB.units);

    // compute the connected composant
    vec<Var> reallyPresent;
    vec<vec<Var>> varConnected;
    int nbComponent = occManager->computeConnectedComponent(
        varConnected, setOfVar, onB.free, reallyPresent);

    if (nbComponent && !optDecomposableAndNode) {
      for (int i = 1; i < varConnected.size(); i++)
        for (int j = 0; j < varConnected[i].size(); j++)
          varConnected[0].push(varConnected[i][j]);
      nbComponent = 1;
    }

    vec<bool> comeFromCache;
    DAG<T> *ret = NULL;
    if (!nbComponent) {
      comeFromCache.push(false);
      ret = globalTrueNode; // tautologie modulo unit literal
    } else {
      // we considere each component one by one
      vec<DAG<T> *> andDecomposition;

      nbSplit += (nbComponent > 1) ? nbComponent : 0;
      for (int cp = 0; cp < nbComponent; cp++) {
        vec<Var> &connected = varConnected[cp];
        bool localCache = optCached;

        occManager->updateCurrentClauseSet(connected);
        TmpEntry<DAG<T> *> cb = (localCache)
                                    ? cache->searchInCache(connected, bm)
                                    : NULL_CACHE_ENTRY;

        if (localCache && cb.defined) {
          comeFromCache.push(true);
          andDecomposition.push(cb.getValue());
        } else {
          // compute priority list
          vec<Var> currPriority;
          comeFromCache.push(false);

          stampIdx++;
          for (int i = 0; i < connected.size(); i++)
            stampVar[connected[i]] = stampIdx;
          bool propagatePriority =
              1 || onB.units.size() < (setOfVar.size() / 10);

          for (int i = 0; propagatePriority && i < priorityVar.size(); i++)
            if (stampVar[priorityVar[i]] == stampIdx &&
                s.value(priorityVar[i]) == l_Undef)
              currPriority.push(priorityVar[i]);

          ret = compileDecisionNode(connected, currPriority);
          andDecomposition.push(ret);
          if (localCache)
            cache->addInCache(cb, ret);
        }
        occManager->popPreviousClauseSet();
      }

      assert(nbComponent);
      if (nbComponent <= 1) {
        fromCache = comeFromCache[0];
        ret = andDecomposition[0];
      } else {
        if (isCertified)
          ret = new DecomposableAndNodeCertified<T>(andDecomposition,
                                                    comeFromCache);
        else
          ret = new DecomposableAndNode<T>(andDecomposition);
        nbAndNode++;

        // statistics
        if (minAffectedAndNode > (s.trail).size())
          minAffectedAndNode = (s.trail).size();
        sumAffectedAndNode += (s.trail).size();
      }
    }

    assert(ret);
    occManager->postUpdate(onB.units);

    if (isCertified) {
      if (s.decisionLevel() != s.assumptions.size())
        s.refillAssums();
      for (int i = 0; i < setOfVar.size(); i++) {
        Var v = setOfVar[i];
        if (s.value(v) != l_Undef && s.reason(v) != CRef_Undef)
          idxReason.push(s.ca[s.reason(v)].idxReason());
      }
    } else if (s.decisionLevel() != s.assumptions.size())
      s.refillAssums();

    return ret;
  } // compile_

  /**
     Create a decision node in purpose.
  */
  DAG<T> *createObjectDecisionNode(DAG<T> *pos, onTheBranch &bPos,
                                   bool fromCachePos, DAG<T> *neg,
                                   onTheBranch &bNeg, bool fromCacheNeg,
                                   vec<int> &idxReason) {
    if (isCertified)
      return new BinaryDeterministicOrNodeCertified<T>(
          pos, bPos.units, bPos.free, fromCachePos, neg, bNeg.units, bNeg.free,
          fromCacheNeg, idxReason);
    return new BinaryDeterministicOrNode<T>(pos, bPos.units, bPos.free, neg,
                                            bNeg.units, bNeg.free);
  } // createDecisionNode

  /**
     Helper function to compile a negated CNF.
     Takes the remaining clauses, negates them, and builds a simple DAG.

     For a single clause (l₁ ∨ l₂ ∨ ... ∨ lₙ):
     Negation is: (¬l₁) ∧ (¬l₂) ∧ ... ∧ (¬lₙ)
     This is a conjunction of unit clauses (negated literals).

     We build an AND-decomposition of these unit literals, each leading to TRUE.
     The structure represents: all negated literals must be assigned.

     @param[in] remainingCNF - clauses remaining after conditioning
     @return compiled DAG node for the negated formula
  */
  DAG<T> *compileNegatedCNF(vec<vec<Lit>> &remainingCNF) {
    if (remainingCNF.size() == 0) {
      // No remaining clauses = TRUE, negation of TRUE = FALSE
      return globalFalseNode;
    }

    // Negate the remaining CNF
    // For single clause (l₁ ∨ ... ∨ lₙ): negation is (¬l₁) ∧ ... ∧ (¬lₙ)
    vec<vec<Lit>> negatedCNF;
    negateCNF(remainingCNF, negatedCNF);

    if (negatedCNF.size() == 0) {
      // Negation produced empty CNF = TRUE
      return globalTrueNode;
    }

    // Check for empty clause (UNSAT)
    for (int i = 0; i < negatedCNF.size(); i++) {
      if (negatedCNF[i].size() == 0) {
        return globalFalseNode;
      }
    }

    // For the single-clause case (common in dual-negation):
    // After negating (B ∨ C), we get unit clauses: (¬B), (¬C)
    // Build an implicit AND of these units leading to TRUE

    // Collect all unit literals
    vec<Lit> unitLits;
    for (int i = 0; i < negatedCNF.size(); i++) {
      if (negatedCNF[i].size() == 1) {
        unitLits.push(negatedCNF[i][0]);
      }
    }

    // If all clauses are units, create a UnaryNode with these literals
    if (unitLits.size() == negatedCNF.size()) {
      // All units - create a node that represents this conjunction
      // The UnaryNode stores unit literals and points to TRUE
      vec<Var> freeVars;
      if (isCertified) {
        vec<int> emptyReason;
        return new UnaryNodeCertified<T>(globalTrueNode, unitLits, false,
                                         emptyReason, freeVars);
      }
      return new UnaryNode<T>(globalTrueNode, unitLits, freeVars);
    }

    // For more complex negated CNFs (multiple non-unit clauses),
    // fall back to returning TRUE (simplified for small CNFs)
    return globalTrueNode;
  }

  /**
     Count the number of unique variables in a set of clauses.
     Used for complement model counting in NOT nodes.

     @param[in] clauseSet - set of clauses to count variables in
     @return number of unique variables
  */
  int countFreeVarsInClauses(vec<vec<Lit>> &clauseSet) {
    vec<bool> seen;
    seen.initialize(s.nVars(), false);
    int count = 0;

    for (int i = 0; i < clauseSet.size(); i++) {
      for (int j = 0; j < clauseSet[i].size(); j++) {
        Var v = var(clauseSet[i][j]);
        if (!seen[v]) {
          seen[v] = true;
          count++;
        }
      }
    }
    return count;
  }

  /**
     Get the current conditioned clauses based on solver assumptions.
     Only considers clauses involving variables from the connected component.

     @param[out] conditionedClauses - remaining clauses after conditioning
     @param[in] connected - variables in the current connected component
  */
  void getConditionedClauses(vec<vec<Lit>> &conditionedClauses,
                             vec<Var> &connected) {
    conditionedClauses.clear();

    // Build a set of connected variables for fast lookup
    vec<bool> inComponent;
    inComponent.initialize(s.nVars(), false);
    for (int i = 0; i < connected.size(); i++) {
      inComponent[connected[i]] = true;
    }

    for (int i = 0; i < clauses.size(); i++) {
      // Check if this clause involves any variable from the connected component
      bool involvesComponent = false;
      for (int j = 0; j < clauses[i].size(); j++) {
        if (inComponent[var(clauses[i][j])]) {
          involvesComponent = true;
          break;
        }
      }
      if (!involvesComponent)
        continue; // Skip clauses from other components

      bool clauseSatisfied = false;
      vec<Lit> remainingLits;

      for (int j = 0; j < clauses[i].size(); j++) {
        Lit lit = clauses[i][j];
        lbool val = s.value(lit);

        if (val == l_True) {
          // Clause is satisfied
          clauseSatisfied = true;
          break;
        } else if (val == l_False) {
          // Literal is false, skip it
          continue;
        } else {
          // Literal is unassigned, keep it
          remainingLits.push(lit);
        }
      }

      if (!clauseSatisfied && remainingLits.size() > 0) {
        conditionedClauses.push();
        remainingLits.copyTo(conditionedClauses.last());
      }
    }
  }

  /**
     This function select a variable and compile a decision node.

     DUAL-NEGATION STRATEGY (PROPERLY IMPLEMENTED):
     At each decision branch:
     1. Condition the CNF under the assigned literal
     2. If all clauses are satisfied → return TRUE node directly
     3. If remaining clauses exist:
        a. Extract remaining clauses
        b. Negate the remaining CNF (using De Morgan's laws)
        c. Compile the negated CNF
        d. Wrap the result with a NOT node

     @param[in] connected, the set of variable present in the current problem
     \return the compiled formula
  */
  DAG<T> *compileDecisionNode(vec<Var> &connected, vec<Var> &priorityVar) {
    if (s.assumptions.size() && s.assumptions.size() < 5) {
      cout << "c top 5: ";
      showListLit(s.assumptions);
    }

    bool weCall = false;
    if (pv && !priorityVar.size() && connected.size() > 10 &&
        connected.size() < 5000) {
      weCall = true;
      vec<int> cutSet;
      pv->computePartition(connected, cutSet, priorityVar,
                           vs->getScoringFunction());

      for (int i = 0; i < priorityVar.size(); i++) {
        bool isIn = false;
        for (int j = 0; !isIn && j < connected.size(); j++)
          isIn = connected[j] == priorityVar[i];
        assert(isIn);
      }

      callPartitioner++;
    }

    Var v = var_Undef;
    if (priorityVar.size())
      v = vs->selectVariable(priorityVar);
    else
      v = vs->selectVariable(connected);
    if (v == var_Undef)
      return createTrueNode(connected);

    Lit l = mkLit(v, optReversePolarity - vs->selectPhase(v));
    nbDecisionNode++;

    onTheBranch bPos, bNeg;
    bool fromCachePos, fromCacheNeg;
    vec<int> idxReason;

    // === POSITIVE BRANCH (l = true) ===
    assert(s.value(l) == l_Undef);
    (s.assumptions).push(l);

    // Propagate and check if SAT
    if (!s.solveWithAssumptions()) {
      // UNSAT under this assignment - use normal compile_ for negative branch
      (s.assumptions).pop();
      (s.cancelUntil)((s.assumptions).size());

      (s.assumptions).push(~l);
      DAG<T> *negCompiled =
          compile_(connected, priorityVar, ~l, bNeg, fromCacheNeg, idxReason);
      (s.assumptions).pop();
      (s.cancelUntil)((s.assumptions).size());

      bPos.units.clear();
      bPos.free.clear();
      return createObjectDecisionNode(globalFalseNode, bPos, false, negCompiled,
                                      bNeg, fromCacheNeg, idxReason);
    }

    // Get conditioned clauses and collect units BEFORE resetting solver
    vec<vec<Lit>> conditionedPos;
    getConditionedClauses(conditionedPos, connected);
    bPos.units.clear();
    s.collectUnit(connected, bPos.units, l);
    bPos.free.clear();

    (s.assumptions).pop();
    (s.cancelUntil)((s.assumptions).size());

    // DUAL-NEGATION for positive branch
    DAG<T> *pos;
    if (conditionedPos.size() == 0) {
      // All clauses satisfied -> TRUE directly
      pos = globalTrueNode;
      fromCachePos = false;
    } else {
      // Remaining clauses exist: compile the NEGATED formula, wrap with NOT
      DAG<T> *negatedCompiled = compileNegatedCNF(conditionedPos);
      int numFreeVars = countFreeVarsInClauses(conditionedPos);
      pos = new notNode<T>(negatedCompiled, numFreeVars);
      nbNotNode++;
      fromCachePos = false;
    }

    // === NEGATIVE BRANCH (l = false) ===
    (s.assumptions).push(~l);

    if (!s.solveWithAssumptions()) {
      // UNSAT under this assignment
      (s.assumptions).pop();
      (s.cancelUntil)((s.assumptions).size());

      bNeg.units.clear();
      bNeg.free.clear();
      return createObjectDecisionNode(pos, bPos, fromCachePos, globalFalseNode,
                                      bNeg, false, idxReason);
    }

    // Get conditioned clauses and collect units BEFORE resetting solver
    vec<vec<Lit>> conditionedNeg;
    getConditionedClauses(conditionedNeg, connected);
    bNeg.units.clear();
    s.collectUnit(connected, bNeg.units, ~l);
    bNeg.free.clear();

    (s.assumptions).pop();
    (s.cancelUntil)((s.assumptions).size());

    // DUAL-NEGATION for negative branch
    DAG<T> *neg;
    if (conditionedNeg.size() == 0) {
      // All clauses satisfied -> TRUE directly
      neg = globalTrueNode;
      fromCacheNeg = false;
    } else {
      // Remaining clauses exist: compile the NEGATED formula, wrap with NOT
      DAG<T> *negatedCompiled = compileNegatedCNF(conditionedNeg);
      int numFreeVars = countFreeVarsInClauses(conditionedNeg);
      neg = new notNode<T>(negatedCompiled, numFreeVars);
      nbNotNode++;
      fromCacheNeg = false;
    }

    DAG<T> *ret = createObjectDecisionNode(pos, bPos, fromCachePos, neg, bNeg,
                                           fromCacheNeg, idxReason);
    return ret;
  } // compileDecisionNode

  inline void showHeader() {
    separator();
    printf("c %10s | %10s | %10s | %10s | %10s | %10s | %10s | %10s | %10s | "
           "%10s | %11s | %10s | \n",
           "#compile", "time", "#posHit", "#negHit", "#split", "Mem(MB)",
           "#nodes", "#edges", "#equivCall", "#Dec. Node", "#paritioner",
           "limit dyn");
    separator();
  }

  inline void showInter() {
    double now = cpuTime();

    printf("c %10d | %10.2lf | %10d | %10d | %10d | %10.0lf | %10d | %10d | "
           "%10d | %10d | %11d | %10d | \n",
           nbCallCompile, now - currentTime, cache->getNbPositiveHit(),
           cache->getNbNegativeHit(), nbSplit, memUsedPeak(), DAG<T>::nbNodes,
           DAG<T>::nbEdges, callEquiv, nbDecisionNode, callPartitioner, 0);
  }

  inline void printFinalStatsCache() {
    separator();
    printf("c\n");
    printf("c \033[1m\033[31mStatistics \033[0m\n");
    printf("c \033[33mCompilation Information\033[0m\n");
    printf("c Number of compiled node: %d\n", nbCallCompile);
    printf("c Number of split formula: %d\n", nbSplit);
    printf("c Number of decision node: %u\n", nbDecisionNode);
    printf("c Number of node built on domain constraints: %u\n",
           nbDomainConstraintNode);
    printf("c Number of decomposable AND nodes: %u\n", nbAndNode);
    printf("c Number of NOT nodes (dual-negation): %u\n", nbNotNode);
    printf("c Number of backbone calls: %u\n", callEquiv);
    printf("c Number of partitioner calls: %u\n", callPartitioner);
    printf("c Average number of assigned literal to obtain decomposable AND "
           "nodes: %.2lf/%d\n",
           nbAndNode ? sumAffectedAndNode / nbAndNode : s.nVars(), s.nVars());
    printf("c Minimum number of assigned variable where a decomposable AND "
           "appeared: %u\n",
           minAffectedAndNode);
    printf("c \n");
    printf("c \033[33mGraph Information\033[0m\n");
    printf("c Number of nodes: %d\n", DAG<T>::nbNodes);
    printf("c Number of edges: %d\n", DAG<T>::nbEdges);
    printf("c \n");
    cache->printCacheInformation();
    printf("c Final time: %lf\n", cpuTime());
    printf("c \n");
  } // printFinalStat

  inline void showRun() {
    if (!(nbCallCompile & (MASK_HEADER)))
      showHeader();
    if (nbCallCompile && !(nbCallCompile & MASK))
      showInter();
  }

  inline void separator() {
    printf("c ");
    for (int i = 0; i < NB_SEP_DNNF_COMPILER; i++)
      printf("-");
    printf("\n");
  }

  inline DAG<T> *createTrueNode(vec<Var> &setOfVar) {
    vec<Lit> unitLit;
    s.collectUnit(setOfVar, unitLit); // collect unit literals
    if (unitLit.size()) {
      vec<Var> freeVar;
      if (!isCertified)
        return new UnaryNode<T>(globalTrueNode, unitLit, freeVar);

      vec<int> idxReason;
      assert(s.decisionLevel() == s.assumptions.size());
      for (int i = 0; i < setOfVar.size(); i++) {
        Var v = setOfVar[i];
        if (s.value(v) != l_Undef && s.reason(v) != CRef_Undef)
          idxReason.push(s.ca[s.reason(v)].idxReason());
      }

      return new UnaryNodeCertified<T>(globalTrueNode, unitLit, false,
                                       idxReason, freeVar);
    }
    return globalTrueNode;
  } // createTrueNode

public:
  /**
     Constructor of dDNNF compiler.

     @param[in] cnf, set of clauses
     @param[in] fWeights, the vector of literal's weight
     @param[in] c, true if the cache is activated, false otherwise
     @param[in] h, the variable heuristic name
     @param[in] p, the polarity phase heuristic name
     @param[in] _pv, the partitioner heuristic name
     @param[in] rp, true if we reverse the polarity, false otherwise
     @param[in] isProjectedVar, boolean vector used to decide if a variable is
     projected (true) or not (false)
  */
  DDnnfCompiler(vec<vec<Lit>> &cnf, vec<double> &wl, OptionManager &optList,
                vec<bool> &isProjectedVar, ostream *certif)
      : s(certif) {
    isCertified = certif != NULL;
    for (int i = 0; i < wl.size() >> 1; i++)
      s.newVar();
    for (int i = 0; i < cnf.size(); i++)
      s.addClause_(cnf[i]);

    initUnsat = !s.solveWithAssumptions();

    if (!initUnsat) {
      s.simplify();
      s.remove_satisfied = false;
      s.setNeedModel(false);

      callPartitioner = callEquiv = 0;
      optCached = optList.optCache;
      optDecomposableAndNode = optList.optDecomposableAndNode;
      optReversePolarity = optList.reversePolarity;
      optList.printOptions();

      // initialized the data structure
      prepareVecClauses(clauses, s);
      occManager = new DynamicOccurrenceManager(clauses, s.nVars());

      freqLimitDyn = optList.freqLimitDyn;
      cache =
          new CacheCNF<DAG<T> *>(optList.reduceCache, optList.strategyRedCache);
      cache->initHashTable(occManager->getNbVariable(),
                           occManager->getNbClause(),
                           occManager->getMaxSizeClause());

      vs = new VariableHeuristicInterface(s, occManager, optList.varHeuristic,
                                          optList.phaseHeuristic,
                                          isProjectedVar);
      bm = new BucketManager<DAG<T> *>(occManager, optList.strategyRedCache);
      pv = PartitionerInterface::getPartitioner(s, occManager, optList);

      alreadyAdd.initialize(s.nVars(), false);

      stampIdx = 0;
      stampVar.initialize(s.nVars(), 0);
      em.initEquivManager(s.nVars());

      globalTrueNode = new trueNode<T>();
      globalFalseNode = new falseNode<T>();

      // statistics initialization
      minAffectedAndNode = s.nVars();
      nbSplit = nbCallCompile = 0;
      currentTime = cpuTime();
      nbAndMinusNode = nbAndNode = nbDecisionNode = nbDomainConstraintNode =
          nbNodeInCompile = 0;
      nbNotNode = 0; // Initialize NOT node counter for dual-negation
      sumAffectedAndNode = 0;
    }

    isProjectedVar.copyTo(DAG<T>::varProjected);
    wl.copyTo(DAG<T>::weights);
    for (int i = 0; i < s.nVars(); i++)
      DAG<T>::weightsVar.push(wl[i << 1] + wl[(i << 1) | 1]);
    if (!initUnsat)
      cache->setInfoFormula(s.nVars(), cnf.size(),
                            occManager->getMaxSizeClause());
  } // DDnnfCompiler

  ~DDnnfCompiler() {
    if (pv)
      delete pv;
    delete cache;
    delete vs;
    delete bm;
    delete occManager;
  }

  /**
     Compile the CNF formula into a dDNNF structure.

     \return a DAG
  */
  rootNode<T> *compile() {
    DAG<T> *d = NULL;
    rootNode<T> *root = new rootNode<T>(s.nVars());
    vec<Var> freeVariable, setOfVar, priorityVar;
    DAG<T>::initSizeVector(s.nVars());
    vec<int> idxReason;

    if (initUnsat)
      root->assignRootNode(s.trail, new falseNode<T>(), false, s.nVars(),
                           freeVariable, idxReason);
    else {
      bool fromCache = false;
      onTheBranch bData;
      if (!s.solveWithAssumptions())
        d = globalFalseNode;
      else {
        for (int i = 0; i < s.nVars(); i++)
          setOfVar.push(i);
        d = compile_(setOfVar, priorityVar, lit_Undef, bData, fromCache,
                     idxReason);
      }

      assert(s.decisionLevel() == 0 && d);
      printFinalStatsCache();
      root->assignRootNode(bData.units, d, fromCache, s.nVars(), bData.free,
                           idxReason);
    }
    return root;
  } // compile
};

#endif
