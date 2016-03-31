
#include <stdexcept>
#include <list>
#include <vector>

#include <nlopt.hpp>

#include "NloptSolver.h"
#include "Optimization.h"

namespace Drake {
namespace {
Eigen::VectorXd MakeEigenVector(const std::vector<double>& x) {
  Eigen::VectorXd xvec(x.size());
  for (size_t i = 0; i < x.size(); i++) {
    xvec[i] = x[i];
  }
  return xvec;
}

TaylorVecXd MakeInputTaylorVec(const Eigen::VectorXd& xvec,
                               const VariableList& variable_list) {
  size_t var_count = 0;
  for (const DecisionVariableView& v : variable_list) {
    var_count += v.size();
  }

  auto tx = initializeAutoDiff(xvec);
  TaylorVecXd this_x(var_count);
  size_t index = 0;
  for (const DecisionVariableView& v : variable_list) {
      this_x.segment(index, v.size()) = tx.segment(v.index(), v.size());
      index += v.size();
    }
  return this_x;
}

double EvaluateCosts(const std::vector<double>& x,
                     std::vector<double>& grad,
                     void* f_data) {
  const OptimizationProblem* prog =
      reinterpret_cast<const OptimizationProblem*>(f_data);

  double cost = 0;
  Eigen::VectorXd xvec = MakeEigenVector(x);

  auto tx = initializeAutoDiff(xvec);
  TaylorVecXd ty(1);
  TaylorVecXd this_x(x.size());

  if (!grad.empty()) {
    grad.assign(grad.size(), 0);
  }

  for (auto const& binding : prog->getGenericObjectives()) {
    size_t index = 0;
    for (const DecisionVariableView& v : binding.getVariableList()) {
      this_x.segment(index, v.size()) = tx.segment(v.index(), v.size());
      index += v.size();
    }

    binding.getConstraint()->eval(this_x, ty);

    cost += ty(0).value();
    if (!grad.empty()) {
      for (const DecisionVariableView& v : binding.getVariableList()) {
        for (size_t j = v.index(); j < v.index() + v.size(); j++) {
          grad[j] += ty(0).derivatives()(j);
        }
      }
    }
  }

  return cost;
}

/// Structure to marshall data into the NLopt callback functions,
/// which take only a single pointer argument.
struct WrappedConstraint {
  const Constraint* constraint;
  const VariableList* variable_list;
};

double ApplyConstraintBounds(double result, double lb, double ub) {
  // Our constraints are expressed in the form lb <= f(x) <= ub.
  // NLopt always wants the value of a constraint expressed as
  // f(x) <= 0.
  //
  // For upper bounds rewrite as: f(x) - ub <= 0
  // For lower bounds rewrite as: -f(x) + lb <= 0

  if (ub != std::numeric_limits<double>::infinity()) {
    if ((lb != -std::numeric_limits<double>::infinity()) && (lb != ub)) {
      throw std::runtime_error(
          "Constraints with different upper and lower bounds not implemented.");
    }
    result -= ub;
  } else {
    if (lb == -std::numeric_limits<double>::infinity()) {
      throw std::runtime_error(
          "Unable to handle constraint with no bounds.");
    }
    result *= -1;
    result += lb;
  }
  return result;
}

double EvaluateScalarConstraint(const std::vector<double>& x,
                                std::vector<double>& grad,
                                void* f_data) {
  const WrappedConstraint* wrapped =
      reinterpret_cast<WrappedConstraint*>(f_data);

  Eigen::VectorXd xvec = MakeEigenVector(x);

  const Constraint* c = wrapped->constraint;
  const size_t num_constraints = c->getNumConstraints();
  assert(num_constraints == 1);

  TaylorVecXd ty(1);
  TaylorVecXd this_x = MakeInputTaylorVec(xvec, *(wrapped->variable_list));
  c->eval(this_x, ty);
  const double result = ApplyConstraintBounds(
      ty(0).value(), c->getLowerBound()(0), c->getUpperBound()(0));

  // TODO sam.creasey How should lower bounds be handled?  We don't
  // have any tests which use them...

  if (!grad.empty()) {
    grad.assign(grad.size(), 0);
    const double grad_sign =
        (c->getUpperBound()(0) ==
         std::numeric_limits<double>::infinity()) ? -1 : 1;
    for (const DecisionVariableView& v : *(wrapped->variable_list)) {
      for (size_t j = v.index(); j < v.index() + v.size(); j++) {
        grad[j] = ty(0).derivatives()(j) * grad_sign;
      }
    }
  }
  return result;
}

void EvaluateVectorConstraint(unsigned m, double* result, unsigned n,
                              const double* x, double* grad, void* f_data) {
  const WrappedConstraint* wrapped =
      reinterpret_cast<WrappedConstraint*>(f_data);

  Eigen::VectorXd xvec(n);
  for (size_t i = 0; i < n; i++) {
    xvec[i] = x[i];
    if (grad) { grad[i] = 0; }
  }

  const Constraint* c = wrapped->constraint;
  const size_t num_constraints = c->getNumConstraints();
  assert(num_constraints == m);

  TaylorVecXd ty(m);
  TaylorVecXd this_x = MakeInputTaylorVec(xvec, *(wrapped->variable_list));
  c->eval(this_x, ty);

  const Eigen::VectorXd& lower_bound = c->getLowerBound();
  const Eigen::VectorXd& upper_bound = c->getUpperBound();
  for (size_t i = 0; i < num_constraints; i++) {
    result[i] = ApplyConstraintBounds(
        ty(i).value(), lower_bound(i), upper_bound(i));
  }

  if (grad) {
    for (const DecisionVariableView& v : *(wrapped->variable_list)) {
      for (size_t i = 0; i < num_constraints; i++) {
        const double grad_sign =
            (c->getUpperBound()(i) ==
             std::numeric_limits<double>::infinity()) ? -1 : 1;
         for (size_t j = v.index(); j < v.index() + v.size(); j++) {
           grad[j] += ty(i).derivatives()(j) * grad_sign;
         }
      }
    }
  }
}
}

bool NloptSolver::available() const {
  return true;
}

bool NloptSolver::solve(OptimizationProblem &prog) const {

  int nx = prog.getNumVars();

  // Load the algo to use and the size.
  nlopt::opt opt(nlopt::LD_SLSQP, nx);

  const Eigen::VectorXd& initial_guess = prog.getInitialGuess();
  std::vector<double> x(initial_guess.size());
  for (size_t i = 0; i < x.size(); i++) {
    x[i] = initial_guess[i];
  }

  std::vector<double> xlow(nx, -std::numeric_limits<double>::infinity());
  std::vector<double> xupp(nx, std::numeric_limits<double>::infinity());

  for (auto const& binding : prog.getBoundingBoxConstraints()) {
    auto const& c = binding.getConstraint();
    const Eigen::VectorXd& lower_bound = c->getLowerBound();
    const Eigen::VectorXd& upper_bound = c->getUpperBound();
    for (const DecisionVariableView& v : binding.getVariableList()) {
      for (int k = 0; k < v.size(); k++) {
        const int idx = v.index() + k;
        xlow[idx] = std::max(lower_bound(k), xlow[idx]);
        xupp[idx] = std::min(upper_bound(k), xupp[idx]);
        if (x[idx] < xlow[idx]) { x[idx] = xlow[idx]; }
        if (x[idx] > xupp[idx]) { x[idx] = xupp[idx]; }
      }
    }
  }

  opt.set_lower_bounds(xlow);
  opt.set_upper_bounds(xupp);

  opt.set_min_objective(EvaluateCosts, &prog);

  // TODO sam.creasey All hardcoded tolerances in this function should
  // be made configurable when #1879 is fixed.
  const double constraint_tol = 1e-6;
  const double xtol_rel = 1e-6;

  std::list<WrappedConstraint> wrapped_list;

  // TODO sam.creasey Missing test coverage for generic constraints
  // with >1 output.
  for (auto& c : prog.getGenericConstraints()) {
    WrappedConstraint wrapped = { c.getConstraint().get(),
                                  &c.getVariableList() };
    wrapped_list.push_back(wrapped);
    std::vector<double> tol(c.getConstraint()->getNumConstraints(),
                            constraint_tol);
    opt.add_inequality_mconstraint(
        EvaluateVectorConstraint, &wrapped_list.back(), tol);
  }

  // sam.creasey: The initial implementation of this code used
  // add_equality_mconstraint to handle constraints with multiple
  // outputs as a vector constraint.  This did not seem to work.  The
  // version below breaks out the problem into multiple constraints.
  std::list<LinearEqualityConstraint> equalities;
  for (auto& c : prog.getLinearEqualityConstraints()) {
    const size_t num_constraints = c.getConstraint()->getNumConstraints();
    const auto& A = c.getConstraint()->getMatrix();
    const auto& b = c.getConstraint()->getLowerBound();
    for (size_t i = 0; i < num_constraints; i++) {
      equalities.push_back(LinearEqualityConstraint(A.row(i), b.row(i)));
      WrappedConstraint wrapped = { &equalities.back(),
                                    &c.getVariableList() };
      wrapped_list.push_back(wrapped);
      opt.add_equality_constraint(
          EvaluateScalarConstraint, &wrapped_list.back(), constraint_tol);
    }
  }

  // TODO sam.creasey Missing test coverage for linear constraints
  // with >1 output.
  for (auto& c : prog.getLinearConstraints()) {
    WrappedConstraint wrapped = { c.getConstraint().get(),
                                  &c.getVariableList() };
    wrapped_list.push_back(wrapped);
    std::vector<double> tol(c.getConstraint()->getNumConstraints(),
                            constraint_tol);
    opt.add_inequality_mconstraint(
        EvaluateVectorConstraint, &wrapped_list.back(), tol);
  }

  opt.set_xtol_rel(xtol_rel);

  double minf = 0;
  opt.optimize(x, minf);

  Eigen::VectorXd sol(x.size());
  for (int i = 0; i < nx; i++) {
    sol(i) = x[i];
  }

  prog.setDecisionVariableValues(sol);
  return true;
}
}
