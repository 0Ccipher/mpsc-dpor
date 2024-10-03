/*
 * Omkar Tuppe
 */

#ifndef __MPSC_CALCULATOR_HPP__
#define __MPSC_CALCULATOR_HPP__

#include "Calculator.hpp"

class MPSCCalculator : public Calculator {

public:
	MPSCCalculator(ExecutionGraph &g) : Calculator(g) {}

	/* Overrided Calculator methods */

	/* Initialize necessary matrices */
	void initCalc() override;

	/* Performs a step of the LB calculation */
	Calculator::CalculationResult doCalc() override;

	/* The calculator is informed about the removal of some events */
	void removeAfter(const VectorClock &preds) override;

	std::unique_ptr<Calculator> clone(ExecutionGraph &g) const override {
		return std::make_unique<MPSCCalculator>(g);
	}

private:
	/* Returns a list with all accesses that are accessed at least twice */
	std::vector<SAddr> getDoubleLocs() const;

	std::vector<Event> calcSCFencesSuccs(const std::vector<Event> &fcs,
					     const Event e) const;
	std::vector<Event> calcSCFencesPreds(const std::vector<Event> &fcs,
					     const Event e) const;
	std::vector<Event> calcSCSuccs(const std::vector<Event> &fcs,
				       const Event e) const;
	std::vector<Event> calcSCPreds(const std::vector<Event> &fcs,
				       const Event e) const;
	std::vector<Event> calcRfSCSuccs(const std::vector<Event> &fcs,
					 const Event e) const;
	std::vector<Event> calcRfSCFencesSuccs(const std::vector<Event> &fcs,
					       const Event e) const;

	void addRbEdges(const std::vector<Event> &fcs,
			const std::vector<Event> &moAfter,
			const std::vector<Event> &moRfAfter,
			Calculator::GlobalRelation &matrix, const Event &e) const;
	void addMoRfEdges(const std::vector<Event> &fcs,
			  const std::vector<Event> &moAfter,
			  const std::vector<Event> &moRfAfter,
			  Calculator::GlobalRelation &matrix, const Event &e) const;
	void addSCEcosLoc(const std::vector<Event> &fcs,
			  Calculator::GlobalRelation &coMatrix,
			  Calculator::GlobalRelation &pscMatrix) const;

	void addSCEcos(const std::vector<Event> &fcs,
		       const std::vector<SAddr> &scLocs,
		       Calculator::GlobalRelation &matrix) const;

	void addInitEdges(const std::vector<Event> &fcs,
			  Calculator::GlobalRelation &matrix) const;
	void addSbHbEdges(Calculator::GlobalRelation &matrix) const;

	Calculator::CalculationResult addPscConstraints();
	void calcPscRelation();
};

#endif /* __MPSC_CALCULATOR_HPP__ */
