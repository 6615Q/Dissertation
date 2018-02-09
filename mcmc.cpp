/*
 *  mcmc.cpp
 *  Selection_Recombination
 *
 *  Created by Joshua Schraiber on 5/2/13.
 *  Copyright 2013 UC Berkeley. All rights reserved.
 *
 */

#include "mcmc.h"
#include "MbRandom.h"
#include "settings.h"
#include "path.h"
#include "measure.h"
#include "param.h"

#include<iomanip>
#include<fstream>

mcmc::mcmc(settings& mySettings, MbRandom* r) {
	random = r;
	printFreq = mySettings.get_printFreq();
	sampleFreq = mySettings.get_sampleFreq();
	num_gen = mySettings.get_num_gen();
	minUpdate = mySettings.get_grid();
	if (!mySettings.get_linked()) {
		no_linked_sites(mySettings);
	} else {
		
	}
}

void mcmc::no_linked_sites(settings& mySettings) {
	//open files
	std::string paramName = mySettings.get_baseName() + ".param";
	std::string trajName = mySettings.get_baseName() + ".traj";
	std::string timeName = mySettings.get_baseName() + ".time";
	paramFile.open(paramName.c_str());
	trajFile.open(trajName.c_str());
	timeFile.open(timeName.c_str());
	
	//initialize wfMeasure
	wfMeasure* curWF = new wfMeasure(random,0);
	wfMeasure* oldWF = NULL;
	curWF->set_num_test(mySettings.get_num_test());
		    
    //parse the settings
    popsize* myPop = mySettings.parse_popsize_file();
    std::vector<sample_time*> sample_time_vec = mySettings.parse_input_file(random);
    
    //initialize path
    curPath = new wfSamplePath(sample_time_vec, myPop, curWF, mySettings);

	
	//propose an allele age
	double firstAge;
	if (mySettings.get_infer_age()) {
		firstAge = curPath->get_time(0)-0.001;
		//this essentially assumes that there is no population size change between curPath->get_time(0)-0.001 and curPath->get_time(0)
		//hopefully this is a reasonable thing to think.
		path* firstPath = new path(curWF->fisher(mySettings.get_fOrigin()), curPath->get_traj(0), firstAge, curPath->get_time(0), curWF, mySettings);
		curPath->set_allele_age(firstAge, firstPath, 0);
	}
	
	param_gamma* alpha1 = new param_gamma(mySettings.get_a1start(),random);
	
	param_gamma* alpha2 = new param_gamma(mySettings.get_a2start(),random);
	
	param_path* curParamPath = new param_path(curPath,alpha1,alpha2,random,mySettings);
	
	//initialize the parameter vector
    pars.resize(0);
	pars.push_back(alpha1);
	pars.push_back(alpha2);
	if (!mySettings.get_infer_age()) {
		pars.push_back(new start_freq(curPath->get_traj(0),random,curParamPath));
	} else {
		pars.push_back(new param_age(firstAge, random, curParamPath, mySettings.get_dt(), mySettings.get_grid()));
	}
	pars.push_back(new end_freq(curPath->get_traj(curPath->get_length()-1), random, curParamPath));
    std::vector<int> time_idx(0);
    for (int i = 0; i < sample_time_vec.size()-1; i++) {
        if (sample_time_vec[i]->get_oldest() < sample_time_vec[i]->get_youngest()) {
            if (!mySettings.get_infer_age()) {
                std::cout << "ERROR: Cannot have uncertain times without inferring allele age. Will be fixed in the future" << std::endl;
                exit(1);
            }
            sample_time_vec[i]->set_path(curParamPath);
            pars.push_back(sample_time_vec[i]);
            time_idx.push_back(i);
        }
    }
	pars.push_back(curParamPath);
    
    //prepare output file
    prepareOutput(mySettings.get_infer_age(), time_idx);
    
	//initialize the proposal ratios
	//probably move this somewhere else
	std::vector<double> propChance(0);
	propChance.push_back(mySettings.get_a1prop()); //update alpha1 1
	propChance.push_back(mySettings.get_a2prop()); //update alpha2 1
	propChance.push_back(mySettings.get_ageprop()); //update start/age 2
	propChance.push_back(mySettings.get_endprop()); //update end 2
    for (int i = 0; i < time_idx.size(); i++) {
        propChance.push_back(mySettings.get_timeprop()); //update times 1
    }
	propChance.push_back(mySettings.get_pathprop()); //update path 5

	//store as a cdf
	double sum = 0;
	for (int i = 0; i < propChance.size(); i++) {
		sum += propChance[i];
	}
	double cumsum = 0;
	for (int i = 0; i < propChance.size(); i++) {
		cumsum += propChance[i]/sum;
		propChance[i] = cumsum;
	}
    
    
	
	//compute starting lnL
	curlnL = compute_lnL_sample_only(curPath);
	
	//run mcmc
	for (gen = 0; gen < num_gen; gen++) {
		
		std::string state;
		double propRatio = 0;
		double priorRatio = 0;
		double u = random->uniformRv();
		//propose a parameter change
		for (curProp = 0; curProp < propChance.size(); curProp++) {
			if (u < propChance[curProp]) {
				break;
			}
		}
                
		pars[curProp]->increaseProp();
		propRatio = pars[curProp]->propose();
		priorRatio = pars[curProp]->prior();
			
		oldWF = curWF;
		curWF = new wfMeasure(random,pars[0]->get());
		oldlnL = curlnL;
		curlnL = compute_lnL_sample_only(curPath);

		
		double LLRatio = curlnL-oldlnL;
        if (curlnL != curlnL || oldlnL != oldlnL) {
            std::cout << curlnL << " " << oldlnL << std::endl;
            exit(1);
        }
		if (curProp == 0 || curProp == 1) {
			//need to compute LL ratio due to the new alpha...
			cbpMeasure quickCBP(random);
			LLRatio += quickCBP.log_girsanov_wfwf_r(curPath, pars[0]->getOld(), pars[0]->get(), pars[1]->getOld(), pars[1]->get(), curPath->get_pop());
		}
		double mh = LLRatio+propRatio+priorRatio;
		u = random->uniformRv();
		
		if (gen % printFreq == 0) {
			std::cout << gen << " " << curProp;
			std::cout << std::setprecision(10) <<  " " << oldlnL << " -> " << curlnL << " " << LLRatio << " " << propRatio << " " << priorRatio << " " << mh << " " << log(u) << " ";
		}
		
		if (log(u) < mh) {
			//accept
			if (curProp < pars.size()-1) {
				pars[curProp]->increaseAccept();
			}
			curPath->set_update_begin(0);
			curPath->set_old_index(-1);
			delete oldWF;
			state = "Accept";
		} else {
			//reject
			if (curProp < pars.size()-1) {
				pars[curProp]->reset();
			}
			curPath->reset();
			curPath->set_update_begin(0);
			curPath->set_old_index(-1);
			
			delete curWF;
			curWF = oldWF;
			curlnL = oldlnL;
			state = "Reject";
		}
	
		
		if (gen % printFreq == 0) {
			std::cout << state << std::endl;
		}
		
		int tuningFreq = num_gen/1000;
		if (tuningFreq < 100) {
			tuningFreq = 100;
		}
		if (tuningFreq > 1000) {
			tuningFreq = 1000;
		}
		if (gen % tuningFreq == 0) {
			for (int i = 0; i < pars.size()-1; i++) {
				pars[i]->updateTuning();
			}
		}
				
        
		if (gen % sampleFreq == 0) {
            printState();
		}
	}
	
	

}

//for now, this computes the lnL of the WHOLE PATH (wrt Wiener measure) and SAMPLES
//could be optimized to only care about updated portions of path?
double mcmc::compute_lnL(wfSamplePath* p, measure* m, wienerMeasure* wm) {
	//compute dP/dW
	double gir = wm->log_girsanov(p, m, 0, PI);
	//compute sampling probs
	double sample_prob = 0;
	for (int i = 0; i < p->get_num_samples(); i++) {
		sample_prob += p->sampleProb(i);
	}
	
	if (gir != gir) {
		std::cout << "Likelihood is nan at generation " << gen << ". Proposal " << curProp << std::endl;
		p->print_traj(std::cout);
		p->print_time(std::cout);
		exit(1);
	}
	return gir + sample_prob;
}

double mcmc::compute_lnL_sample_only(wfSamplePath* p) {
	double sample_prob = 0;
	for (int i = 0; i < p->get_num_samples(); i++) {
		sample_prob += p->sampleProb(i);
	}
	
	return sample_prob;
}

void mcmc::prepareOutput(bool infer_age, std::vector<int> time_idx) {
    paramFile << "gen\tlnL\tpathlnL\talpha1\talpha2";
    if (infer_age) {
        paramFile << "\tage";
    } else {
        paramFile << "\tstart_freq";
    }
    paramFile << "\tend_freq";
    for (int i = 0; i < time_idx.size(); i++) {
        paramFile << "\tsample_time_" << time_idx[i];
    }
    paramFile << std::endl;
}

void mcmc::printState() {
    cbpMeasure testCBP(random);
    double pathlnL = testCBP.log_girsanov_wf_r(curPath, pars[0]->get(), pars[1]->get(), curPath->get_pop(), 0);
    paramFile << gen << "\t" << curlnL << "\t" << pathlnL;
    for (int i = 0; i < pars.size()-1; i++) {
        paramFile << "\t" << pars[i]->get();
    }
    paramFile << std::endl;
    trajFile << gen << " ";
    curPath->print_traj(trajFile << std::setprecision(20));
    timeFile << gen << " ";
    curPath->print_time(timeFile << std::setprecision(20));
}

