/*!
 * 
 *
 * \brief       LLBFGS
 * 
 * The Limited-Memory Broyden, Fletcher, Goldfarb, Shannon (LBFGS) algorithm
 * is a quasi-step method for unconstrained real-valued optimization.
 * See: http://en.wikipedia.org/wiki/LLBFGS for details.
 * 
 * 
 *
 * \author      S. Dahlgaard, O.Krause
 * \date        2017
 *
 *
 * \par Copyright 1995-2017 Shark Development Team
 * 
 * <BR><HR>
 * This file is part of Shark.
 * <http://shark-ml.org/>
 * 
 * Shark is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published 
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Shark is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with Shark.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
 #define SHARK_COMPILE_DLL
#include <shark/Algorithms/GradientDescent/LBFGS.h>
#include <shark/ObjectiveFunctions/BoxConstraintHandler.h>

using namespace shark;

void LBFGS::initModel(){
	m_bdiag = 1.0;         // Start with the identity
	m_updThres = 1e-10;       // Reasonable threshold
	m_gradientDifferences.clear();
	m_steps.clear();
}
void LBFGS::computeSearchDirection(ObjectiveFunctionType const& function){
	// Update the history if necessary
	RealVector y = m_derivative - m_lastDerivative;
	RealVector s = m_best.point - m_lastPoint;
	updateHist(y, s);
	
	if (function.isConstrained()){
		SHARK_RUNTIME_CHECK(
			function.hasConstraintHandler()
			&& function.getConstraintHandler().isBoxConstrained(),
			"LBFGS does only allow box constraints via a constraint handler"
		);
		typedef BoxConstraintHandler<SearchPointType> Handler;
		Handler const& handler = static_cast<Handler const&>(function.getConstraintHandler());
		getBoxConstrainedDirection(m_searchDirection, handler.lower(), handler.upper());
		SHARK_RUNTIME_CHECK(function.isFeasible(m_best.point + m_searchDirection), "internal error");
	}else{
		noalias(m_searchDirection) = -m_derivative;
		multBInv(m_searchDirection);
	}
}

//from ISerializable
void LBFGS::read( InArchive & archive )
{
	AbstractLineSearchOptimizer::read(archive);
	archive>>m_numHist;
	archive>>m_bdiag;
	archive>>m_steps;
	archive>>m_gradientDifferences;
}

void LBFGS::write( OutArchive & archive ) const
{
	AbstractLineSearchOptimizer::write(archive);
	archive<<m_numHist;
	archive<<m_bdiag;
	archive<<m_steps;
	archive<<m_gradientDifferences;
}

void LBFGS::updateHist(RealVector& y, RealVector &step) {
	//Only update if <y,s> is above some reasonable threshold.
	double ys = inner_prod(y, step);
	if (ys > m_updThres) {
		// Only store m_numHist steps, so possibly pop the oldest.
		if (m_steps.size() >= m_numHist) {
			m_steps.pop_front();
			m_gradientDifferences.pop_front();
		}
		m_steps.push_back(step);
		m_gradientDifferences.push_back(y);
		// Update the hessian approximation.
		m_bdiag =  inner_prod(y,y) / ys;
	}
}

void LBFGS::getBoxConstrainedDirection(
	RealVector& searchDirection, 
	RealVector const& l, RealVector const& u
)const{
	RealVector const& x = m_best.point;
	//when a point is closer than eps to a inequality constraint we
	//consider the constraint as equality constraint.
	double eps = 1.e-13;
	
	//separate movable(active) and immovable(inactive) variables
	RealVector p0 = -m_derivative;//movable search direction
	std::vector<std::size_t> active;
	std::vector<std::size_t> inactive;
	for(std::size_t i = 0; i != l.size(); ++i){
		if((l(i) > x(i) - eps && p0(i) < 0 )
		|| (u(i) < x(i) + eps && p0(i) > 0)){
			p0(i) = 0.0;
			inactive.push_back(i);
		}else{
			active.push_back(i);
		}
	}
	
	//compute the normal proposition of step
	//under the constraint that the immovable variables are kept fixed
	RealVector step = p0;
	multBInv(step);
	for(std::size_t i: inactive){
		step(i) = 0.0;
	}
	
	//check whether the step is feasible
	bool feasible = true;
	for(std::size_t i: active){
		if( (l(i) > x(i) - eps + step(i)) 
		|| (u(i) < x(i) + eps + step(i))){
			feasible = false;
			break;
		}
	}
	//if it is feasible, we are done
	if(feasible){
		searchDirection = step;
		return;
	}
	
	//else we apply the dogleg step
	
	//compute cauchy point p= -p0 / p0^TBp0
	RealVector Bp0 = p0;
	multB(Bp0);
	RealVector cauchy= p0 / inner_prod(p0,Bp0);
	
	//check maximum step length along cauchy direction
	double alpha = 1.0;
	for(std::size_t i: active){
		if(cauchy(i) == 0) continue;
		double l_alpha = (l(i) - x(i))/cauchy(i);
		double u_alpha = (u(i) - x(i))/cauchy(i);
		if(l_alpha > 0)
			alpha = std::min( alpha, l_alpha);
		if(u_alpha > 0)
			alpha = std::min( alpha, u_alpha);
	}

	//if alpha < 1, the cauchy step is infeasible and we
	//simply return the furthest we can go along this direction
	if(alpha < 1){
		noalias(searchDirection) = alpha * cauchy;
		return;
	}
	
	//cauchy point is feasible, therefore we compute the dogleg direction
	RealVector point = x + cauchy;
	RealVector dir = step - cauchy;
	alpha = 1.0;
	for(std::size_t i: active){
		if(dir(i) == 0) continue;
		double l_alpha = (l(i) - point(i))/dir(i);
		double u_alpha = (u(i) - point(i))/dir(i);
		if(l_alpha > 0)
			alpha = std::min( alpha, l_alpha);
		if(u_alpha > 0)
			alpha = std::min( alpha, u_alpha);
	}
	searchDirection = cauchy + alpha * dir; 
}
void LBFGS::multBInv(RealVector& x)const{
	
	RealVector rho(m_numHist);
	RealVector alpha(m_numHist);

	for (std::size_t i = 0; i < m_steps.size(); ++i)
		rho(i) = 1.0 / inner_prod(m_gradientDifferences[i], m_steps[i]);

	for (std::size_t i = m_steps.size(); i > 0; --i) {
		alpha(i-1) = rho(i-1) * inner_prod(m_steps[i-1], x);
		noalias(x) -= alpha(i-1) * m_gradientDifferences[i-1];
	}
	x /= m_bdiag;
	for (std::size_t i = 0; i < m_steps.size(); ++i) {
		double beta = rho(i) * inner_prod(m_gradientDifferences[i], x);
		x += m_steps[i] * (alpha(i) - beta);
	}
}

void LBFGS::multB(RealVector& x)const{
	
	RealMatrix A(m_numHist, x.size(),0.0);
	RealVector beta(m_numHist);

	//compute the beta values (inverse rho of multBInv)
	
	RealVector result = m_bdiag * x;
	for (std::size_t i = 0; i < m_steps.size(); ++i){
		beta(i) = inner_prod(m_gradientDifferences[i], m_steps[i]);
		double yiTx = inner_prod(m_gradientDifferences[i], x);
		noalias(result) += yiTx/beta(i) * m_gradientDifferences[i];
		
		noalias(row(A,i)) = m_bdiag * m_steps[i];
		for (std::size_t j = 0; j < i; ++j){
			double yjTsi = inner_prod(m_gradientDifferences[j], m_steps[i]);
			noalias(row(A,i)) += yjTsi/beta(j) * m_gradientDifferences[j];
		}
		noalias(row(A,i)) -= trans(rows(A,0,i)) % rows(A,0,i) % m_steps[i];
		noalias(row(A,i)) /= std::sqrt(inner_prod(m_steps[i],row(A,i)));
	}
	noalias(result) -= trans(A) % A % x;
	x = result;
}