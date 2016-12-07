// This file is part of ModelPreviewController.

// ModelPreviewController is free software: you can redistribute it and/or
// modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// ModelPreviewController is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with ModelPreviewController.  If not, see
// <http://www.gnu.org/licenses/>.

#include "PreviewController.h"

#include "Constraints.h"
#include "PreviewSystem.h"
#include "debugUtils.h"
#include <algorithm>
#include <exception>
#include <numeric>
#include <sstream>

namespace mpc {

MPCTypeFull::Constraints::Constraints()
    : nrConstr(0)
    , nrEqConstr(0)
    , nrIneqConstr(0)
    , nrBoundConstr(0)
    , wpConstr()
    , wpEqConstr()
    , wpIneqConstr()
    , wpBoundConstr()
{
}

void MPCTypeFull::Constraints::updateNr()
{
    nrConstr = 0;
    nrEqConstr = 0;
    nrIneqConstr = 0;
    nrBoundConstr = 0;

    auto countConstr = [this](auto& wpc, int& nreq) {
        for (auto& wp : wpc) {
            nrConstr += wp.first.lock()->nrConstr();
            nreq += wp.first.lock()->nrConstr();
        }
    };

    countConstr(wpEqConstr, nrEqConstr);
    countConstr(wpIneqConstr, nrIneqConstr);
    countConstr(wpBoundConstr, nrBoundConstr);
}

void MPCTypeFull::Constraints::clear()
{
    nrConstr = 0;
    nrEqConstr = 0;
    nrIneqConstr = 0;
    nrBoundConstr = 0;
    wpConstr.clear();
    wpEqConstr.clear();
    wpIneqConstr.clear();
    wpBoundConstr.clear();
}

/*************************************************************************************************
 *                                         MPCTypeFull                                           *
 *************************************************************************************************/

MPCTypeFull::MPCTypeFull(SolverFlag sFlag)
    : ps_()
    , sol_(solverFactory(sFlag))
    , constraints_()
    , Q_()
    , Aineq_()
    , Aeq_()
    , c_()
    , bineq_()
    , beq_()
    , lb_()
    , ub_()
    , Wx_()
    , Wu_()
    , solveTime_()
    , solveAndBuildTime_()
{
}

MPCTypeFull::MPCTypeFull(const std::shared_ptr<PreviewSystem>& ps, SolverFlag sFlag)
    : ps_(ps)
    , sol_(solverFactory(sFlag))
    , constraints_()
    , Q_(ps_->fullUDim, ps_->fullUDim)
    , Aineq_()
    , Aeq_()
    , c_(ps_->fullUDim)
    , bineq_()
    , beq_()
    , lb_(ps_->fullUDim)
    , ub_(ps_->fullUDim)
    , Wx_(ps_->fullXDim)
    , Wu_(ps_->fullUDim)
    , solveTime_()
    , solveAndBuildTime_()
{
    Wx_.setOnes();
    Wu_.setOnes();
}

void MPCTypeFull::selectQPSolver(SolverFlag flag)
{
    sol_ = solverFactory(flag);
}

void MPCTypeFull::initializeController(const std::shared_ptr<PreviewSystem>& ps)
{
    ps_ = ps;
    Q_.resize(ps_->fullUDim, ps_->fullUDim);
    Aineq_.resize(0, ps_->fullUDim);
    Aeq_.resize(0, ps_->fullUDim);
    c_.resize(ps_->fullUDim);
    bineq_.resize(0);
    beq_.resize(0);
    lb_.resize(ps_->fullUDim);
    lb_.setConstant(-std::numeric_limits<double>::max());
    ub_.resize(ps_->fullUDim);
    ub_.setConstant(std::numeric_limits<double>::max());
    Wx_.resize(ps_->fullXDim);
    Wu_.resize(ps_->fullUDim);
    Wx_.setOnes();
    Wu_.setOnes();
}

bool MPCTypeFull::solve()
{
    solveAndBuildTime_.start();
    checkConstraints();
    updateSystem();
    makeQPForm();
    sol_->SI_problem(ps_->fullUDim, constraints_.nrEqConstr, constraints_.nrIneqConstr);
    solveTime_.start();
    bool success = sol_->SI_solve(Q_, c_, Aeq_, beq_, Aineq_, bineq_, lb_, ub_);
    solveTime_.stop();
    solveAndBuildTime_.stop();
    if (!success)
        sol_->SI_inform();

    return success;
}

const Eigen::VectorXd& MPCTypeFull::control() const noexcept
{
    return sol_->SI_result();
}

Eigen::VectorXd MPCTypeFull::trajectory() const noexcept
{
    return ps_->Phi * ps_->x0 + ps_->Psi * control() + ps_->xi;
}

boost::timer::cpu_times MPCTypeFull::solveTime() const noexcept
{
    return solveTime_.elapsed();
}

boost::timer::cpu_times MPCTypeFull::solveAndBuildTime() const noexcept
{
    return solveAndBuildTime_.elapsed();
}

void MPCTypeFull::weights(const Eigen::VectorXd& Wx, const Eigen::VectorXd& Wu)
{
    assert(Wx.rows() == ps_->xDim);
    assert(Wu.rows() == ps_->uDim);

    for (auto i = 0; i < ps_->nrStep; ++i) {
        Wx_.segment(i * ps_->xDim, ps_->xDim) = Wx;
        Wu_.segment(i * ps_->uDim, ps_->uDim) = Wu;
    }
}

void MPCTypeFull::addConstraint(const std::shared_ptr<Constraint>& constr)
{
    constr->initializeConstraint(*ps_);
    addConstraintByType(constr);

    Aeq_.resize(constraints_.nrEqConstr, ps_->fullUDim);
    beq_.resize(constraints_.nrEqConstr);
    Aineq_.resize(constraints_.nrIneqConstr, ps_->fullUDim);
    bineq_.resize(constraints_.nrIneqConstr);
    lb_.resize(constraints_.nrBoundConstr);
    ub_.resize(constraints_.nrBoundConstr);
}

void MPCTypeFull::resetConstraints() noexcept
{
    constraints_.clear();
}

/*
 *  Protected methods
 */

void MPCTypeFull::addConstraintByType(const std::shared_ptr<Constraint>& constr)
{
    constraints_.nrConstr += constr->nrConstr();
    constraints_.wpConstr.emplace_back(constr, constr->name());
    switch (constr->constraintType()) {
    case ConstraintFlag::EqualityConstraint: {
        constraints_.nrEqConstr += constr->nrConstr();
        // DownCasting to std::shared_ptr<EqIneqConstraint>
        // This is a safe operation since we know that the object is a derived class of a EqIneqConstraint
        constraints_.wpEqConstr.emplace_back(std::static_pointer_cast<EqIneqConstraint>(constr), constr->name());
    } break;
    case ConstraintFlag::InequalityConstraint: {
        constraints_.nrIneqConstr += constr->nrConstr();
        // DownCasting to std::shared_ptr<EqIneqConstraint>
        // This is a safe operation since we know that the object is a derived class of a EqIneqConstraint
        constraints_.wpIneqConstr.emplace_back(std::static_pointer_cast<EqIneqConstraint>(constr), constr->name());
    } break;
    case ConstraintFlag::BoundConstraint: {
        constraints_.nrBoundConstr += constr->nrConstr();
        // DownCasting to std::shared_ptr<ControlBoundConstraint>
        // This is a safe operation since we know that the object is a ControlBoundConstraint
        constraints_.wpBoundConstr.emplace_back(std::static_pointer_cast<ControlBoundConstraint>(constr), constr->name());
    } break;
    }
}

void MPCTypeFull::updateSystem()
{
    auto xDim = ps_->xDim;
    auto uDim = ps_->uDim;

    ps_->Phi.block(0, 0, xDim, xDim) = ps_->A;
    ps_->Psi.block(0, 0, xDim, uDim) = ps_->B;
    ps_->xi.segment(0, xDim) = ps_->d;

    for (auto i = 1; i < ps_->nrStep; ++i) {
        ps_->Phi.block(i * xDim, 0, xDim, xDim).noalias() = ps_->A * ps_->Phi.block((i - 1) * xDim, 0, xDim, xDim);
        for (auto j = 0; j < i; ++j)
            ps_->Psi.block(i * xDim, j * uDim, xDim, uDim).noalias() = ps_->A * ps_->Psi.block((i - 1) * xDim, j * uDim, xDim, uDim);
        ps_->Psi.block(i * xDim, i * uDim, xDim, uDim).noalias() = ps_->B;
        ps_->xi.segment(i * xDim, xDim).noalias() = ps_->A * ps_->xi.segment((i - 1) * xDim, xDim) + ps_->d;
    }

    for (auto& wpc : constraints_.wpConstr)
        wpc.first.lock()->update(*ps_);
}

void MPCTypeFull::makeQPForm()
{
    Q_.noalias() = ps_->Psi.transpose() * Eigen::MatrixXd(Wx_.asDiagonal()) * ps_->Psi + Eigen::MatrixXd(Wu_.asDiagonal());
    c_.noalias() = ps_->Psi.transpose() * Eigen::MatrixXd(Wx_.asDiagonal()) * (ps_->Phi * ps_->x0 - ps_->xd + ps_->xi);

    int nrLines = 0;
    // Get Equality constraints
    for (auto& wpc : constraints_.wpEqConstr) {
        auto cstr = wpc.first.lock();
        Aeq_.block(nrLines, 0, cstr->nrConstr(), ps_->fullUDim) = cstr->A();
        beq_.segment(nrLines, cstr->nrConstr()) = cstr->b();
        nrLines += cstr->nrConstr();
    }

    // Get Inequality constraints
    nrLines = 0;
    for (auto& wpc : constraints_.wpIneqConstr) {
        auto cstr = wpc.first.lock();
        Aineq_.block(nrLines, 0, cstr->nrConstr(), ps_->fullUDim) = cstr->A();
        bineq_.segment(nrLines, cstr->nrConstr()) = cstr->b();
        nrLines += cstr->nrConstr();
    }

    // Get Bound constraints
    nrLines = 0;
    for (auto& wpc : constraints_.wpBoundConstr) {
        auto cstr = wpc.first.lock();
        lb_.segment(nrLines, cstr->nrConstr()) = cstr->lower();
        ub_.segment(nrLines, cstr->nrConstr()) = cstr->upper();
        nrLines += cstr->nrConstr();
    }
}

void MPCTypeFull::checkConstraints()
{
    bool needResizing = false;

    auto checkConstr = [&needResizing](auto wpc, bool useWarn = false) {
        for (auto itr = wpc.begin(); itr != wpc.end();) {
            if ((*itr).first.expired()) {
                (void)useWarn; // Just to make the compiler understand it is used.
                CONSTRAINT_DELETION_WARN(useWarn, "%s%s%s", "Dangling pointer to constraint.\nA '", (*itr).second.c_str(),
                    "' has been destroyed.\nThe constraint has been removed from the controller");
                itr = wpc.erase(itr);
                needResizing = true;
            } else {
                ++itr;
            }
        }
    };

    checkConstr(constraints_.wpConstr, true);
    checkConstr(constraints_.wpEqConstr);
    checkConstr(constraints_.wpIneqConstr);
    checkConstr(constraints_.wpBoundConstr);

    if (needResizing) {
        constraints_.updateNr();
        Aineq_.resize(constraints_.nrIneqConstr, ps_->fullUDim);
        bineq_.resize(constraints_.nrIneqConstr);
        Aeq_.resize(constraints_.nrEqConstr, ps_->fullUDim);
        beq_.resize(constraints_.nrEqConstr);
        lb_.resize(constraints_.nrBoundConstr);
        ub_.resize(constraints_.nrBoundConstr);
    }
}

/*************************************************************************************************
 *                                         MPCTypeLast                                           *
 *************************************************************************************************/

MPCTypeLast::MPCTypeLast(SolverFlag sFlag)
    : MPCTypeFull::MPCTypeFull(sFlag)
{
}

MPCTypeLast::MPCTypeLast(const std::shared_ptr<PreviewSystem>& ps, SolverFlag sFlag)
    : MPCTypeFull::MPCTypeFull(ps, sFlag)
{
    Wx_.resize(ps_->xDim);
    Wx_.setOnes();
}

void MPCTypeLast::initializeController(const std::shared_ptr<PreviewSystem>& ps)
{
    ps_ = ps;
    Q_.resize(ps_->fullUDim, ps_->fullUDim);
    Aineq_.resize(0, ps_->fullUDim);
    Aeq_.resize(0, ps_->fullUDim);
    c_.resize(ps_->fullUDim);
    bineq_.resize(0);
    beq_.resize(0);
    lb_.resize(ps_->fullUDim);
    lb_.setConstant(-std::numeric_limits<double>::max())
        ub_.resize(ps_->fullUDim);
    ub_.setConstant(std::numeric_limits<double>::max())
        Wx_.resize(ps_->xDim);
    Wu_.resize(ps_->fullUDim);
    Wx_.setOnes();
    Wu_.setOnes();
}

void MPCTypeLast::weights(const Eigen::VectorXd& Wx, const Eigen::VectorXd& Wu)
{
    assert(Wx.rows() == ps_->xDim);
    assert(Wu.rows() == ps_->uDim);

    Wx_ = Wx;
    for (auto i = 0; i < ps_->nrStep; ++i)
        Wu_.segment(i * ps_->uDim, ps_->uDim) = Wu;
}

/*
 *  Protected methods
 */

void MPCTypeLast::makeQPForm()
{
    auto xDim = ps_->xDim;
    const Eigen::MatrixXd& psi = ps_->Psi.bottomRows(xDim);
    Q_.noalias() = psi.transpose() * Eigen::MatrixXd(Wx_.asDiagonal()) * psi + Eigen::MatrixXd(Wu_.asDiagonal());
    c_.noalias() = psi.transpose() * Eigen::MatrixXd(Wx_.asDiagonal()) * (ps_->Phi.bottomRows(xDim) * ps_->x0 - ps_->xd.tail(xDim) + ps_->xi.tail(xDim));

    int nrLines = 0;
    // Get Equality constraints
    for (auto& wpc : constraints_.wpEqConstr) {
        auto cstr = wpc.first.lock();
        Aeq_.block(nrLines, 0, cstr->nrConstr(), ps_->fullUDim) = cstr->A();
        beq_.segment(nrLines, cstr->nrConstr()) = cstr->b();
        nrLines += cstr->nrConstr();
    }

    // Get Inequality constraints
    nrLines = 0;
    for (auto& wpc : constraints_.wpIneqConstr) {
        auto cstr = wpc.first.lock();
        Aineq_.block(nrLines, 0, cstr->nrConstr(), ps_->fullUDim) = cstr->A();
        bineq_.segment(nrLines, cstr->nrConstr()) = cstr->b();
        nrLines += cstr->nrConstr();
    }

    // Get Bound constraints
    nrLines = 0;
    for (auto& wpc : constraints_.wpBoundConstr) {
        auto cstr = wpc.first.lock();
        lb_.segment(nrLines, cstr->nrConstr()) = cstr->lower();
        ub_.segment(nrLines, cstr->nrConstr()) = cstr->upper();
        nrLines += cstr->nrConstr();
    }
}

} // namespace pc