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
#ifndef UTILS_NEGATE_CNF_HH
#define UTILS_NEGATE_CNF_HH

#include "../mtl/Vec.hh"
#include "SolverTypes.hh"
#include <vector>

/**
 * Naive CNF negation utility for d4-dual-negation algorithm.
 * 
 * Negates a CNF formula using De Morgan's laws:
 *   ¬[(x₁ ∨ x₂ ∨ ...) ∧ (y₁ ∨ ...)] = (¬x₁ ∧ ¬x₂ ∧ ...) ∨ (¬y₁ ∧ ...)
 * 
 * This produces a DNF, which is then converted back to CNF by
 * enumerating all combinations (naive expansion for small formulas).
 */

/**
 * Negate a single literal
 */
inline Lit negateLit(Lit l)
{
  return ~l;
}

/**
 * Negate a CNF formula naively.
 * Input: CNF as vector of clauses (each clause is a disjunction)
 * Output: Negated formula as CNF
 * 
 * For small CNFs, we enumerate the DNF terms and convert to CNF.
 * Each DNF term becomes a set of unit clauses; the overall DNF
 * is converted by taking one literal from each original clause.
 * 
 * ¬(C₁ ∧ C₂ ∧ ... ∧ Cₙ) = ¬C₁ ∨ ¬C₂ ∨ ... ∨ ¬Cₙ
 * where ¬(l₁ ∨ l₂ ∨ ...) = (¬l₁ ∧ ¬l₂ ∧ ...)
 * 
 * So the negation is: (¬l₁₁ ∧ ¬l₁₂ ∧ ...) ∨ (¬l₂₁ ∧ ...) ∨ ...
 * 
 * To convert this DNF back to CNF, we use distributive law.
 * For small formulas, we enumerate all combinations.
 */
inline void negateCNF(vec<vec<Lit> > &inputCNF, vec<vec<Lit> > &outputCNF)
{
  outputCNF.clear();
  
  if(inputCNF.size() == 0)
  {
    // Negation of empty CNF (true) is false: add empty clause
    outputCNF.push();
    return;
  }
  
  // Special case: single clause
  // ¬(l₁ ∨ l₂ ∨ ... ∨ lₙ) = (¬l₁) ∧ (¬l₂) ∧ ... ∧ (¬lₙ)
  if(inputCNF.size() == 1)
  {
    for(int i = 0; i < inputCNF[0].size(); i++)
    {
      outputCNF.push();
      outputCNF.last().push(negateLit(inputCNF[0][i]));
    }
    return;
  }
  
  // General case: multiple clauses
  // ¬(C₁ ∧ C₂ ∧ ... ∧ Cₙ) = ¬C₁ ∨ ¬C₂ ∨ ... ∨ ¬Cₙ
  // 
  // Each ¬Cᵢ is a conjunction of negated literals.
  // The overall DNF has terms: pick one negated literal from each ¬Cᵢ
  // Wait, that's wrong. Let me reconsider:
  //
  // ¬(C₁ ∧ C₂) where C₁ = (a ∨ b) and C₂ = (c ∨ d)
  // = ¬C₁ ∨ ¬C₂
  // = (¬a ∧ ¬b) ∨ (¬c ∧ ¬d)
  //
  // This DNF needs to be converted to CNF.
  // DNF to CNF: (A₁ ∧ A₂) ∨ (B₁ ∧ B₂) = (A₁ ∨ B₁) ∧ (A₁ ∨ B₂) ∧ (A₂ ∨ B₁) ∧ (A₂ ∨ B₂)
  //
  // For n DNF terms, each being a conjunction, we form clauses by taking
  // one literal from each DNF term in all possible combinations.
  
  // First, create DNF terms: each term is ¬Cᵢ = conjunction of ¬literals
  std::vector<std::vector<Lit> > dnfTerms;
  for(int i = 0; i < inputCNF.size(); i++)
  {
    std::vector<Lit> term;
    for(int j = 0; j < inputCNF[i].size(); j++)
    {
      term.push_back(negateLit(inputCNF[i][j]));
    }
    dnfTerms.push_back(term);
  }
  
  // Convert DNF to CNF by distributing
  // Start with the first term's literals as individual "partial clauses"
  std::vector<std::vector<Lit> > partialClauses;
  for(size_t i = 0; i < dnfTerms[0].size(); i++)
  {
    std::vector<Lit> clause;
    clause.push_back(dnfTerms[0][i]);
    partialClauses.push_back(clause);
  }
  
  // For each subsequent DNF term, multiply out
  for(size_t t = 1; t < dnfTerms.size(); t++)
  {
    std::vector<std::vector<Lit> > newPartials;
    for(size_t p = 0; p < partialClauses.size(); p++)
    {
      for(size_t l = 0; l < dnfTerms[t].size(); l++)
      {
        Lit newLit = dnfTerms[t][l];
        // Check against existing clause BEFORE merging
        bool isTautology = false;
        bool isDuplicate = false;
        for(size_t k = 0; k < partialClauses[p].size(); k++) {
          if (partialClauses[p][k] == ~newLit) { isTautology = true; break; }
          if (partialClauses[p][k] == newLit)  { isDuplicate = true; break; }
        }
        if (isTautology) continue;  // skip tautological clause (contains l and ~l)
        std::vector<Lit> newClause = partialClauses[p];
        if (!isDuplicate) newClause.push_back(newLit);
        newPartials.push_back(newClause);
      }
    }
    partialClauses = newPartials;
  }
  
  // Convert to output format
  for(size_t i = 0; i < partialClauses.size(); i++)
  {
    outputCNF.push();
    for(size_t j = 0; j < partialClauses[i].size(); j++)
    {
      outputCNF.last().push(partialClauses[i][j]);
    }
  }
}

/**
 * Condition a CNF formula under a literal assignment.
 * Returns the simplified CNF after assigning the given literal to true.
 * 
 * @param inputCNF - the input CNF formula
 * @param l - the literal to assign to true
 * @param outputCNF - the conditioned CNF
 */
inline void conditionCNF(vec<vec<Lit> > &inputCNF, Lit l, vec<vec<Lit> > &outputCNF)
{
  outputCNF.clear();
  
  for(int i = 0; i < inputCNF.size(); i++)
  {
    bool clauseSatisfied = false;
    vec<Lit> newClause;
    
    for(int j = 0; j < inputCNF[i].size(); j++)
    {
      Lit lit = inputCNF[i][j];
      
      if(lit == l)
      {
        // Clause is satisfied
        clauseSatisfied = true;
        break;
      }
      else if(lit == ~l)
      {
        // This literal is false, skip it
        continue;
      }
      else
      {
        // Keep this literal
        newClause.push(lit);
      }
    }
    
    if(!clauseSatisfied)
    {
      if(newClause.size() == 0)
      {
        // Empty clause - formula is UNSAT under this assignment
        outputCNF.push(); // Empty clause
        return;
      }
      outputCNF.push();
      newClause.copyTo(outputCNF.last());
    }
  }
}

#endif
