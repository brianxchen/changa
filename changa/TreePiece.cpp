/** @file TreePiece.cpp
 */

#include <cstdio>
#include <algorithm>
#include <fstream>
#include <assert.h>
// jetley
#include "limits.h"

//#include "ComlibManager.h"

#include "ParallelGravity.h"
#include "DataManager.h"
#include "Reductions.h"
// jetley
#include "MetisCosmoLB.h"
// jetley - refactoring
//#include "codes.h"
#include "Opt.h"
#include "Compute.h"
#include "TreeWalk.h"
//#include "State.h"

#include "Space.h"
#include "gravity.h"

#ifdef CELL
#include "spert_ppu.h"
#include "cell_typedef.h"
#endif

using namespace std;
using namespace SFC;
using namespace TreeStuff;
using namespace TypeHandling;

int TreeStuff::maxBucketSize;

#ifdef CELL
int workRequestOut = 0;
CkVec<CellComputation> ewaldMessages;
#endif

//forward declaration
string getColor(GenericTreeNode*);

/*
 * set periodic information in all the TreePieces
 */
void TreePiece::setPeriodic(int nRepsPar, // Number of replicas in
					  // each direction
			    double fPeriodPar, // Size of periodic box
			    int bEwaldPar,     // Use Ewald summation
			    double fEwCutPar,  // Cutoff on real summation
			    double dEwhCutPar, // Cutoff on Fourier summation
			    int bPeriodPar     // Periodic boundaries
			    ) 
{
    nReplicas = nRepsPar;
    fPeriod = Vector3D<double>(fPeriodPar, fPeriodPar, fPeriodPar);
    bEwald = bEwaldPar;
    fEwCut  = fEwCutPar;
    dEwhCut = dEwhCutPar;
    bPeriodic = bPeriodPar;
    if(ewt == NULL) {
	ewt = new EWT[nMaxEwhLoop];
    }
}

// Scale velocities (needed to convert to canonical momenta for
// comoving coordinates.
void TreePiece::velScale(double dScale) 
{
    for(unsigned int i = 0; i < myNumParticles; ++i)
	myParticles[i+1].velocity *= dScale;
    }

/// After the bounding box has been found, we can assign keys to the particles
void TreePiece::assignKeys(CkReductionMsg* m) {
	if(m->getSize() != sizeof(OrientedBox<float>)) {
		ckerr << thisIndex << ": TreePiece: Fatal: Wrong size reduction message received!" << endl;
		CkAssert(0);
		callback.send(0);
		delete m;
		return;
	}
	
	boundingBox = *static_cast<OrientedBox<float> *>(m->getData());
	delete m;
	if(thisIndex == 0 && verbosity)
		ckout << "TreePiece: Bounding box originally: "
		     << boundingBox << endl;
	//give particles keys, using bounding box to scale
	if(domainDecomposition!=ORB_dec){
	      // get longest axis
	      Vector3D<float> bsize = boundingBox.size();
	      float max = (bsize.x > bsize.y) ? bsize.x : bsize.y;
	      max = (max > bsize.z) ? max : bsize.z;
	      //
	      // Make the bounding box cubical.
	      //
	      Vector3D<float> bcenter = boundingBox.center();
	      bsize = Vector3D<float>(0.5*max);
	      boundingBox = OrientedBox<float>(bcenter-bsize, bcenter+bsize);
	      if(thisIndex == 0 && verbosity)
		      ckout << "TreePiece: Bounding box now: "
			   << boundingBox << endl;

	      for(unsigned int i = 0; i < myNumParticles; ++i) {
		myParticles[i+1].key = generateKey(myParticles[i+1].position,
						   boundingBox);
	      }
	      sort(&myParticles[1], &myParticles[myNumParticles+1]);
	}
  
#if COSMO_DEBUG > 1
  char fout[100];
  sprintf(fout,"tree.%d.%d.before",thisIndex,iterationNo);
  ofstream ofs(fout);
  for (int i=1; i<=myNumParticles; ++i)
    ofs << keyBits(myParticles[i].key,63) << " " << myParticles[i].position[0] << " "
        << myParticles[i].position[1] << " " << myParticles[i].position[2] << endl;
  ofs.close();
#endif

	contribute(0, 0, CkReduction::concat, callback);
	
	if(verbosity >= 5)
		cout << thisIndex << ": TreePiece: Assigned keys to all my particles" << endl;
}

/**************ORB Decomposition***************/

/*void TreePiece::getBoundingBox(OrientedBox<float>& box){
  box = boundingBox;
}*/

/// Three comparison routines used in sort and upper_bound
/// to order particles in each of three dimensions, respectively
bool comp_dim0(GravityParticle p1, GravityParticle p2) {
    return p1.position[0] < p2.position[0];
}
bool comp_dim1(GravityParticle p1, GravityParticle p2) {
    return p1.position[1] < p2.position[1];
}
bool comp_dim2(GravityParticle p1, GravityParticle p2) {
    return p1.position[2] < p2.position[2];
}

///Initialize stuff before doing ORB decomposition
void TreePiece::initORBPieces(const CkCallback& cb){

  OrientedBox<float> box = boundingBox;
  orbBoundaries.clear();
  orbBoundaries.push_back(myParticles+1);
  orbBoundaries.push_back(myParticles+myNumParticles+1);
  firstTime=true;

  phase=0;

  //Initialize function pointers
  compFuncPtr[0]= &comp_dim0;
  compFuncPtr[1]= &comp_dim1;
  compFuncPtr[2]= &comp_dim2;

	myBinCountsORB.clear();
	myBinCountsORB.push_back(myNumParticles);

  //Find out how many levels will be there before we go into the tree owned
  //completely by the TreePiece
  chunkRootLevel=0;
  unsigned int tmp = numTreePieces;
  while(tmp){
    tmp >>= 1;
    chunkRootLevel++;
  }
  chunkRootLevel--;
  
  boxes = new OrientedBox<float>[chunkRootLevel+1];
  splitDims = new char[chunkRootLevel+1];

  boxes[0] = boundingBox;
  
  contribute(sizeof(OrientedBox<float>), &box, boxReduction, cb);
}

/*class Compare{ //Defines the comparison operator on the map used in balancer
  int dim;
public:
  Compare() {}
  Compare(int i) : dim(i) {}
  
  void setDim(int i){ dim = i; }
  
  bool operator()(GravityParticle& p1, GravityParticle& p2) const {
    return p1.position[dim] < p2.position[dim];
  }
};*/

void TreePiece::initBeforeORBSend(unsigned int myCount, const CkCallback& cb, const CkCallback& cback){

  callback = cb;
  //sorterCallBack = sorterCb;
  CkCallback nextCallback = cback;
  myExpectedCount = myCount;
  
  mySortedParticles.clear();
  mySortedParticles.reserve(myExpectedCount);
  
  /*if(myExpectedCount > myNumParticles){
    delete [] myParticles;
    myParticles = new GravityParticle[myExpectedCount + 2];
  }
  myNumParticles = myExpectedCount;*/

  contribute(0, 0, CkReduction::concat, nextCallback);
}

//void TreePiece::sendORBParticles(unsigned int myCount, const CkCallback& cb, const CkCallback& sorterCb){
void TreePiece::sendORBParticles(){

  /*callback = cb;
  sorterCallBack = sorterCb;
  myExpectedCount = myCount;

  std::list<GravityParticle *>::iterator iter;
  std::list<GravityParticle *>::iterator iter2;

  mySortedParticles.clear();
  mySortedParticles.reserve(myExpectedCount);*/
  
  std::list<GravityParticle *>::iterator iter;
  std::list<GravityParticle *>::iterator iter2;
  
  int i=0;
  for(iter=orbBoundaries.begin();iter!=orbBoundaries.end();iter++,i++){
		iter2=iter;
		iter2++;
		if(iter2==orbBoundaries.end())
			break;
    if(i==thisIndex){
			//CkPrintf("[%d] send %d particles to %d\n",thisIndex,myBinCounts[i],i);
      if(myBinCountsORB[i]>0)
        acceptORBParticles(*iter,myBinCountsORB[i]);
		}
    else{
			//CkPrintf("[%d] send %d particles to %d\n",thisIndex,myBinCounts[i],i);
      if(myBinCountsORB[i]>0)
        pieces[i].acceptORBParticles(*iter,myBinCountsORB[i]);
		}
  }

  if(myExpectedCount > (int) myNumParticles){
    delete [] myParticles;
    myParticles = new GravityParticle[myExpectedCount + 2];
  }
  myNumParticles = myExpectedCount;
}

/// Accept particles from other TreePieces once the sorting has finished
void TreePiece::acceptORBParticles(const GravityParticle* particles, const int n) {
     
  copy(particles, particles + n, back_inserter(mySortedParticles));
  
  //CkPrintf("[%d] accepted %d particles:myexpected:%d,got:%d\n",thisIndex,n,myExpectedCount,mySortedParticles.size());
  if(myExpectedCount == mySortedParticles.size()) {
	  //I've got all my particles
    //Assigning keys to particles
    //Key k = 1 << 63;
    //Key k = thisIndex;
    for(int i=0;i<myExpectedCount;i++){
      mySortedParticles[i].key = thisIndex;
    }
	  //sort(mySortedParticles.begin(), mySortedParticles.end());
	  copy(mySortedParticles.begin(), mySortedParticles.end(), &myParticles[1]);
	  //signify completion with a reduction
	  if(verbosity>1)
      ckout << thisIndex <<" contributing to accept particles"<<endl;
	  if (root != NULL) {
	    root->fullyDelete();
	    delete root;
	    root = NULL;
      nodeLookupTable.clear();
	  }
	  contribute(0, 0, CkReduction::concat, callback);
  }
}

void TreePiece::finalizeBoundaries(ORBSplittersMsg *splittersMsg){
 
  CkCallback& cback = splittersMsg->cb;
  
  std::list<GravityParticle *>::iterator iter;
  std::list<GravityParticle *>::iterator iter2;

  iter = orbBoundaries.begin();
  iter2 = orbBoundaries.begin();
  iter2++;

  phase++;

  int index = thisIndex >> (chunkRootLevel-phase+1);

  Key lastBit;
  lastBit = thisIndex >> (chunkRootLevel-phase);
  lastBit = lastBit & 0x1;
  
  boxes[phase] = boxes[phase-1];
  if(lastBit){
    boxes[phase].lesser_corner[splittersMsg->dim[index]] = splittersMsg->pos[index];
  }
  else{
    boxes[phase].greater_corner[splittersMsg->dim[index]] = splittersMsg->pos[index];
  }

  splitDims[phase-1]=splittersMsg->dim[index];
  
  for(int i=0;i<splittersMsg->length;i++){
    
    int dimen=(int)splittersMsg->dim[i];
	  //Current location of the division is stored in a variable
    //Evaluate the number of particles in each division

    GravityParticle dummy;
    GravityParticle* divStart = *iter;
    Vector3D<double> divide(0.0,0.0,0.0);
    divide[dimen] = splittersMsg->pos[i];
    dummy.position = divide;
    GravityParticle* divEnd = upper_bound(*iter,*iter2,dummy,compFuncPtr[dimen]);
    
    orbBoundaries.insert(iter2,divEnd);
    iter = iter2;
    iter2++;
	}

  firstTime = true;
  
  myBinCountsORB.assign(2*splittersMsg->length,0);
  copy(tempBinCounts.begin(),tempBinCounts.end(),myBinCountsORB.begin());

  contribute(0,0,CkReduction::concat,cback);

}

void TreePiece::evaluateParticleCounts(ORBSplittersMsg *splittersMsg){

  //myBinCounts.assign(2*splittersMsg->length,0);
  
  //if(firstTime){
    //myBinCounts.assign(splittersMsg->length,0);
    //copy(tempBinCounts.begin(),tempBinCounts.end(),myBinCounts.begin());
  //}
  CkCallback& cback = splittersMsg->cb;
  
  tempBinCounts.assign(2*splittersMsg->length,0);

  std::list<GravityParticle *>::iterator iter;
  std::list<GravityParticle *>::iterator iter2;

  iter = orbBoundaries.begin();
  iter2 = orbBoundaries.begin();
  iter2++;
  
  for(int i=0;i<splittersMsg->length;i++){
    
    int dimen = (int)splittersMsg->dim[i];
    if(firstTime){
      sort(*iter,*iter2,compFuncPtr[dimen]);
    }
    //curDivision = pos;
    /*if(firstTime){
      curDivision = pos;
      phaseLeader = leader;
      Compare comp(dim);
      sort(myParticles+1, myParticles+myNumParticles+1,comp);
    }*/
	  //Current location of the division is stored in a variable
    //Evaluate the number of particles in each division

		GravityParticle dummy;
    GravityParticle* divStart = *iter;
    Vector3D<double> divide(0.0,0.0,0.0);
    divide[dimen] = splittersMsg->pos[i];
    dummy.position = divide;
    GravityParticle* divEnd = upper_bound(*iter,*iter2,dummy,compFuncPtr[dimen]);
    tempBinCounts[2*i] = divEnd - divStart;
    tempBinCounts[2*i + 1] = myBinCountsORB[i] - (divEnd - divStart);

    iter++; iter2++;
	}
  
  if(firstTime)
    firstTime=false;
  //thisProxy[phaseLeader].collectORBCounts(firstCnt,secondCnt);
  contribute(2*splittersMsg->length*sizeof(int), &(*tempBinCounts.begin()), CkReduction::sum_int, cback);
}
/**********************************************/

/*
void TreePiece::registerWithDataManager(const CkGroupID& dataManagerID, const CkCallback& cb) {
	dataManager = CProxy_DataManager(dataManagerID);
	dm = dataManager.ckLocalBranch();
	if(dm == 0) {
		cerr << thisIndex << ": TreePiece: Fatal: Couldn't register with my DataManger" << endl;
		cb.send(0);
		return;
	}
	
	dm->myTreePieces.push_back(thisIndex);

	contribute(0, 0, CkReduction::concat, cb);
}
*/

/// Determine my part of the sorting histograms by counting the number
/// of my particles in each bin
void TreePiece::evaluateBoundaries(SFC::Key* keys, const int n, int isRefine, const CkCallback& cb) {
#ifdef COSMO_EVENT
  double startTimer = CmiWallTimer();
#endif

  int numBins = isRefine ? n / 2 : n - 1;
  //this array will contain the number of particles I own in each bin
  //myBinCounts.assign(numBins, 0);
  myBinCounts.resize(numBins);
  int *myCounts = myBinCounts.getVec();
  memset(myCounts, 0, numBins*sizeof(int));
  Key* endKeys = keys+n;
  GravityParticle *binBegin = &myParticles[1];
  GravityParticle *binEnd;
  GravityParticle dummy;
  //int binIter = 0;
  //vector<int>::iterator binIter = myBinCounts.begin();
  //vector<Key>::iterator keyIter = dm->boundaryKeys.begin();
  Key* keyIter = lower_bound(keys, keys+n, binBegin->key);
  int binIter = isRefine ? (keyIter - keys) / 2: keyIter - keys - 1;
      int change = 1;
      if (isRefine && !((keyIter - keys) & 1)) change = 0;
      for( ; keyIter != endKeys; ++keyIter) {
        dummy.key = *keyIter;
        /// find the last place I could put this splitter key in
        /// my array of particles
        binEnd = upper_bound(binBegin, &myParticles[myNumParticles+1],
            dummy);
        /// this tells me the number of particles between the
        /// last two splitter keys
        if (change) {
          myCounts[binIter] = (binEnd - binBegin);
          ++binIter;
        }
        if(&myParticles[myNumParticles+1] <= binEnd)
          break;
        binBegin = binEnd;
        if (isRefine) change ^= 1;
      }

#ifdef COSMO_EVENTS
      traceUserBracketEvent(boundaryEvaluationUE, startTimer, CmiWallTimer());
#endif
      //send my bin counts back in a reduction
      contribute(numBins * sizeof(int), myCounts, CkReduction::sum_int, cb);
}

/// Once final splitter keys have been decided, I need to give my
/// particles out to the TreePiece responsible for them

void TreePiece::unshuffleParticles(CkReductionMsg* m) {
	callback = *static_cast<CkCallback *>(m->getData());
	delete m;

	if (dm == NULL) {
	  dm = (DataManager*)CkLocalNodeBranch(dataManagerID);
	}

	//find my responsibility
	myPlace = find(dm->responsibleIndex.begin(), dm->responsibleIndex.end(), thisIndex) - dm->responsibleIndex.begin();
	//assign my bounding keys
	leftSplitter = dm->boundaryKeys[myPlace];
	rightSplitter = dm->boundaryKeys[myPlace + 1];
	
	vector<Key>::iterator iter = dm->boundaryKeys.begin();
	vector<Key>::const_iterator endKeys = dm->boundaryKeys.end();
	vector<int>::iterator responsibleIter = dm->responsibleIndex.begin();
	GravityParticle *binBegin = &myParticles[1];
	GravityParticle *binEnd;
	GravityParticle dummy;
	for(++iter; iter != endKeys; ++iter, ++responsibleIter) {
		dummy.key = *iter;
		//find particles between this and the last key
		binEnd = upper_bound(binBegin, &myParticles[myNumParticles+1],
				     dummy);
		//if I have any particles in this bin, send them to the responsible TreePiece
		if((binEnd - binBegin) > 0) {
                  if (verbosity>=3) CkPrintf("me:%d to:%d how many:%d\n",thisIndex,*responsibleIter,(binEnd-binBegin));
                  if(*responsibleIter == thisIndex) {
                    acceptSortedParticles(binBegin, binEnd - binBegin);
                  } else {
                    pieces[*responsibleIter].acceptSortedParticles(binBegin, binEnd - binBegin);
                  }
		}
		if(&myParticles[myNumParticles + 1] <= binEnd)
			break;
		binBegin = binEnd;
	}

        incomingParticlesSelf = true;
        acceptSortedParticles(binBegin, 0);
	//resize myParticles so we can fit the sorted particles and
	//the boundary particles
	//if(dm->particleCounts[myPlace] > (int) myNumParticles) {
	//    delete[] myParticles;
	//    myParticles = new GravityParticle[dm->particleCounts[myPlace] + 2];
	//    }
	//myNumParticles = dm->particleCounts[myPlace];
}

/// Accept particles from other TreePieces once the sorting has finished
void TreePiece::acceptSortedParticles(const GravityParticle* particles, const int n) {

  if (dm == NULL) {
      dm = (DataManager*)CkLocalNodeBranch(dataManagerID);
  }
  if(myPlace == -1) {
    if (dm == NULL) {
      dm = (DataManager*)CkLocalNodeBranch(dataManagerID);
    }
    myPlace = find(dm->responsibleIndex.begin(), dm->responsibleIndex.end(), thisIndex) - dm->responsibleIndex.begin();
    CkAssert(myPlace < dm->responsibleIndex.size());
  }
  
  assert(myPlace >= 0 && myPlace < dm->particleCounts.size());
  // allocate new particles array on first call
  if (incomingParticles == NULL) {
    incomingParticles = new GravityParticle[dm->particleCounts[myPlace] + 2];
    assert(incomingParticles != NULL);
    if (verbosity > 1)
      ckout << "Treepiece "<<thisIndex<<": allocated "
	<< dm->particleCounts[myPlace]+2 <<" particles"<<endl;
  }

  memcpy(&incomingParticles[incomingParticlesArrived+1], particles, n*sizeof(GravityParticle));
  incomingParticlesArrived += n;

  if (verbosity>=3) ckout << thisIndex <<" waiting for "<<dm->particleCounts[myPlace]-incomingParticlesArrived<<" particles ("<<dm->particleCounts[myPlace]<<"-"<<incomingParticlesArrived<<")"<<(incomingParticlesSelf?" self":"")<<endl;

  if(dm->particleCounts[myPlace] == incomingParticlesArrived && incomingParticlesSelf) {
    //I've got all my particles
    delete[] myParticles;
    myParticles = incomingParticles;
    incomingParticles = NULL;
    myNumParticles = dm->particleCounts[myPlace];
    incomingParticlesArrived = 0;
    incomingParticlesSelf = false;

    sort(myParticles+1, myParticles+myNumParticles+1);
    //signify completion with a reduction
    if(verbosity>1) ckout << thisIndex <<" contributing to accept particles"<<endl;

    if (root != NULL) {
      root->fullyDelete();
      delete root;
      root = NULL;
      nodeLookupTable.clear();
    }
    contribute(0, 0, CkReduction::concat, callback);
  }
}

// Sum energies for diagnostics
void TreePiece::calcEnergy(const CkCallback& cb) {
    double dEnergy[6]; // 0 -> kinetic; 1 -> virial ; 2 -> potential
    Vector3D<double> L;
        
    dEnergy[0] = 0.0;
    dEnergy[1] = 0.0;
    dEnergy[2] = 0.0;
    for(unsigned int i = 0; i < myNumParticles; ++i) {
	GravityParticle *p = &myParticles[i+1];
	
	dEnergy[0] += p->mass*p->velocity.lengthSquared();
	dEnergy[1] += p->mass*dot(p->treeAcceleration, p->position);
	dEnergy[2] += p->mass*p->potential;
	L += p->mass*cross(p->position, p->velocity);
	}
    dEnergy[0] *= 0.5;
    dEnergy[2] *= 0.5;
    dEnergy[3] = L.x;
    dEnergy[4] = L.y;
    dEnergy[5] = L.z;
    
    contribute(6*sizeof(double), dEnergy, CkReduction::sum_double, cb);
}

void TreePiece::kick(int iKickRung, double dDelta[MAXRUNG+1], const CkCallback& cb) {
  for(unsigned int i = 1; i <= myNumParticles; ++i) {
    if(myParticles[i].rung >= iKickRung) {
      myParticles[i].velocity += dDelta[myParticles[i].rung]*myParticles[i].treeAcceleration;
    }
  }
  contribute(0, 0, CkReduction::concat, cb);
}

/**
 * Adjust timesteps of active particles.
 * @param iKickRung The rung we are on.
 * @param bEpsAccStep Use sqrt(eps/acc) timestepping
 * @param bGravStep Use sqrt(r^3/GM) timestepping
 * @param dEta Factor to use in determing timestep
 * @param dDelta Base timestep
 * @param dAccFac Acceleration scaling for cosmology
 * @param cb Callback function reduces currrent maximum rung
 */
void TreePiece::adjust(int iKickRung, int bEpsAccStep, int bGravStep,
		       double dEta, double dDelta,
                       double dAccFac, const CkCallback& cb) {
  int iCurrMaxRung = 0;
  for(unsigned int i = 1; i <= myNumParticles; ++i) {
    if(myParticles[i].rung >= iKickRung) {
      assert(myParticles[i].soft > 0.0);
      double dTIdeal = dDelta;
      if(bEpsAccStep) {
	  double dt = dEta*sqrt(myParticles[i].soft
				/(dAccFac*myParticles[i].treeAcceleration.length()));
	  if(dt < dTIdeal)
	      dTIdeal = dt;
	  }
      if(bGravStep) {
	  double dt = dEta/sqrt(dAccFac*myParticles[i].dtGrav);
	  if(dt < dTIdeal)
	      dTIdeal = dt;
	  }
	  
      int iNewRung = DtToRung(dDelta, dTIdeal);
      if(iNewRung < iKickRung) iNewRung = iKickRung;
      if(iNewRung > iCurrMaxRung) iCurrMaxRung = iNewRung;
      myParticles[i].rung = iNewRung;
    }
  }
  contribute(sizeof(int), &iCurrMaxRung, CkReduction::max_int, cb);
}

void TreePiece::rungStats(const CkCallback& cb) {
  int nInRung[MAXRUNG+1];
    
  for(int iRung = 0; iRung <= MAXRUNG; iRung++) nInRung[iRung] = 0;
  for(unsigned int i = 1; i <= myNumParticles; ++i) {
    nInRung[myParticles[i].rung]++;
  }
  contribute((MAXRUNG+1)*sizeof(int), nInRung, CkReduction::sum_int, cb);
}

void TreePiece::drift(double dDelta, const CkCallback& cb) {
    callback = cb;		// called by assignKeys()
  if (root != NULL) {
    // Delete the tree since is no longer useful
    root->fullyDelete();
    delete root;
    root = NULL;
    nodeLookupTable.clear();
  }
  if(bucketReqs != NULL) {
      delete[] bucketReqs;
      bucketReqs = NULL;
      }

  boundingBox.reset();
  int bInBox = 1;
  
  for(unsigned int i = 0; i < myNumParticles; ++i) {
      myParticles[i+1].position += dDelta*myParticles[i+1].velocity;
      if(bPeriodic) {
	  for(int j = 0; j < 3; j++) {
	      if(myParticles[i+1].position[j] >= 0.5*fPeriod[j]){
		  myParticles[i+1].position[j] -= fPeriod[j];
		  }
	      if(myParticles[i+1].position[j] < -0.5*fPeriod[j]){
		  myParticles[i+1].position[j] += fPeriod[j];
		  }
	      // Sanity Checks
	      bInBox = bInBox
		  && (myParticles[i+1].position[j] >= -0.5*fPeriod[j]);
	      bInBox = bInBox
		  && (myParticles[i+1].position[j] < 0.5*fPeriod[j]);
	      }
	  CkAssert(bInBox);
	  }
      boundingBox.grow(myParticles[i+1].position);
      }
  CkAssert(bInBox);
  contribute(sizeof(OrientedBox<float>), &boundingBox,
		   growOrientedBox_float,
		   CkCallback(CkIndex_TreePiece::assignKeys(0), pieces));
}

void TreePiece::setSoft(const double dSoft) {
  for(unsigned int i = 1; i <= myNumParticles; ++i) {
      myParticles[i].soft = dSoft;
  }
}

/*
 * Gathers information for center of mass calculation
 * For each particle type the 0th and first mass moment is summed.
 */
void TreePiece::getCOM(const CkCallback& cb) {
    int i;
    double com[12]; // structure is m*position and mass for all
		    // particle types;
    for(i = 0; i < 12; i++)
	com[i] = 0;
    for(i = 1; i <= myNumParticles; ++i) {
	double m = myParticles[i].mass;
	if ( TYPETest(&(myParticles[i]), TYPE_GAS) ) {
	    com[0] += m*myParticles[i].position[0];
	    com[1] += m*myParticles[i].position[1];
	    com[2] += m*myParticles[i].position[2];
	    com[3] += m;
	    }
	else if ( TYPETest(&(myParticles[i]), TYPE_DARK) ) {
	    com[4] += m*myParticles[i].position[0];
	    com[5] += m*myParticles[i].position[1];
	    com[6] += m*myParticles[i].position[2];
	    com[7] += m;
	    }
	else if ( TYPETest(&(myParticles[i]), TYPE_STAR) ) {
	    com[8] += m*myParticles[i].position[0];
	    com[9] += m*myParticles[i].position[1];
	    com[10] += m*myParticles[i].position[2];
	    com[11] += m;
	    }
	}
    contribute(12*sizeof(double), com, CkReduction::sum_double, cb);
}

/*
 * Gathers information for center of mass calculation for one type of
 * particle.
 */
void TreePiece::getCOMByType(int iType, const CkCallback& cb) {
    int i;
    double com[4]; // structure is m*position and mass

    for(i = 0; i < 4; i++)
	com[i] = 0;
    for(i = 1; i <= myNumParticles; ++i) {
	if ( TYPETest(&(myParticles[i]), iType) ) {
	    double m = myParticles[i].mass;
	    com[0] += m*myParticles[i].position[0];
	    com[1] += m*myParticles[i].position[1];
	    com[2] += m*myParticles[i].position[2];
	    com[3] += m;
	    }
	}
    contribute(4*sizeof(double), com, CkReduction::sum_double, cb);
}

struct SortStruct {
  int iOrder;
  int iStore;
};

int CompSortStruct(const void * a, const void * b) {
  return ( ( ((struct SortStruct *) a)->iOrder < ((struct SortStruct *) b)->iOrder ? -1 : 1 ) );
}

void TreePiece::SetTypeFromFileSweep(int iSetMask, char *file, 
	   struct SortStruct *ss, int nss, int *pniOrder, int *pnSet) {
  int niOrder = 0, nSet = 0;
  int iOrder, iOrderOld, nRet;
  FILE *fp;
  int iss;

  fp = fopen( file, "r" );
  assert( fp != NULL );

  iss = 0;
  iOrderOld = -1;
  while ( (nRet=fscanf( fp, "%d\n", &iOrder )) == 1 ) {
	niOrder++;
	assert( iOrder > iOrderOld );
	iOrderOld = iOrder;
	while (ss[iss].iOrder < iOrder) {
	  iss++;
	  if (iss >= nss) goto DoneSS;
	}
	if (iOrder == ss[iss].iOrder) {
	  TYPESet(&(myParticles[1 + ss[iss].iStore]),iSetMask);
	  nSet++;
	}
  }
 
DoneSS:
  /* Finish reading file to verify consistency across processors */
  while ( (nRet=fscanf( fp, "%d\n", &iOrder )) == 1 ) {
	niOrder++;
	assert( iOrder > iOrderOld );
	iOrderOld = iOrder;
	}
  fclose(fp);

  *pniOrder += niOrder;
  *pnSet += nSet;

  return;
}

/*
 * Set particle type by reading in iOrders from file
 */
void
TreePiece::setTypeFromFile(int iSetMask, char *file, const CkCallback& cb)
{
  struct SortStruct *ss;
  int i,nss;

  int niOrder = 0;
  int nSet = 0;

  nss = myNumParticles;
  ss = (struct SortStruct *) malloc(sizeof(*ss)*nss);
  assert( ss != NULL );

  for(i=0;i<nss;++i) {
	ss[i].iOrder = 	myParticles[i+1].iOrder;
	ss[i].iStore = i;
  }

  qsort( ss, nss, sizeof(*ss), CompSortStruct );

  SetTypeFromFileSweep(iSetMask, file, ss, nss, &niOrder, &nSet);

  free( ss );

  int nSetOut[2];
  nSetOut[0] = niOrder;
  nSetOut[1] = nSet;
  
  contribute(2*sizeof(int), nSetOut, CkReduction::sum_int, cb);
}

/*
 * Render this processors portion of the image
 */
void TreePiece::DumpFrame(InDumpFrame in, const CkCallback& cb) 
{
    void *bufImage = malloc(sizeof(in) + in.nxPix*in.nyPix*sizeof(DFIMAGE));
    void *Image = ((char *)bufImage) + sizeof(in);
    int nImage;
    *((struct inDumpFrame *)bufImage) = in; //start of reduction
					    //message is the parameters
    
    dfClearImage( &in, Image, &nImage);
    GravityParticle *p = &(myParticles[1]);
    
    dfRenderParticlesInit( &in, TYPE_GAS, TYPE_DARK, TYPE_STAR,
			   &p->position[0], &p->mass, &p->soft, &p->soft,
			   &p->iType, 
#ifdef GASOLINE 
			   &p->fTimeForm,
#else
		/* N.B. This is just a place holder when we don't have stars */
			   &p->mass,
#endif
			   p, sizeof(*p) );
    dfRenderParticles( &in, Image, p, myNumParticles);
    contribute(sizeof(in) + nImage, bufImage, dfImageReduction, cb);
    free(bufImage);
    }

void TreePiece::buildTree(int bucketSize, const CkCallback& cb) {
#if COSMO_DEBUG > 1
  char fout[100];
  sprintf(fout,"tree.%d.%d.after",thisIndex,iterationNo);
  ofstream ofs(fout);
  for (int i=1; i<=myNumParticles; ++i)
    ofs << keyBits(myParticles[i].key,63) << " " << myParticles[i].position[0] << " "
        << myParticles[i].position[1] << " " << myParticles[i].position[2] << endl;
  ofs.close();
#endif

  maxBucketSize = bucketSize;
  callback = cb;

#if INTERLIST_VER > 0
  myTreeLevels=-1;
#endif
  //printing all particles
  //CkPrintf("\n\n\nbuilding tree, useTree:%d\n\n\n\n",useTree);
  //for(int i=0;i<myNumParticles+2;i++)
    //CkPrintf("[%d] %016llx  %lf,%lf,%lf\n",thisIndex,myParticles[i].key,myParticles[i].position.x,myParticles[i].position.y,myParticles[i].position.z);
  // decide which logic are we using to divide the particles: Oct or ORB
  switch (useTree) {
  case Binary_Oct:
  case Oct_Oct:
    Key bounds[2];
    //sort(myParticles+1, myParticles+myNumParticles+1);
#ifdef COSMO_PRINT
    CkPrintf("[%d] Keys: %016llx %016llx\n",thisIndex,myParticles[1].key,myParticles[myNumParticles].key);
#endif
    bounds[0] = myParticles[1].key;
    bounds[1] = myParticles[myNumParticles].key;
//    contribute(2 * sizeof(Key), bounds, CkReduction::concat, CkCallback(CkIndex_TreePiece::collectSplitters(0), thisArrayID));
      contribute(2 * sizeof(Key), bounds, CkReduction::concat, CkCallback(CkIndex_DataManager::collectSplitters(0), CProxy_DataManager(dataManagerID)));
    break;
  case Binary_ORB:
    //CkAbort("ORB logic for tree-build not yet implemented");
    //contribute(0,0,CkReduction::concat,sorterCallBack);
    contribute(0, 0, CkReduction::concat, CkCallback(CkIndex_TreePiece::startORBTreeBuild(0), thisArrayID));
    break;
  }
}

/*
class KeyDouble {
  Key first;
  Key second;
public:
  inline bool operator<(const KeyDouble& k) const {
    return first < k.first;
  }
};
*/

void TreePiece::quiescence() {
  /*
  char fout[100];
  sprintf(fout,"tree.%d.%d",thisIndex,iterationNo);
  ofstream ofs(fout);
  printTree(root,ofs);
  ofs.close();
  */
  /*
  CkPrintf("[%d] quiescence, %d left\n",thisIndex,momentRequests.size());
  for (MomentRequestType::iterator iter = momentRequests.begin(); iter != momentRequests.end(); iter++) {
    CkVec<int> *l = iter->second;
    for (int i=0; i<l->length(); ++i) {
      CkPrintf("[%d] quiescence: %s to %d\n",thisIndex,keyBits(iter->first,63).c_str(),(*l)[i]);
    }
  }
  */
  CkPrintf("[%d] quiescence detected, pending %d\n",thisIndex,myNumParticlesPending);
  for (unsigned int i=0; i<numBuckets; ++i) {
    //if (bucketReqs[i].numAdditionalRequests != 0)
    if (sRemoteGravityState->counterArrays[0][i] + sLocalGravityState->counterArrays[0][i] == 0)
      //CkPrintf("[%d] requests for %d remaining %d\n",thisIndex,i,bucketReqs[i].numAdditionalRequests);
      CkPrintf("[%d] requests for %d remaining %d\n",thisIndex,i,sRemoteGravityState->counterArrays[0][i] + sLocalGravityState->counterArrays[0][i]);
  }
  CkPrintf("quiescence detected!\n");
  mainChare.niceExit();
}

GenericTreeNode *TreePiece::get3DIndex() {
  GenericTreeNode *node = root;
  while (node != NULL && node->getType() != Internal && node->getType() != Bucket) {
    int next = -1;
    GenericTreeNode *child;
    for (int i=0; i<node->numChildren(); ++i) {
      child = node->getChildren(i);
      if (child->getType() == Internal || child->getType() == Boundary) {
        if (next != -1) return NULL;
        next = i;
      }
    }
    if (next == -1) return NULL;
    node = node->getChildren(next);
  }
  return node;
}

/*
void TreePiece::collectSplitters(CkReductionMsg* m) {
  numSplitters = 2 * numTreePieces;
  delete[] splitters;
  splitters = new Key[numSplitters];
  Key* splits = static_cast<Key *>(m->getData());
  copy(splits, splits + numSplitters, splitters);
  KeyDouble* splitters2 = (KeyDouble *)splitters;
  //sort(splitters, splitters + numSplitters);
  sort(splitters2, splitters2 + numTreePieces);
  for (unsigned int i=1; i<numSplitters; ++i) {
    if (splitters[i] < splitters[i-1]) {
      //for (unsigned int j=0; j<numSplitters; ++j)
      //  CkPrintf("%d: Key %d = %016llx\n",thisIndex,j,splitters[j]);
      if(thisIndex==0)
        CkAbort("Keys not ordered");
    }
  }
  splitters[0] = firstPossibleKey;
  contribute(0, 0, CkReduction::concat, CkCallback(CkIndex_TreePiece::startOctTreeBuild(0), thisArrayID));
  delete m;
  if(verbosity > 3)
    ckerr << "TreePiece " << thisIndex << ": Collected splitters" << endl;
}
*/

/*****************ORB**********************/
/*void TreePiece::receiveBoundingBoxes(BoundingBoxes *msg){

  //OrientedBox<float> *boxesMsg = static_cast<OrientedBox<float>* >(msg->getData())
  //boxes = new OrientedBox<float>[numTreePieces];
  
  OrientedBox<float> *boxesMsg = msg->boxes;
  copy(boxesMsg,boxesMsg+numTreePieces,boxes);
 
  contribute(0, 0, CkReduction::concat, CkCallback(CkIndex_TreePiece::startORBTreeBuild(0), thisArrayID));
}*/

void TreePiece::startORBTreeBuild(CkReductionMsg* m){
  delete m;
 
  myParticles[0].key = thisIndex;
  myParticles[myNumParticles+1].key = thisIndex;

  /*//Find out how many levels will be there before tree goes
  //into a treepiece
  chunkRootLevel=0;
  unsigned int tmp = numTreePieces;
  while(tmp){
    tmp >>= 1;
    chunkRootLevel++;
  }
  chunkRootLevel--;*/
  compFuncPtr[0]= &comp_dim0;
  compFuncPtr[1]= &comp_dim1;
  compFuncPtr[2]= &comp_dim2;
  
  root = new BinaryTreeNode(1, numTreePieces>1?Tree::Boundary:Tree::Internal, 0, myNumParticles+1, 0);
  
  if (thisIndex == 0) root->firstParticle ++;
  if (thisIndex == (int)numTreePieces-1) root->lastParticle --;
  root->particleCount = myNumParticles;
  nodeLookupTable[(Tree::NodeKey)1] = root;

  //root->key = firstPossibleKey;
  root->boundingBox = boundingBox;
  //nodeLookup[root->lookupKey()] = root;
  numBuckets = 0;
  bucketList.clear();
  
  if(verbosity > 3)
    ckerr << "TreePiece " << thisIndex << ": Starting tree build" << endl;

#if INTERLIST_VER > 0
  root->startBucket=0;
#endif
  // recursively build the tree
  buildORBTree(root, 0);

  delete [] boxes;
  delete [] splitDims;
  //Keys to all the particles have been assigned inside buildORBTree
  
  //CkPrintf("[%d] finished building local tree\n",thisIndex);
  
  // check all the pending requests in for RemoteMoments
  for (MomentRequestType::iterator iter = momentRequests.begin(); iter != momentRequests.end(); ) {
    NodeKey nodeKey = iter->first;
    GenericTreeNode *node = keyToNode(nodeKey);
    CkVec<int> *l = iter->second;
    //CkPrintf("[%d] checking moments requests for %s (%s) upon treebuild finished\n",thisIndex,keyBits(iter->first,63).c_str(),keyBits(node->getKey(),63).c_str());
    CkAssert(node != NULL);
    // we actually need to increment the iterator before deleting the element,
    // otherwise the iterator lose its validity!
    iter++;
    if (node->getType() == Empty || node->moments.totalMass > 0) {
      for (int i=0; i<l->length(); ++i) {
	streamingProxy[(*l)[i]].receiveRemoteMoments(nodeKey, node->getType(), node->firstParticle, node->particleCount, node->moments, node->boundingBox);
	//CkPrintf("[%d] sending moments of %s to %d upon treebuild finished\n",thisIndex,keyBits(node->getKey(),63).c_str(),(*l)[i]);
      }
      delete l;
      momentRequests.erase(node->getKey());
    }
  }

  if(verbosity > 3)
    ckerr << "TreePiece " << thisIndex << ": Number of buckets: " << numBuckets << endl;
  if(verbosity > 3)
    ckerr << "TreePiece " << thisIndex << ": Finished tree build, resolving boundary nodes" << endl;

  if (numTreePieces == 1) {
    dm->notifyPresence(root);
    contribute(sizeof(callback), &callback, CkReduction::random, CkCallback(CkIndex_DataManager::combineLocalTrees((CkReductionMsg*)NULL), CProxy_DataManager(dataManagerID)));
  }

}

OrientedBox<float> TreePiece::constructBoundingBox(GenericTreeNode* node,int level, int numChild){

  OrientedBox<float> tmpBox;
  if(node->getType()==NonLocal){
    if(numChild==0){
      tmpBox = boxes[level];
      tmpBox.greater_corner[splitDims[level]] = boxes[level+1].lesser_corner[splitDims[level]];
    }
    else{
      tmpBox = boxes[level];
      tmpBox.lesser_corner[splitDims[level]] = boxes[level+1].greater_corner[splitDims[level]];
    }
    return tmpBox;
  }
  else{
    return boxes[level+1];
  }
  
  /*SFC::Key tmp = 1 << (level+1);
  tmp = key - tmp;
  
  for(int i=0;i<numTreePieces;i++){
    if(tmp==(i>>(chunkRootLevel-level-1))){
      tmpBox.grow(boxes[i].lesser_corner);
      tmpBox.grow(boxes[i].greater_corner);
    }
  }
  return tmpBox;*/
}

void TreePiece::buildORBTree(GenericTreeNode * node, int level){
  
#if INTERLIST_VER > 0
  if(level>myTreeLevels)
    myTreeLevels=level;
#endif
  //CkPrintf("[%d] in build ORB Tree, level:%d\n",thisIndex,level);
  if (level == 63) {
    ckerr << thisIndex << ": TreePiece(ORB): This piece of tree has exhausted all the bits in the keys.  Super double-plus ungood!" << endl;
    ckerr << "Left particle: " << (node->firstParticle) << " Right particle: " << (node->lastParticle) << endl;
    ckerr << "Left key : " << keyBits((myParticles[node->firstParticle]).key, 63).c_str() << endl;
    ckerr << "Right key: " << keyBits((myParticles[node->lastParticle]).key, 63).c_str() << endl;
    return;
  }

  CkAssert(node->getType() == Boundary || node->getType() == Internal);
  
  node->makeOrbChildren(myParticles, myNumParticles, level, chunkRootLevel, compFuncPtr);
  node->rungs = 0;

  GenericTreeNode *child;
  for (unsigned int i=0; i<node->numChildren(); ++i) {
    child = node->getChildren(i);
    CkAssert(child != NULL);

    if(level<chunkRootLevel){
      child->boundingBox = constructBoundingBox(child,level,i);
    }

#if INTERLIST_VER > 0
    child->startBucket=numBuckets;
#endif
    nodeLookupTable[child->getKey()] = child;
    if (child->getType() == NonLocal) {
      // find a remote index for the node
      int first, last;
      bool isShared = nodeOwnership(child->getKey(), first, last);
      //CkPrintf("[%d] child Key:%lld, firstOwner:%d, lastOwner:%d\n",thisIndex,child->getKey(),first,last);
      CkAssert(!isShared);
      if (last < first) {
	      // the node is really empty because falling between two TreePieces
              child->makeEmpty();
	      child->remoteIndex = thisIndex;
      } else {
	      child->remoteIndex = first + (thisIndex & (last-first));
	      // if we have a remote child, the node is a Boundary. Thus count that we
	      // have to receive one more message for the NonLocal node
	      node->remoteIndex --;
	      // request the remote chare to fill this node with the Moments
	      streamingProxy[child->remoteIndex].requestRemoteMoments(child->getKey(), thisIndex);
	      //CkPrintf("[%d] asking for moments of %s to %d\n",thisIndex,keyBits(child->getKey(),63).c_str(),child->remoteIndex);
      }
    } else if (child->getType() == Internal && child->lastParticle - child->firstParticle < maxBucketSize) {
      CkAssert(child->firstParticle != 0 && child->lastParticle != myNumParticles+1);
      child->remoteIndex = thisIndex;
      child->makeBucket(myParticles);
      bucketList.push_back(child);

      //Assign keys to all the particles inside the bucket
      int num = child->lastParticle - child->firstParticle + 1;
      int bits = 0;

      while(num > (1<<bits)){ bits++; }

      Key mask = 1 << (level+1);
      mask = ~mask;
      Key tmpKey = child->getKey() & mask;
      tmpKey = tmpKey << bits;

      for(int i=child->firstParticle;i<=child->lastParticle;i++){
        myParticles[i].key = tmpKey;
        tmpKey++;
      }

#if INTERLIST_VER > 0
      child->bucketListIndex=numBuckets;
      child->startBucket=numBuckets;
#endif
      numBuckets++;
      if (node->getType() != Boundary) node->moments += child->moments;
      if (child->rungs > node->rungs) node->rungs = child->rungs;
    } else if (child->getType() == Empty) {
      child->remoteIndex = thisIndex;
    } else {
      if (child->getType() == Internal) child->remoteIndex = thisIndex;
      // else the index is already 0
      buildORBTree(child, level+1);
      // if we have a Boundary child, we will have to compute it's multipole
      // before we can compute the multipole of the current node (and we'll do
      // it in receiveRemoteMoments)
      if (child->getType() == Boundary) node->remoteIndex --;
      if (node->getType() != Boundary) node->moments += child->moments;
      if (child->rungs > node->rungs) node->rungs = child->rungs;
    }
  }

  /* The old version collected Boundary nodes, the new version collects NonLocal nodes */

  if (node->getType() == Internal) {
    calculateRadiusFarthestCorner(node->moments, node->boundingBox);
  }

}
/******************************************/

void TreePiece::startOctTreeBuild(CkReductionMsg* m) {
  delete m;
	
  if (dm == NULL) {
      dm = (DataManager*)CkLocalNodeBranch(dataManagerID);
  }

  if(thisIndex == 0)
    myParticles[0].key = firstPossibleKey;
  else
    myParticles[0].key = dm->splitters[2 * thisIndex - 1];
	
  if(thisIndex == (int) numTreePieces - 1)
    myParticles[myNumParticles + 1].key = lastPossibleKey;
  else
    myParticles[myNumParticles + 1].key = dm->splitters[2 * thisIndex + 2];
	
  // create the root of the global tree
  switch (useTree) {
  case Binary_Oct:
    root = new BinaryTreeNode(1, numTreePieces>1?Tree::Boundary:Tree::Internal, 0, myNumParticles+1, 0);
    break;
  case Oct_Oct:
    //root = new OctTreeNode(1, Tree::Boundary, 0, myNumParticles+1, 0);
    break;
  default:
    CkAbort("We should have never reached here!");
  }

  if (thisIndex == 0) root->firstParticle ++;
  if (thisIndex == (int)numTreePieces-1) root->lastParticle --;
  root->particleCount = myNumParticles;
  nodeLookupTable[(Tree::NodeKey)1] = root;

  //root->key = firstPossibleKey;
  root->boundingBox = boundingBox;
  //nodeLookup[root->lookupKey()] = root;
  numBuckets = 0;
  bucketList.clear();
  
  if(verbosity > 3)
    ckerr << "TreePiece " << thisIndex << ": Starting tree build" << endl;

  // set the number of chunks in which we will split the tree for remote computation
  /* OLD, moved to the CacheManager and startIteration
  numChunks = root->getNumChunks(_numChunks);
  assert(numChunks > 0);
  //remainingChunk = new int[numChunks];
  root->getChunks(_numChunks, prefetchRoots;
  */

#if INTERLIST_VER > 0
  root->startBucket=0;
#endif
  // recursively build the tree
  buildOctTree(root, 0);
/* jetley - save the first internal node for use later.
   needed because each treepiece must, for oct decomposition, send its centroid to a
   load balancing strategy object. the previous tree will have been deleted at this point.
   */
  CkVec <GenericTreeNode *> queue;
  GenericTreeNode *child, *temp;
  
  OrientedBox<float> box;
  queue.push_back(root);
  while(queue.size() != 0){
    temp = queue[queue.size()-1];
    CkAssert(temp != NULL);
    queue.remove(queue.size()-1);
    for(int i = 0; i < temp->numChildren(); i++){
      child = temp->getChildren(i);
      CkAssert(child != NULL);
      if(child->getType() == Boundary)
        queue.push_back(child);   // might have child that is an Internal node
      else if(child->getType() == Internal || child->getType() == Bucket){
        box.grow(child->boundingBox);
      }
    }
  }
  savedCentroid = box.center();

  //CkPrintf("[%d] finished building local tree\n",thisIndex);
  
  // check all the pending requests in for RemoteMoments
  for (MomentRequestType::iterator iter = momentRequests.begin(); iter != momentRequests.end(); ) {
    NodeKey nodeKey = iter->first;
    GenericTreeNode *node = keyToNode(nodeKey);
    CkVec<int> *l = iter->second;
    //CkPrintf("[%d] checking moments requests for %s (%s) upon treebuild finished\n",thisIndex,keyBits(iter->first,63).c_str(),keyBits(node->getKey(),63).c_str());
    CkAssert(node != NULL);
    // we actually need to increment the iterator before deleting the element,
    // otherwise the iterator lose its validity!
    iter++;
    if (node->getType() == Empty || node->moments.totalMass > 0) {
      for (int i=0; i<l->length(); ++i) {
	streamingProxy[(*l)[i]].receiveRemoteMoments(nodeKey, node->getType(), node->firstParticle, node->particleCount, node->moments, node->boundingBox);
	//CkPrintf("[%d] sending moments of %s to %d upon treebuild finished\n",thisIndex,keyBits(node->getKey(),63).c_str(),(*l)[i]);
      }
      delete l;
      momentRequests.erase(node->getKey());
    }
  }

  if(verbosity > 3)
    ckerr << "TreePiece " << thisIndex << ": Number of buckets: " << numBuckets << endl;
  if(verbosity > 3)
    ckerr << "TreePiece " << thisIndex << ": Finished tree build, resolving boundary nodes" << endl;

  if (numTreePieces == 1) {
    dm->notifyPresence(root);
    contribute(sizeof(callback), &callback, CkReduction::random, CkCallback(CkIndex_DataManager::combineLocalTrees((CkReductionMsg*)NULL), CProxy_DataManager(dataManagerID)));
  }
}

/// Determine who are all the owners of this node
/// @return true if the caller is part of the owners, false otherwise
inline bool TreePiece::nodeOwnership(const Tree::NodeKey nkey, int &firstOwner, int &lastOwner) {
  
  if(useTree == Binary_ORB){ // Added for ORB Trees
    int keyLevel=0;
    Key tmpKey = Key(nkey);
    while(tmpKey > 1){
      tmpKey >>= 1;
      keyLevel++;
    }
    if(keyLevel >= chunkRootLevel){
      tmpKey = nkey >> (keyLevel-chunkRootLevel);
      tmpKey = tmpKey - (1 << chunkRootLevel);
      firstOwner = tmpKey;
      lastOwner = tmpKey;
    }
    else{
      tmpKey = nkey << (chunkRootLevel - keyLevel);
      tmpKey = tmpKey - (1 << chunkRootLevel);
      firstOwner = tmpKey;

      Key mask = (1 << (chunkRootLevel - keyLevel)) - 1;
      tmpKey = nkey << (chunkRootLevel - keyLevel);
      tmpKey = tmpKey - (1 << chunkRootLevel);
      tmpKey = tmpKey + mask;
      lastOwner = tmpKey;
    }
  }
  else{
    Key firstKey = Key(nkey);
    Key lastKey = Key(nkey + 1);
    const Key mask = Key(1) << 63;
    while (! (firstKey & mask)) {
      firstKey <<= 1;
      lastKey <<= 1;
    }
    firstKey &= ~mask;
    lastKey &= ~mask;
    lastKey -= 1;
    Key *locLeft = lower_bound(dm->splitters, dm->splitters + dm->numSplitters, firstKey);
    Key *locRight = upper_bound(locLeft, dm->splitters + dm->numSplitters, lastKey);
    firstOwner = (locLeft - dm->splitters) >> 1;
    lastOwner = (locRight - dm->splitters - 1) >> 1;
#if COSMO_PRINT > 1
    std::string str = keyBits(nkey,63);
    CkPrintf("[%d] NO: key=%s, first=%d, last=%d\n",thisIndex,str.c_str(),locLeft-dm->splitters,locRight-dm->splitters);
#endif
  }
  return (thisIndex >= firstOwner && thisIndex <= lastOwner);
}

/** A recursive algorithm for building my tree.
    Examines successive bits in the particles' keys, looking for splits.
    Each bit is a level of nodes in the tree.  We keep going down until
    we can bucket the particles.
*/
void TreePiece::buildOctTree(GenericTreeNode * node, int level) {

#if INTERLIST_VER > 0
  if(level>myTreeLevels)
    myTreeLevels=level;
#endif
  
  if (level == 63) {
    ckerr << thisIndex << ": TreePiece: This piece of tree has exhausted all the bits in the keys.  Super double-plus ungood!" << endl;
    ckerr << "Left particle: " << (node->firstParticle) << " Right particle: " << (node->lastParticle) << endl;
    ckerr << "Left key : " << keyBits((myParticles[node->firstParticle]).key, 63).c_str() << endl;
    ckerr << "Right key: " << keyBits((myParticles[node->lastParticle]).key, 63).c_str() << endl;
    return;
  }

  CkAssert(node->getType() == Boundary || node->getType() == Internal);
  
  node->makeOctChildren(myParticles, myNumParticles, level);
  node->boundingBox.reset();
  node->rungs = 0;

  GenericTreeNode *child;
  for (unsigned int i=0; i<node->numChildren(); ++i) {
    child = node->getChildren(i);
    CkAssert(child != NULL);
#if INTERLIST_VER > 0
    child->startBucket=numBuckets;
#endif
    nodeLookupTable[child->getKey()] = child;
    if (child->getType() == NonLocal) {
      // find a remote index for the node
      int first, last;
      bool isShared = nodeOwnership(child->getKey(), first, last);
      CkAssert(!isShared);
      if (last < first) {
	// the node is really empty because falling between two TreePieces
	child->makeEmpty();
	child->remoteIndex = thisIndex;
      } else {
	child->remoteIndex = first + (thisIndex & (last-first));
	// if we have a remote child, the node is a Boundary. Thus count that we
	// have to receive one more message for the NonLocal node
	node->remoteIndex --;
	// request the remote chare to fill this node with the Moments
	streamingProxy[child->remoteIndex].requestRemoteMoments(child->getKey(), thisIndex);
	//CkPrintf("[%d] asking for moments of %s to %d\n",thisIndex,keyBits(child->getKey(),63).c_str(),child->remoteIndex);
      }
    } else if (child->getType() == Internal && child->lastParticle - child->firstParticle < maxBucketSize) {
      CkAssert(child->firstParticle != 0 && child->lastParticle != myNumParticles+1);
      child->remoteIndex = thisIndex;
      child->makeBucket(myParticles);
      bucketList.push_back(child);
#if INTERLIST_VER > 0
      child->bucketListIndex=numBuckets;
      child->startBucket=numBuckets;
#endif
      numBuckets++;
      if (node->getType() != Boundary) {
        node->moments += child->moments;
        node->boundingBox.grow(child->boundingBox);
      }
      if (child->rungs > node->rungs) node->rungs = child->rungs;
    } else if (child->getType() == Empty) {
      child->remoteIndex = thisIndex;
    } else {
      if (child->getType() == Internal) child->remoteIndex = thisIndex;
      // else the index is already 0
      buildOctTree(child, level+1);
      // if we have a Boundary child, we will have to compute it's multipole
      // before we can compute the multipole of the current node (and we'll do
      // it in receiveRemoteMoments)
      if (child->getType() == Boundary) node->remoteIndex --;
      if (node->getType() != Boundary) {
        node->moments += child->moments;
        node->boundingBox.grow(child->boundingBox);
      }
      // for the rung information we can always do now since it is a local property
      if (child->rungs > node->rungs) node->rungs = child->rungs;
    }
  }

  /* The old version collected Boundary nodes, the new version collects NonLocal nodes */

  if (node->getType() == Internal) {
    calculateRadiusFarthestCorner(node->moments, node->boundingBox);
  }
}

void TreePiece::requestRemoteMoments(const Tree::NodeKey key, int sender) {
  GenericTreeNode *node = keyToNode(key);
  if (node != NULL && (node->getType() == Empty || node->moments.totalMass > 0)) {
    streamingProxy[sender].receiveRemoteMoments(key, node->getType(), node->firstParticle, node->particleCount, node->moments, node->boundingBox);
    //CkPrintf("[%d] sending moments of %s to %d directly\n",thisIndex,keyBits(node->getKey(),63).c_str(),sender);
  } else {
    CkVec<int> *l = momentRequests[key];
    if (l == NULL) {
      l = new CkVec<int>();
      momentRequests[key] = l;
      //CkPrintf("[%d] Inserting new CkVec\n",thisIndex);
    }
    l->push_back(sender);
    //CkPrintf("[%d] queued request from %d for %s\n",thisIndex,sender,keyBits(key,63).c_str());
  }
}

void TreePiece::receiveRemoteMoments(const Tree::NodeKey key, Tree::NodeType type, int firstParticle, int numParticles, const MultipoleMoments& moments, const OrientedBox<double>& box) {
  GenericTreeNode *node = keyToNode(key);
  CkAssert(node != NULL);
  //CkPrintf("[%d] received moments for %s\n",thisIndex,keyBits(key,63).c_str());
  // assign the incoming moments to the node
  if (type == Empty) node->makeEmpty();
  else {
    if (type == Bucket) {
      node->setType(NonLocalBucket);
      node->firstParticle = firstParticle;
      node->lastParticle = firstParticle + numParticles - 1;
    }
    node->particleCount = numParticles;
    node->moments = moments;
    node->boundingBox = box;
  }
  // look if we can compute the moments of some ancestors, and eventually send
  // them to a requester
  GenericTreeNode *parent = node->parent;
  while (parent != NULL && ++parent->remoteIndex == 0) {
    // compute the multipole for the parent
    //CkPrintf("[%d] computed multipole of %s\n",thisIndex,keyBits(parent->getKey(),63).c_str());
    parent->particleCount = 0;
    parent->remoteIndex = thisIndex; // reset the reference index to ourself
    GenericTreeNode *child;
    for (unsigned int i=0; i<parent->numChildren(); ++i) {
      child = parent->getChildren(i);
      parent->particleCount += child->particleCount;
      parent->moments += child->moments;
      parent->boundingBox.grow(child->boundingBox);
    }
    calculateRadiusFarthestCorner(parent->moments, parent->boundingBox);
    // check if someone has requested this node
    MomentRequestType::iterator iter;
    if ((iter = momentRequests.find(parent->getKey())) != momentRequests.end()) {
      CkVec<int> *l = iter->second;
      for (int i=0; i<l->length(); ++i) {
	streamingProxy[(*l)[i]].receiveRemoteMoments(parent->getKey(), parent->getType(), parent->firstParticle, parent->particleCount, parent->moments, parent->boundingBox);
	//CkPrintf("[%d] sending moments of %s to %d\n",thisIndex,keyBits(parent->getKey(),63).c_str(),(*l)[i]);
      }
      delete l;
      momentRequests.erase(parent->getKey());
    }
    // go to the next ancestor
    node = parent;
    parent = node->parent;
  }
  if (parent == NULL) {
    // if we are here then we are at the root, and thus we have finished to get
    // all moments
    //CkPrintf("[%d] contributing after building the tree\n",thisIndex);
    dm->notifyPresence(root);
    contribute(sizeof(callback), &callback, CkReduction::random, CkCallback(CkIndex_DataManager::combineLocalTrees((CkReductionMsg*)NULL), CProxy_DataManager(dataManagerID)));
  }// else CkPrintf("[%d] still missing one child of %s\n",thisIndex,keyBits(parent->getKey(),63).c_str());
}

Vector3D<double> TreePiece::decodeOffset(int reqID) 
{
    int offsetcode = reqID >> 22;
    int x = (offsetcode & 0x7) - 3;
    int y = ((offsetcode >> 3) & 0x7) - 3;
    int z = ((offsetcode >> 6) & 0x7) - 3;
    
    Vector3D<double> offset(x*fPeriod.x, y*fPeriod.y, z*fPeriod.z);
    
    return offset;
    }

int decodeReqID(int reqID) 
{
    const int offsetmask = 0x1ff << 22;
    
    return reqID & (~offsetmask);
    }

int encodeOffset(int reqID, int x, int y, int z)
{
    // Bit limitations follow
    CkAssert(x > -4 && x < 4);
    CkAssert(y > -4 && y < 4);
    CkAssert(z > -4 && z < 4);
    // Replica in each direction is mapped to 0-7 range
    int offsetcode = (x + 3) | ((y+3) << 3) | ((z+3) << 6);
    
    // Assume we only have 32 bits to work with (minus sign bit)
    // 4 million buckets is our limitation
    CkAssert(reqID < (1 << 22));
    return reqID | (offsetcode << 22);
    }

// Add reqID to encoded offset
int reEncodeOffset(int reqID, int offsetID)
{
    const int offsetmask = 0x1ff << 22;
    
    return reqID | (offsetmask & offsetID);
    }


void TreePiece::initBuckets() {
  int ewaldCondition = (bEwald ? 0 : 1);
  for (unsigned int j=0; j<numBuckets; ++j) {
    GenericTreeNode* node = bucketList[j];
    int numParticlesInBucket = node->particleCount;

    CkAssert(numParticlesInBucket <= maxBucketSize);
    
    // TODO: active bounds may give a performance boost in the
    // multi-timstep regime.
    // node->boundingBox.reset();  // XXXX dangerous should have separate
				// Active bounds
    for(int i = node->firstParticle; i <= node->lastParticle; ++i) {
      if (myParticles[i].rung >= activeRung) {
	nActive++;
        myParticles[i].treeAcceleration = 0;
        myParticles[i].potential = 0;
	myParticles[i].dtGrav = 0;
	// node->boundingBox.grow(myParticles[i].position);
      }
    }
    bucketReqs[j].finished = ewaldCondition;
    //bucketReqs[j].numAdditionalRequests = numChunks + 1;

/*#if COSMO_DEBUG > 1
    if(iterationNo==1 || listMigrated==true){
      std::set<Tree::NodeKey> *list = new std::set<Tree::NodeKey>();
      bucketcheckList.push_back(*list);
    }
#endif*/
  }
#if COSMO_DEBUG > 1 || defined CHANGA_REFACTOR_WALKCHECK
  bucketcheckList.resize(numBuckets);
#endif
}

void TreePiece::startNextBucket() {
  if(currentBucket >= numBuckets)
    return;

  sTopDown->init(sGravity, this);
  sGravity->init(bucketList[currentBucket], activeRung, sLocal);
  //State *nullstate = sGravity->getResumeState(currentBucket);

  // no need to save this combination in activeWalks list (local walks never miss)

  // start the tree walk from the tree built in the cache
  if (bucketList[currentBucket]->rungs >= activeRung) {
    for(int cr = 0; cr < numChunks; cr++){
      //GenericTreeNode *chunkRoot = keyToNode(prefetchRoots[cr]);
      GenericTreeNode *chunkRoot = dm->chunkRootToNode(prefetchRoots[cr]);
      for(int x = -nReplicas; x <= nReplicas; x++) {
        for(int y = -nReplicas; y <= nReplicas; y++) {
          for(int z = -nReplicas; z <= nReplicas; z++) {
            // last -1 arg is the activeWalkIndex
            sTopDown->walk(chunkRoot, sLocalGravityState, -1, encodeOffset(currentBucket, x,y,z), -1);
          }
        }
      }
    }
    // compute the ewald component of the force
    /*if(bEwald) {
      BucketEwald(bucketList[currentBucket], nReplicas, fEwCut);
    }*/
  }
  //bucketReqs[currentBucket].numAdditionalRequests --;
  sLocalGravityState->counterArrays[0][currentBucket]--;
  finishBucket(currentBucket);
  //	currentBucket++;
  //	startNextBucket();
  //delete gc;
  //delete nullstate; 
  //delete opt;
  //delete tw;
}

/*inline*/
void TreePiece::finishBucket(int iBucket) {
  BucketGravityRequest *req = &bucketReqs[iBucket];
  GenericTreeNode *node = bucketList[iBucket];
#ifdef COSMO_PRINT
  CkPrintf("[%d] Is finished %d? finished=%d, %d,%d still missing!\n",thisIndex,iBucket,req->finished,
          sRemoteGravityState->counterArrays[0][iBucket], sLocalGravityState->counterArrays[0][iBucket]
          );
          //req->numAdditionalRequests);
#endif

  // XXX finished means Ewald is done.
  if(req->finished && (sRemoteGravityState->counterArrays[0][iBucket] + sLocalGravityState->counterArrays[0][iBucket]) == 0) {
    myNumParticlesPending -= node->particleCount;
#ifdef COSMO_PRINT
    CkPrintf("[%d] Finished bucket %d, %d particles remaining\n",thisIndex,iBucket,myNumParticlesPending);
#endif
    /*
    int iStart = bucketList[iBucket]->firstParticle;
    for(unsigned int i = 0; i < req->numParticlesInBucket; ++i) {
      myParticles[iStart + i].treeAcceleration
	+= req->accelerations[i];
      myParticles[iStart + i].potential
	+= req->potentials[i];
    }
    */
    if(started && myNumParticlesPending == 0) {
      started = false;
      markWalkDone();
      /*
      delete sTopDown;
      delete sGravity;
      delete sLocal;
      delete sRemote;
      activeWalks.free();
      */
      //contribute(0, 0, CkReduction::concat, callback);
      /*   cout << "TreePiece " << thisIndex << ": Made " << myNumProxyCalls
	   << " proxy calls forward, " << myNumProxyCallsBack
	   << " to respond in finishBucket" << endl;*/
#if COSMO_STATS > 0
      if(verbosity>1)
	CkPrintf("[%d] TreePiece %d finished with bucket %d , openCriterions:%lld\n",CkMyPe(),thisIndex,iBucket,numOpenCriterionCalls);
#else
      if(verbosity>1)
	CkPrintf("[%d] TreePiece %d finished with bucket %d\n",CkMyPe(),thisIndex,iBucket);
#endif
      if(verbosity > 4)
	ckerr << "TreePiece " << thisIndex << ": My particles are done"
	     << endl;
    }
  }
}

void TreePiece::doAllBuckets(){
#if COSMO_DEBUG > 0
  char fout[100];
  sprintf(fout,"tree.%d.%d",thisIndex,iterationNo);
  ofstream ofs(fout);
  printTree(root,ofs);
  ofs.close();
  report();
#endif

  dummyMsg *msg = new (8*sizeof(int)) dummyMsg;
  *((int *)CkPriorityPtr(msg)) = numTreePieces * numChunks + thisIndex + 1;
  CkSetQueueing(msg,CK_QUEUEING_IFIFO);
  msg->val=0;

#if INTERLIST_VER > 0
  checkListLocal[0].length()=0;
  for(int i=0;i<=myTreeLevels;i++){
    cellListLocal[i].length()=0;
    particleListLocal[i].length()=0;
  }
#endif
  
  thisProxy[thisIndex].nextBucket(msg);
}

#ifdef CELL
#define CELLTHREASHOLD 100
#define CELLEWALDTHREASHOLD 30

inline void cellEnableComputation() {
  if (workRequestOut < CELLEWALDTHREASHOLD) {
    for (int i=0; i<ewaldMessages.length(); ++i) {
      CellComputation &comp = ewaldMessages[i];
      comp.owner.calculateEwald(comp.msg);
    }
    ewaldMessages.removeAll();
  }
}
#endif

void TreePiece::nextBucket(dummyMsg *msg){
  unsigned int i=0;

#if INTERLIST_VER > 0
  
  NodeType childType;
 
  while (i<_yieldPeriod && currentBucket < numBuckets
#ifdef CELL
    && workRequestOut < CELLTHREASHOLD
#endif
      ) {

    if(currentBucket==0){
      curNodeLocal=root;
      curLevelLocal=0;
    }
    
    //Top-level routine to compute interaction list for this bucket
    //Starts with curNodeLocal which belongs to myTree and walks down myTree
    //building lists at each level
    preWalkInterTree();
    
    //CkAssert(curNodeLocal->getType()==Bucket);
    CkAssert(checkListLocal[curLevelLocal].length()==0);
    
    if(myLocalCheckListEmpty && curNodeLocal->getType()!=Bucket){
      
      GenericTreeNode *tmpNode;
  
      int startBucket=curNodeLocal->startBucket;
      int lastBucket;
      int k;
  
      for(k=startBucket+1;k<numBuckets;k++){
        tmpNode = bucketList[k];
        if(tmpNode->lastParticle>curNodeLocal->lastParticle)
          break;
      }
      lastBucket=k-1;
    
      for(k=startBucket;k<=lastBucket;k++){
        calculateForceLocalBucket(k);
        currentBucket++;
        i++;
      }
      myLocalCheckListEmpty=false;
    }
    else{
      CkAssert(curNodeLocal->getType()==Bucket);
      //Go over both the lists to calculate forces with one bucket
      calculateForceLocalBucket(curNodeLocal->bucketListIndex);
      currentBucket++;
      i++;
    }

    /*GenericTreeNode* node = bucketList[curNodeLocal->bucketListIndex];
  
    for(unsigned int j = node->firstParticle; j <= node->lastParticle; ++j) {
      //for(int k=0;k<=curLevelLocal;k++){
        //if(cellListLocal[k].length()>0 || particleListLocal[k].length()>0)
          listPartForce(cellListLocal,particleListLocal,myParticles[j],curLevelLocal);
      //}
    }*/
   
    //Right now, because of -O3 BucketForce routines perform better than partForce routine
    //So, using the following
    if(currentBucket>=numBuckets)
      break;

    //Calculate starting Node for next tree walk
    //Go up myTree till we get to a parent whose all children are not done  with building lists
    //then, take the child as the starting point for the next tree walk
    GenericTreeNode *tmpNode=curNodeLocal;
    int flag=0;
    tmpNode->visitedL=true;
    while(tmpNode->parent != NULL){
      
      cellListLocal[curLevelLocal].length()=0;
      particleListLocal[curLevelLocal].length()=0;
      
      tmpNode=tmpNode->parent;
      curLevelLocal--;
      GenericTreeNode* childIterator;
      for(unsigned int j = 0; j < tmpNode->numChildren(); ++j) {
        childIterator = tmpNode->getChildren(j);
        CkAssert (childIterator != NULL);
        childType = childIterator->getType();
        if(childIterator->visitedL == false){
          if(childType == NonLocal || childType == Cached || childType == NonLocalBucket || childType == CachedBucket || childType==Empty || childType==CachedEmpty ){//|| childIterator->rungs < activeRung){
            childIterator->visitedL=true;
          }
          else{
            curNodeLocal=childIterator;
            curLevelLocal++;
            flag=1;
            break;
          }
        }
      }
      if(flag==0){
        tmpNode->visitedL=true;
      }
      if(flag==1){
        flag=0;
        break;
      }
    }
    /*
    int nextActiveBucket = numBuckets;
    if (tmpNode->parent != NULL) {
      CkAssert (curLevelLocal > 0);
      nextActiveBucket = curNodeLocal->startBucket;
    }
    // mark skipped (non-active) buckets as done
    for ( ; currentBucket < nextActiveBucket; currentBucket++) {
      bucketReqs[currentBucket].finished = 1;
      finishBucket(currentBucket);
    }
    */
  }

#else 
  while(i<_yieldPeriod && currentBucket<numBuckets){
    startNextBucket();
    currentBucket++;
    i++;
  }
#endif

  if (currentBucket<numBuckets) {
    thisProxy[thisIndex].nextBucket(msg);
  } else {
    delete msg;
  }
}

void TreePiece::calculateGravityLocal() {
  doAllBuckets();
}

#ifdef CELL
void cellSPE_ewald(void *data) {
  CellEwaldRequest *cgr = (CellEwaldRequest *)data;
  //CkPrintf("cellSPE_ewald %d\n", cgr->firstBucket);
  int i;
  free_aligned(cgr->roData);
  int offset = (cgr->numActiveData + 3) & ~0x3;
  for (i=0; i<cgr->numActiveData; ++i) {
    // copy the forces calculated to the particle's data
    GravityParticle *dest = cgr->particles[i];
    //CkPrintf("cellSPE_single part %d: %p, %f %f %f\n",i,dest,cr->activeData[i].treeAcceleration.x,cr->activeData[i].treeAcceleration.y,cr->activeData[i].treeAcceleration.z);
    dest->treeAcceleration.x = dest->treeAcceleration.x + cgr->woData[i+offset];
    dest->treeAcceleration.y = dest->treeAcceleration.y + cgr->woData[i+2*offset];
    dest->treeAcceleration.z = dest->treeAcceleration.z + cgr->woData[i+3*offset];
    dest->potential += cgr->woData[i];
  }
  for (i=cgr->firstBucket; i<=cgr->lastBucket; ++i) {
    cgr->tp->bucketReqs[i].finished = 1;
    cgr->tp->finishBucket(i);
  }
  free_aligned(cgr->woData);
  delete cgr->particles;
  workRequestOut --;
  delete cgr;
  cellEnableComputation();
}
#endif

void TreePiece::calculateEwald(dummyMsg *msg) {
  unsigned int i=0;
  while (i<_yieldPeriod && ewaldCurrentBucket < numBuckets
#ifdef CELL
	 && workRequestOut < CELLEWALDTHREASHOLD
#endif
	 ) {
#ifdef CELL_EWALD
    int activePart=0, indexActivePart=0;
    for (int k=bucketList[ewaldCurrentBucket]->firstParticle; k<=bucketList[ewaldCurrentBucket]->lastParticle; ++k) {
      if (myParticles[k].rung >= activeRung) activePart++;
    }
    GravityParticle **partList = new GravityParticle*[activePart];
    int outputSize = ROUNDUP_128(4*sizeof(cellSPEtype)*(activePart+3));
    int inputSize = ROUNDUP_128(sizeof(CellEwaldContainer)+nEwhLoop*sizeof(CellEWT)+3*sizeof(cellSPEtype)*(activePart+3));
    cellSPEtype *output = (cellSPEtype*)malloc_aligned(outputSize, 128);
    CellEwaldContainer *input = (CellEwaldContainer*)malloc_aligned(inputSize, 128);
    cellSPEtype *positionX = (cellSPEtype*)(((char*)input)+sizeof(CellEwaldContainer));
    cellSPEtype *positionY = (cellSPEtype*)(((char*)positionX)+((activePart+3)>>2)*(4*sizeof(cellSPEtype)));
    cellSPEtype *positionZ = (cellSPEtype*)(((char*)positionY)+((activePart+3)>>2)*(4*sizeof(cellSPEtype)));
    CellEWT *ewtIn = (CellEWT*)(((char*)positionZ)+((activePart+3)>>2)*(4*sizeof(cellSPEtype)));
    CellEwaldRequest *cr = new CellEwaldRequest(output, activePart, input, partList, this, ewaldCurrentBucket, ewaldCurrentBucket);
    for (int k=bucketList[ewaldCurrentBucket]->firstParticle; k<=bucketList[ewaldCurrentBucket]->lastParticle; ++k) {
      if (myParticles[k].rung >= activeRung) {
	positionX[indexActivePart] = myParticles[k].position.x;
	positionY[indexActivePart] = myParticles[k].position.y;
	positionZ[indexActivePart] = myParticles[k].position.z;
	partList[indexActivePart++] = &myParticles[k];
      }
    }
    input->rootMoments = root->moments;
    input->fEwCut = fEwCut;
    input->fPeriod = fPeriod.x;
    input->numPart = activePart;
    input->nReps = nReplicas;
    input->nEwhLoop = nEwhLoop;
    for (int k=0; k<nEwhLoop; ++k) {
      ewtIn[k] = ewt[k];
    }
    sendWorkRequest (3, NULL, 0, input, inputSize, output, outputSize, (void*)cr, 0, cellSPE_ewald, NULL);
    workRequestOut ++;
#else
    BucketEwald(bucketList[ewaldCurrentBucket], nReplicas, fEwCut);
    
    bucketReqs[ewaldCurrentBucket].finished = 1;
    finishBucket(ewaldCurrentBucket);
#endif
    
    ewaldCurrentBucket++;
    i++;
  }

  if (ewaldCurrentBucket<numBuckets) {
#ifdef CELL
    if (workRequestOut < CELLEWALDTHREASHOLD)
#endif
      thisProxy[thisIndex].calculateEwald(msg);
#ifdef CELL
    else
      ewaldMessages.insertAtEnd(CellComputation(thisProxy[thisIndex], msg));
#endif
  } else {
    delete msg;
  }
}

#if INTERLIST_VER > 0
void TreePiece::initNodeStatus(GenericTreeNode *node){
      
  GenericTreeNode *child;
  node->visitedR=false;
 
  NodeType childType;
 
  if(node->getType()==Bucket)
    return;
  
  for(unsigned int j = 0; j < node->numChildren(); ++j) {
    child = node->getChildren(j);
    CkAssert (child != NULL);
    childType = child->getType();
    if(!(childType == NonLocal || childType == NonLocalBucket || childType == Cached || childType == CachedBucket || childType==Empty || childType==CachedEmpty)){
      initNodeStatus(child);
    }
  }
}

#ifdef CELL
void cellSPE_callback(void *data) {
  //CkPrintf("cellSPE_callback\n");
  CellGroupRequest *cgr = (CellGroupRequest *)data;
  cgr->tp->bucketReqs[cgr->bucket].numAdditionalRequests --;
  cgr->tp->finishBucket(cgr->bucket);
  delete cgr->particles;
  delete cgr;
}

void cellSPE_single(void *data) {
  CellRequest *cr = (CellRequest *)data;
  free_aligned(cr->roData);
  for (int i=0; i<cr->numActiveData; ++i) {
    // copy the forces calculated to the particle's data
    GravityParticle *dest = cr->particles[i];
    //CkPrintf("cellSPE_single part %d: %p, %f %f %f\n",i,dest,cr->activeData[i].treeAcceleration.x,cr->activeData[i].treeAcceleration.y,cr->activeData[i].treeAcceleration.z);
    dest->treeAcceleration = dest->treeAcceleration + cr->activeData[i].treeAcceleration;
    dest->potential += cr->activeData[i].potential;
    if (cr->activeData[i].dtGrav > dest->dtGrav) {
      dest->dtGrav = cr->activeData[i].dtGrav;
    }
  }
  free_aligned(cr->activeData);
  workRequestOut --;
  delete cr;
  cellEnableComputation();
}
#endif

#define CELLBUFFERSIZE 16*1024
void TreePiece::calculateForceLocalBucket(int bucketIndex){
    int cellListIter;
    int partListIter;

    /*for (int i=1; i<=myNumParticles; ++i) {
      if (myParticles[i].treeAcceleration.x != 0 ||
	  myParticles[i].treeAcceleration.y != 0 ||
	  myParticles[i].treeAcceleration.z != 0)
	CkPrintf("AAARRRGGGHHH (%d)!!! Particle %d (%d) on TP %d has acc: %f %f %f\n",bucketIndex,i,myParticles[i].iOrder,thisIndex,myParticles[i].treeAcceleration.x,myParticles[i].treeAcceleration.y,myParticles[i].treeAcceleration.z);
    }*/
    
    if (bucketList[bucketIndex]->rungs >= activeRung) {
#ifdef CELL
      //enableTrace();
      // Combine all the computation in a request to be sent to the cell SPE
      int activePart=0, indexActivePart=0;
      for (int k=bucketList[bucketIndex]->firstParticle; k<=bucketList[bucketIndex]->lastParticle; ++k) {
        if (myParticles[k].rung >= activeRung) activePart++;
      }
      int activePartDataSize = ROUNDUP_128((activePart+3)*sizeof(CellGravityParticle));
      CellGravityParticle *activePartData = (CellGravityParticle*)malloc_aligned(activePartDataSize,128);
      GravityParticle **partList = new GravityParticle*[activePart];
      CellGroupRequest *cgr = new CellGroupRequest(this, bucketIndex, partList);
      WRGroupHandle wrh = createWRGroup(cgr, cellSPE_callback);
      for (int k=bucketList[bucketIndex]->firstParticle; k<=bucketList[bucketIndex]->lastParticle; ++k) {
        if (myParticles[k].rung >= activeRung) {
          activePartData[indexActivePart] = myParticles[k];
	  //CkPrintf("[%d] particle %d: %d on bucket %d, acc: %f %f %f\n",thisIndex,k,myParticles[k].iOrder,bucketIndex,activePartData[indexActivePart].treeAcceleration.x,activePartData[indexActivePart].treeAcceleration.y,activePartData[indexActivePart].treeAcceleration.z);
	  //CkPrintf("[%d] partList %d: particle %d (%p)\n",thisIndex,indexActivePart,k,&myParticles[k]);
          partList[indexActivePart++] = &myParticles[k];
        }
      }
      /*int numPart=0, numNodes=0;
      for(int k=0;k<=curLevelLocal;k++) {
        numNodes += cellListLocal[k].length();
        for(int kk=0; kk<particleListLocal[k].length(); kk++) {
          numPart += particleListLocal[k][kk].numParticles;
        }
      }*/
      int particlesPerRequest = (CELLBUFFERSIZE - 2*sizeof(int)) / sizeof(CellExternalGravityParticle);
      int nodesPerRequest = (CELLBUFFERSIZE - 2*sizeof(int)) / sizeof(CellMultipoleMoments);
      CellContainer *particleContainer = (CellContainer*)malloc_aligned(CELLBUFFERSIZE,128);
      CellExternalGravityParticle *particleData = (CellExternalGravityParticle*)&particleContainer->data;
      CellContainer *nodesContainer = (CellContainer*)malloc_aligned(CELLBUFFERSIZE,128);
      CellMultipoleMoments *nodesData = (CellMultipoleMoments*)&nodesContainer->data;
      int indexNodes=0, indexPart=0;
#endif
      //BucketGravityRequest& req = bucketReqs[bucketIndex];
      for(int k=0;k<=curLevelLocal;k++){
        int computed=0;
        double startTimer = CmiWallTimer();
#ifdef HPM_COUNTER
        hpmStart(1,"node force");
#endif
        for(cellListIter=0;cellListIter<cellListLocal[k].length();cellListIter++){
          OffsetNode tmp = cellListLocal[k][cellListIter];
#if COSMO_DEBUG > 1
          bucketcheckList[bucketIndex].insert(tmp->getKey());
          combineKeys(tmp->getKey(),bucketIndex);
#endif
#ifdef CELL_NODE
          nodesData[indexNodes] = tmp.node->moments;
          Vector3D<double> tmpoffsetID = decodeOffset(tmp.offsetID);
          nodesData[indexNodes].cm += tmpoffsetID;
          indexNodes++;
          if (indexNodes == nodesPerRequest) {
            // send off request
            void *activeData = malloc_aligned(activePartDataSize,128);
            memcpy(activeData, activePartData, activePart*sizeof(CellGravityParticle));
            CellRequest *userData = new CellRequest((CellGravityParticle*)activeData, activePart, nodesContainer, partList, this);
	    nodesContainer->numInt = activePart;
	    nodesContainer->numExt = indexNodes;
	    //CkPrintf("[%d] sending request 1 %p+%d, %p+%d\n",thisIndex,activeData, activePartDataSize, nodesContainer, CELLBUFFERSIZE);
            sendWorkRequest (1, activeData, activePartDataSize, nodesContainer, CELLBUFFERSIZE, NULL, 0,
                            (void*)userData, WORK_REQUEST_FLAGS_BOTH_CALLBACKS, cellSPE_single, wrh);
	    workRequestOut ++;
            nodeInterLocal += activePart * indexNodes;
	    nodesContainer = (CellContainer*)malloc_aligned(CELLBUFFERSIZE,128);
            nodesData = (CellMultipoleMoments*)&nodesContainer->data;
            indexNodes = 0;
          }
#else
          computed = nodeBucketForce(tmp.node, bucketList[bucketIndex], myParticles,
                                     decodeOffset(tmp.offsetID), activeRung);
#endif
        }
#ifdef CELL
        if (indexNodes > 0 && k==curLevelLocal) {
#ifdef CELL_NODE
          void *activeData = malloc_aligned(activePartDataSize,128);
          memcpy(activeData, activePartData, activePart*sizeof(CellGravityParticle));
          CellRequest *userData = new CellRequest((CellGravityParticle*)activeData, activePart, nodesContainer, partList, this);
	  nodesContainer->numInt = activePart;
	  nodesContainer->numExt = indexNodes;
          //CkPrintf("[%d] sending request 2 %p+%d, %p+%d\n",thisIndex,activeData, activePartDataSize, nodesContainer, ROUNDUP_128(indexNodes*sizeof(CellMultipoleMoments)));
          sendWorkRequest (1, activeData, activePartDataSize, nodesContainer, ROUNDUP_128(indexNodes*sizeof(CellMultipoleMoments)+2*sizeof(int)),
                           NULL, 0, (void*)userData, WORK_REQUEST_FLAGS_BOTH_CALLBACKS, cellSPE_single, wrh);
	  workRequestOut ++;
          nodeInterLocal += activePart * indexNodes;
#endif
        } else if (k==curLevelLocal) {
          free_aligned(nodesContainer);
        }
#endif
#ifdef HPM_COUNTER
        hpmStop(1);
#endif
        nodeInterLocal += cellListLocal[k].length() * computed;
        
        LocalPartInfo pinfo;
        double newTimer = CmiWallTimer();
        traceUserBracketEvent(nodeForceUE, startTimer, newTimer);
#ifdef HPM_COUNTER
    hpmStart(2,"particle force");
#endif
        for(partListIter=0;partListIter<particleListLocal[k].length();partListIter++){
          pinfo = particleListLocal[k][partListIter];
#if COSMO_DEBUG > 1
          bucketcheckList[bucketIndex].insert((pinfo.nd)->getKey());
          combineKeys((pinfo.nd)->getKey(),bucketIndex);
#endif
          for(int j = 0; j < pinfo.numParticles; ++j){
#ifdef CELL_PART
            particleData[indexPart] = pinfo.particles[j];
            particleData[indexPart].position += pinfo.offset;
            indexPart++;
            if (indexPart == particlesPerRequest) {
              // send off request
              void *activeData = malloc_aligned(activePartDataSize,128);
              memcpy(activeData, activePartData, activePart*sizeof(CellGravityParticle));
              CellRequest *userData = new CellRequest((CellGravityParticle*)activeData, activePart, particleContainer, partList, this);
	      particleContainer->numInt = activePart;
	      particleContainer->numExt = indexPart;
              //CkPrintf("[%d] sending request 3 %p+%d, %p+%d\n",thisIndex,activeData, activePartDataSize, particleContainer, CELLBUFFERSIZE);
              sendWorkRequest (2, activeData, activePartDataSize, particleContainer, CELLBUFFERSIZE, NULL, 0,
                              (void*)userData, WORK_REQUEST_FLAGS_BOTH_CALLBACKS, cellSPE_single, wrh);
	      workRequestOut ++;
	      particleInterLocal += activePart * indexPart;
	      particleContainer = (CellContainer*)malloc_aligned(CELLBUFFERSIZE,128);
              particleData = (CellExternalGravityParticle*)&particleContainer->data;
              indexPart = 0;
            }
#else
            computed = partBucketForce(&pinfo.particles[j], bucketList[bucketIndex], myParticles, pinfo.offset, activeRung);
#endif
          }
          particleInterLocal += pinfo.numParticles * computed;
        }
#ifdef CELL
        if (indexPart > 0 && k==curLevelLocal) {
#ifdef CELL_PART
          //void *activeData = malloc_aligned(activePartDataSize,128);
          //memcpy(activeData, activePartData, activePart*sizeof(GravityParticle));
          CellRequest *userData = new CellRequest(activePartData, activePart, particleContainer, partList, this);
	  particleContainer->numInt = activePart;
	  particleContainer->numExt = indexPart;
          //CkPrintf("[%d] sending request 4 %p+%d, %p+%d (%d int %d ext)\n",thisIndex,activePartData, activePartDataSize, particleContainer, ROUNDUP_128(indexPart*sizeof(CellExternalGravityParticle)),activePart,indexPart);
          sendWorkRequest (2, activePartData, activePartDataSize, particleContainer, ROUNDUP_128(indexPart*sizeof(CellExternalGravityParticle)+2*sizeof(int)),
                           NULL, 0, (void*)userData, WORK_REQUEST_FLAGS_BOTH_CALLBACKS, cellSPE_single, wrh);
	  workRequestOut ++;
          particleInterLocal += activePart * indexPart;
#endif
        } else if (k==curLevelLocal) {
          free_aligned(activePartData);
          free_aligned(particleContainer);
        }
#endif
#ifdef HPM_COUNTER
    hpmStop(2);
#endif
#ifdef COSMO_EVENTS
        traceUserBracketEvent(partForceUE, newTimer, CmiWallTimer());
#endif
      }
#ifdef CELL
      // Now all the requests have been made
      completeWRGroup(wrh);
      OffloadAPIProgress();
#endif
      /*if(bEwald) {
        BucketEwald(bucketList[bucketIndex], nReplicas, fEwCut);
      }*/
    }

#ifndef CELL
    //bucketReqs[bucketIndex].numAdditionalRequests --;
    finishBucket(bucketIndex);
#endif
}

void TreePiece::calculateForceRemoteBucket(int bucketIndex, int chunk){
    
  int cellListIter;
  int partListIter;

  if (bucketList[bucketIndex]->rungs >= activeRung) {
    //BucketGravityRequest& req = bucketReqs[bucketIndex];
    for(int k=0;k<=curLevelRemote;k++){
      int computed;
      double startTimer = CmiWallTimer();
#ifdef HPM_COUNTER
      hpmStart(1,"node force");
#endif
      for(cellListIter=0;cellListIter<cellList[k].length();cellListIter++){
        OffsetNode tmp= cellList[k][cellListIter];
#if COSMO_DEBUG > 1
        bucketcheckList[bucketIndex].insert(tmp.node->getKey());
        combineKeys(tmp.node->getKey(),bucketIndex);
#endif
        //computed = nodeBucketForce(tmp.node, bucketList[bucketIndex], myParticles,
        //                           decodeOffset(tmp.offsetID), activeRung);
      }
#ifdef HPM_COUNTER
      hpmStop(1);
#endif
      nodeInterRemote[chunk] += cellList[k].length() * computed;
      
      double newTimer = CmiWallTimer();
      traceUserBracketEvent(nodeForceUE, startTimer, newTimer);
#ifdef HPM_COUNTER
    hpmStart(2,"particle force");
#endif
      for(partListIter=0;partListIter<particleList[k].length();partListIter++){
        RemotePartInfo pinfo = particleList[k][partListIter];
#if COSMO_DEBUG > 1
        bucketcheckList[bucketIndex].insert((pinfo.nd)->getKey());
        combineKeys((pinfo.nd)->getKey(),bucketIndex);
#endif
        for(int j = 0; j < pinfo.numParticles; ++j){
          //computed = partBucketForce(&pinfo.particles[j], bucketList[bucketIndex],
          //                           myParticles, pinfo.offset, activeRung);
        }
        particleInterRemote[chunk] += pinfo.numParticles * computed;
      }
#ifdef HPM_COUNTER
    hpmStop(2);
#endif
#ifdef COSMO_EVENTS
      traceUserBracketEvent(partForceUE, newTimer, CmiWallTimer());
#endif
    }
  }

  //bucketReqs[bucketIndex].numAdditionalRequests--;
  finishBucket(bucketIndex);
  //remainingChunk[nChunk] -= bucketList[bucketIndex]->particleCount;

}
/*
void TreePiece::calculateForceRemoteBucket(int bucketIndex){
    
  GenericTreeNode* node = bucketList[bucketIndex];
  
  for(unsigned int j = node->firstParticle; j <= node->lastParticle; ++j) {
    //for(int k=0;k<=curLevelRemote;k++){
      //if(cellList[k].length()>0 || particleList[k].length()>0)
        listPartForce(cellList,particleList,myParticles[j],curLevelRemote);
    //}
  }
  //bucketReqs[bucketIndex].numAdditionalRequests--;
  finishBucket(bucketIndex);

}
*/
#endif

void TreePiece::calculateGravityRemote(ComputeChunkMsg *msg) {
  unsigned int i=0;
  // cache internal tree: start directly asking the CacheManager
  GenericTreeNode *chunkRoot = dm->chunkRootToNode(prefetchRoots[msg->chunkNum]);
  //GenericTreeNode *chunkRoot = keyToNode(prefetchRoots[msg->chunkNum]);
  
    // OK to pass bogus arguments because we don't expect to miss on this anyway (see CkAssert(chunkRoot) below.)
  if (chunkRoot == NULL) {
    chunkRoot = requestNode(thisIndex, prefetchRoots[msg->chunkNum], msg->chunkNum, -1, -78, true);
  }
  CkAssert(chunkRoot != NULL);
#if COSMO_PRINT > 0
  CkPrintf("[%d] Computing gravity remote for chunk %d with node %016llx\n",thisIndex,msg->chunkNum,chunkRoot->getKey());
#endif

#if INTERLIST_VER > 0

  nChunk = msg->chunkNum;

  NodeType childType;
  //Init for each chunk
  //Possible race condition here....bfore calling calGravRemote...setting some stuff
  if(currentRemoteBucket==0){
    checkList[0].length()=0;
    for(i=0;i<=myTreeLevels;i++){
      cellList[i].length()=0;
      particleList[i].length()=0;
    }
    i=0;

    if(nChunk>0)
      initNodeStatus(root);
  }
  
  while (i<_yieldPeriod && currentRemoteBucket < numBuckets) {

    if(currentRemoteBucket==0){
      curNodeRemote=root;
      curLevelRemote=0;
    }
    preWalkRemoteInterTree(chunkRoot,true);
    
    //Everything calculates forces with buckets
    
    CkAssert(checkList[curLevelRemote].length()==0);
    
    if(myCheckListEmpty && curNodeRemote->getType()!=Bucket){
	
      GenericTreeNode *tmpNode;
  
      int startBucket=curNodeRemote->startBucket;
      int lastBucket;
      int k;
  
      for(k=startBucket+1;k<numBuckets;k++){
        tmpNode = bucketList[k];
        if(tmpNode->lastParticle>curNodeRemote->lastParticle)
          break;
      }
      lastBucket=k-1;
    
      for(k=startBucket;k<=lastBucket;k++){
        calculateForceRemoteBucket(k, nChunk);
        //remainingChunk[nChunk] -= bucketList[k]->particleCount;
        currentRemoteBucket++;
        i++;
      }
      myCheckListEmpty=false;
    }
    else{
      CkAssert(curNodeRemote->getType()==Bucket);
      calculateForceRemoteBucket(curNodeRemote->bucketListIndex, nChunk);
      
      //remainingChunk[nChunk] -= bucketList[curNodeRemote->bucketListIndex]->particleCount;
      currentRemoteBucket++;
      i++;
    }

    //Go over both the lists to calculate forces with one bucket
    
    if(currentRemoteBucket>=numBuckets)
      break;
    
    //Calculate starting Node for next tree walk
    GenericTreeNode *tmpNode=curNodeRemote;
    int flag=0;
    tmpNode->visitedR=true;
    while(tmpNode->parent != NULL){
      
      cellList[curLevelRemote].length()=0;
      particleList[curLevelRemote].length()=0;
      
      tmpNode=tmpNode->parent;
      curLevelRemote--;
      GenericTreeNode* childIterator;
      for(unsigned int j = 0; j < tmpNode->numChildren(); ++j) {
        childIterator = tmpNode->getChildren(j);
        CkAssert (childIterator != NULL);
        childType=childIterator->getType();
        if(childIterator->visitedR == false){
          if(childType == NonLocal || childType == Cached || childType == NonLocalBucket || childType == CachedBucket || childType==Empty || childType==CachedEmpty ){//|| childIterator->rungs < activeRung){
            childIterator->visitedR=true;
          }
          else{
            curNodeRemote=childIterator;
            curLevelRemote++;
            flag=1;
            break;
          }
        }
      }
      if(flag==0){
        tmpNode->visitedR=true;
      }
      if(flag==1){
        flag=0;
        break;
      }
    }
    /*
    int nextActiveBucket = numBuckets;
    if (tmpNode->parent != NULL) {
      CkAssert (curLevelRemote > 0);
      nextActiveBucket = curNodeRemote->startBucket;
    }
    // mark skipped (non-active) buckets as done
    for ( ; currentRemoteBucket < nextActiveBucket; currentRemoteBucket++) {
      finishBucket(currentRemoteBucket);
      //remainingChunk[msg->chunkNum] -= bucketList[currentRemoteBucket]->particleCount;
    }
    */
  }

#else

  sTopDown->init(sGravity, this);
  while (i<_yieldPeriod && currentRemoteBucket < numBuckets) {
#ifdef CHANGA_REFACTOR_WALKCHECK
    if(thisIndex == CHECK_INDEX && currentRemoteBucket == CHECK_BUCKET){
      CkPrintf("Starting remote walk\n");
    }
#endif
    sGravity->init(bucketList[currentRemoteBucket], activeRung, sRemote);
    //State *state = sGravity->getResumeState(currentRemoteBucket);
    int awi = remoteGravityAwi;

    //bucketReqs[currentRemoteBucket].numAdditionalRequests--;
    sRemoteGravityState->counterArrays[0][currentRemoteBucket]--;
    //CkPrintf("[%d], bucket %d has %d more numAdditionalRequests\n", sRemoteGravityState->counterArrays[0][currentRemoteBucket]);

    if (bucketList[currentRemoteBucket]->rungs >= activeRung) {
      for(int x = -nReplicas; x <= nReplicas; x++) {
	for(int y = -nReplicas; y <= nReplicas; y++) {
          for(int z = -nReplicas; z <= nReplicas; z++) {
            /*
            walkBucketRemoteTree(chunkRoot, msg->chunkNum,
                                 encodeOffset(currentRemoteBucket,x, y, z),
                                 true);
            */
#if CHANGA_REFACTOR_DEBUG > 1
            CkPrintf("[%d]: starting remote walk with chunk=%d, currentBucket=%d, (%d,%d,%d)\n", thisIndex, msg->chunkNum, currentRemoteBucket, x, y, z);
#endif
            sTopDown->walk(chunkRoot, sRemoteGravityState, msg->chunkNum,encodeOffset(currentRemoteBucket,x, y, z), awi);

          }
        }
      }
    }

    finishBucket(currentRemoteBucket);
    //remainingChunk[msg->chunkNum] -= bucketList[currentRemoteBucket]->particleCount;
    sRemoteGravityState->counterArrays[1][msg->chunkNum] -= bucketList[currentRemoteBucket]->particleCount;
    currentRemoteBucket++;
    i++;
  }

#endif

  if (currentRemoteBucket < numBuckets) {
    thisProxy[thisIndex].calculateGravityRemote(msg);
#if COSMO_PRINT > 0
    CkPrintf("{%d} sending self-message chunk %d, prio %d\n",thisIndex,msg->chunkNum,*(int*)CkPriorityPtr(msg));
#endif
  } else {
    currentRemoteBucket = 0;
    //CkAssert(remainingChunk[msg->chunkNum] >= 0);
    CkAssert(sRemoteGravityState->counterArrays[1][msg->chunkNum] >= 0);
    //if (remainingChunk[msg->chunkNum] == 0) {
    if (sRemoteGravityState->counterArrays[1][msg->chunkNum] == 0) {
      // we finished completely using this chunk, so we acknowledge the cache
      // if this is not true it means we had some hard misses
#ifdef COSMO_PRINT
      CkPrintf("[%d] Finished chunk %d\n",thisIndex,msg->chunkNum);
#endif
      streamingCache[CkMyPe()].finishedChunk(msg->chunkNum, nodeInterRemote[msg->chunkNum]+particleInterRemote[msg->chunkNum]);
      if (msg->chunkNum == numChunks-1) markWalkDone();
    }
#if COSMO_PRINT > 0
    CkPrintf("{%d} resetting message chunk %d, prio %d\n",thisIndex,msg->chunkNum,*(int*)CkPriorityPtr(msg));
#endif
    delete msg;
  }
}

#if INTERLIST_VER > 0

void TreePiece::preWalkRemoteInterTree(GenericTreeNode *chunkRoot, bool isRoot){

    //Start copying the checkList of previous level to the next level
    int level;
    GenericTreeNode *child;
    OffsetNode node;
    int flag=0;
    NodeType childType;

    while(1){
      level=curLevelRemote-1;

      prevListIter=0;
      checkList[curLevelRemote].length()=0;

      if(curNodeRemote!=root){
        if(checkList[level].length()!=0){
          node=checkList[level][0];
          prevListIter=1;
        }
        else
          node.node=NULL;
      }
      else{
        node.node=chunkRoot;
        if (root->rungs < activeRung) {
          root->visitedR=true;
          myCheckListEmpty=true;
          break;
        }
      }

      if(node.node!=NULL){
	  if(curNodeRemote==root) {
	    for(int x = -nReplicas; x <= nReplicas; x++) {
		for(int y = -nReplicas; y <= nReplicas; y++) {
		    for(int z = -nReplicas; z <= nReplicas; z++) {
			node.offsetID = encodeOffset(0, x, y, z);
			walkRemoteInterTree(node,true);
			}
		    }
		}
	      }
        else
          walkRemoteInterTree(node,false);
      }
      else{
        myCheckListEmpty=true;
        curNodeRemote = curNodeRemote->parent;
        curLevelRemote--;
        break;
      }
      
      //Loop breaking condition
      if(curNodeRemote->getType()==Bucket)
        break;
      
      for(int i=0;i<curNodeRemote->numChildren();i++){
        child = curNodeRemote->getChildren(i);
      	CkAssert (child != NULL);
        childType = child->getType();
        if(child->visitedR==false){
          if(childType == NonLocal || childType == Cached || childType == NonLocalBucket || childType == CachedBucket || childType==Empty || childType==CachedEmpty || child->rungs < activeRung){
            child->visitedR=true;
          }
          else{
            flag=1;
            break;
          }
        }
      }
      if(flag==1){
        curNodeRemote=child;
        curLevelRemote++;
        flag=0;
      }
      else{
        CkPrintf("Exceptional case\n");
        CkAssert(curNodeRemote == root);
        curNodeRemote->visitedR=true;
        break;
      }
    }

}

#endif

#if INTERLIST_VER > 0

void TreePiece::walkRemoteInterTree(OffsetNode node, bool isRoot) {

  cachedWalkInterTree(node);
  
}

#endif

void TreePiece::walkBucketRemoteTree(GenericTreeNode *node, int chunk,
				     int reqID, bool isRoot) {

    Vector3D<double> offset = decodeOffset(reqID);
    int reqIDlist = decodeReqID(reqID);
    
  // The order in which the following if are checked is very important for correctness
  if(node->getType() == Bucket || node->getType() == Internal || node->getType() == Empty) {
#if COSMO_PRINT > 0
    CkPrintf("[%d] bucket %d: internal %llx\n",thisIndex,reqIDlist,node->getKey());
#endif
    return;
  }
  else if(!openCriterionBucket(node, bucketList[reqIDlist], offset, localIndex)) {
#if COSMO_STATS > 0
    numOpenCriterionCalls++;
#endif
    if (isRoot && (node->getType() == Cached || node->getType() == CachedBucket || node->getType() == CachedEmpty)) {
      GenericTreeNode *nd = dm->getRoot();
      //GenericTreeNode *nd = root;
      while (nd != NULL) {
	// if one of the ancestors of this node is not opened, then we don't
	// want to duplicate the computation
#if COSMO_STATS > 0
	numOpenCriterionCalls++;
#endif
	if (!openCriterionBucket(nd, bucketList[reqIDlist], offset, localIndex)) {
#if COSMO_PRINT > 0
	  CkPrintf("[%d] bucket %d: not opened %llx, found node %llx\n",thisIndex,reqIDlist,node->getKey(),nd->getKey());
#endif
	  return;
	}
	int which = nd->whichChild(node->getKey());
	GenericTreeNode *ndp = nd;
	nd = nd->getChildren(which);
#if COSMO_PRINT > 0
	if (nd!=NULL) CkPrintf("[%d] search: got %llx from %llx, %d's child\n",thisIndex,nd->getKey(),ndp->getKey(),which);
#endif
      }
#if COSMO_PRINT > 0
      CkPrintf("[%d] bucket %d: not opened %llx, called cache\n",thisIndex,reqIDlist,node->getKey());
#endif
      cachedWalkBucketTree(node, chunk, reqID);
    } else {
#if COSMO_PRINT > 0
      CkPrintf("[%d] bucket %d: not opened %llx\n",thisIndex,reqID,node->getKey());
#endif
      return;
    }
  } else if (node->getType() != Boundary) {
    // means it is some kind of NonLocal or Cached
#if COSMO_STATS > 0
    numOpenCriterionCalls++;
#endif
#if COSMO_PRINT > 0
      CkPrintf("[%d] bucket %d: calling cached walk with %llx\n",thisIndex,reqIDlist,node->getKey());
#endif
    if (isRoot) {
      GenericTreeNode *nd = dm->getRoot();
      //GenericTreeNode *nd = root;
      while (nd != NULL && nd->getKey() != node->getKey()) {
	// if one of the ancestors of this node is not opened, then we don't
	// want to duplicate the computation
#if COSMO_STATS > 0
	numOpenCriterionCalls++;
#endif
	if (!openCriterionBucket(nd, bucketList[reqIDlist], offset, localIndex)) {
#if COSMO_PRINT > 0
	  CkPrintf("[%d] bucket %d: not opened %llx, found node %llx\n",thisIndex,reqID,node->getKey(),nd->getKey());
#endif
	  return;
	}
	int which = nd->whichChild(node->getKey());
	GenericTreeNode *ndp = nd;
	nd = nd->getChildren(which);
#if COSMO_PRINT > 0
	if (nd!=NULL) CkPrintf("[%d] search: got %llx from %llx, %d's child\n",thisIndex,nd->getKey(),ndp->getKey(),which);
#endif
      }
    }
    cachedWalkBucketTree(node, chunk, reqID);
  } else {
    // here the node is Boundary
#if COSMO_STATS > 0
    nodesOpenedRemote++;
#endif
#if COSMO_STATS > 0
    numOpenCriterionCalls++;
#endif
    GenericTreeNode* childIterator;
    for(unsigned int i = 0; i < node->numChildren(); ++i) {
      childIterator = node->getChildren(i);
      CkAssert (childIterator != NULL);
      walkBucketRemoteTree(childIterator, chunk, reqID, false);
    }
  }
}

// Start tree walk and gravity calculation

void TreePiece::startIteration(int am, // the active mask for multistepping
			       const CkCallback& cb) {

  callback = cb;
  activeRung = am;

  int oldNumChunks = numChunks;
  dm->getChunks(numChunks, prefetchRoots);
  CkArrayIndexMax idxMax = CkArrayIndex1D(thisIndex);
  streamingCache[CkMyPe()].cacheSync(numChunks, idxMax, localIndex);
  //numChunks = n;
  //prefetchRoots = k;

  if (oldNumChunks != numChunks && remainingChunk != NULL) {
    // reallocate remaining chunk to the new size
    delete[] remainingChunk;
    remainingChunk = NULL;
    delete[] nodeInterRemote;
    delete[] particleInterRemote;
  }
  
  if (remainingChunk == NULL) {
    remainingChunk = new int[numChunks];
    nodeInterRemote = new u_int64_t[numChunks];
    particleInterRemote = new u_int64_t[numChunks];
  }
#if COSMO_STATS > 0
  //myNumProxyCalls = 0;
  //myNumProxyCallsBack = 0;
  //myNumCellInteractions=myNumParticleInteractions=myNumMACChecks=0;
  //cachecellcount=0;
  nodesOpenedLocal = 0;
  nodesOpenedRemote = 0;
  numOpenCriterionCalls=0;
#endif
  nodeInterLocal = 0;
  for (int i=0; i<numChunks; ++i) {
    nodeInterRemote[i] = 0;
    particleInterRemote[i] = 0;
  }
  particleInterLocal = 0;
  nActive = 0;
  iterationNo++;

#if 0
  // @TODO we should be able to take this shortcut, but we need to
  // make sure other data is cleaned up.  In particular chunkAck in
  // the CacheManager needs to be cleared.  Perhaps call finishChunk()?
  if(root->rungs < activeRung) { // nothing to do
      if(verbosity >= 3) {
	  ckerr << "TreePiece " << thisIndex << ": no actives" << endl;
	  }
      contribute(0, 0, CkReduction::concat, callback);
      return;
      }
#endif

  //CkAssert(localCache != NULL);
  if(verbosity>1)
    CkPrintf("Node: %d, TreePiece %d: I have %d buckets\n", CkMyNode(),
    	     thisIndex,numBuckets);

  if (bucketReqs==NULL) bucketReqs = new BucketGravityRequest[numBuckets];
  
  currentBucket = 0;
  currentRemoteBucket = 0;
  ewaldCurrentBucket = 0;
  myNumParticlesPending = myNumParticles;
  started = true;

#if INTERLIST_VER > 0
    myCheckListEmpty=false;
    curLevelLocal=0;
    curNodeLocal=NULL;
    curLevelRemote=0;
    curNodeRemote=NULL;
    nChunk=-1;
#endif

  initBuckets();

#if INTERLIST_VER > 0
  //Initialize all the interaction and check lists with empty lists
  myTreeLevels++;
  //myTreeLevels++;
  CkAssert(myTreeLevels>0);
  //if(listMigrated==true || iterationNo==1){
    /*for(int i=0;i<=myTreeLevels;i++){
      CkVec<GenericTreeNode *> *clist = new CkVec<GenericTreeNode *>();
      CkVec<PartInfo> *plist = new CkVec<PartInfo>();
      CkVec<GenericTreeNode *> *ichlist = new CkVec<GenericTreeNode *>();
      CkVec<GenericTreeNode *> *echlist = new CkVec<GenericTreeNode *>();
      CkVec<GenericTreeNode *> *clistl = new CkVec<GenericTreeNode *>();
      CkVec<PartInfo> *plistl = new CkVec<PartInfo>();
      CkVec<GenericTreeNode *> *chlistl = new CkVec<GenericTreeNode *>();
  
      cellList.push_back(*clist);
      //delete clist; 
      particleList.push_back(*plist);
      checkList.push_back(*echlist);
      cellListLocal.push_back(*clistl);
      particleListLocal.push_back(*plistl);
      checkListLocal.push_back(*chlistl);
    }*/
    //listMigrated=false;
  //}
  cellList.resize(myTreeLevels+1);
  particleList.resize(myTreeLevels+1);
  checkList.resize(myTreeLevels+1);
  cellListLocal.resize(myTreeLevels+1);
  particleListLocal.resize(myTreeLevels+1);
  checkListLocal.resize(myTreeLevels+1);
#endif
  
  //for (int i=0; i<numChunks; ++i) remainingChunk[i] = myNumParticles;

  //BucketGravityRequest req0(1);
  //BucketGravityRequest req1(1);
  switch(domainDecomposition){
    case Oct_dec:
    case ORB_dec:
      //Prefetch Roots for Oct
      prefetchReq[0].reset();
      for (unsigned int i=1; i<=myNumParticles; ++i) {
        if (myParticles[i].rung >= activeRung) {
          prefetchReq[0].grow(myParticles[i].position);
        }
      }
      break;
    default:
      //Prefetch Roots for SFC
      prefetchReq[0].reset();
      for (unsigned int i=1; i<=myNumParticles; ++i) {
	  // set to first active particle
        if (myParticles[i].rung >= activeRung) {
          prefetchReq[0].grow(myParticles[i].position);
	  break;
	}
      }
      prefetchReq[1].reset();
      for (unsigned int i=myNumParticles; i>=1; --i) {
	  // set to last active particle
        if (myParticles[i].rung >= activeRung) {
	    prefetchReq[1].grow(myParticles[i].position);
	    break;
	}
      }
      
      break;
  }

  int first, last;
  GenericTreeNode *child = dm->chunkRootToNode(prefetchRoots[0]);
  //GenericTreeNode *child = keyToNode(prefetchRoots[0]);

#if CHANGA_REFACTOR_DEBUG > 0
  CkPrintf("Beginning prefetch\n");
#endif

  // Create objects that are reused by all buckets
  sTopDown = new TopDownTreeWalk;
  sGravity = new GravityCompute;
  sLocal = new LocalOpt;
  sRemote = new RemoteOpt;
  sPrefetch = new PrefetchCompute;
  sPref = new PrefetchOpt;

  // just so that getNewState has an opt type to go by
  sGravity->init((void *)this, activeRung, sLocal);
  // state for local gravity, includes book-keeping variables
  sLocalGravityState = sGravity->getNewState();

  // just so that getNewState has an opt type to go by
  sGravity->init((void *)this, activeRung, sRemote);
  // state for remote gravity, includes book-keeping variables
  sRemoteGravityState = sGravity->getNewState();
  // remainingChunk[]
  sRemoteGravityState->counterArrays[1].reserve(numChunks);
  for(int i = 0; i < numChunks; i++) sRemoteGravityState->counterArrays[1][i] = myNumParticles;

  // numAdditionalRequests[]
  sRemoteGravityState->counterArrays[0].reserve(numBuckets);
  // local component of numAdditionalRequests[]
  sLocalGravityState->counterArrays[0].reserve(numBuckets);

  for(int i = 0; i < numBuckets; i++){
    sRemoteGravityState->counterArrays[0][i] = numChunks;
    sLocalGravityState->counterArrays[0][i] = 1;
  }

  sPrefetch->init((void *)this, activeRung, sPref);
  sTopDown->init(sPrefetch, this);
  sPrefetchState = sPrefetch->getNewState();
  //prefetchWaiting = (2*nReplicas + 1)*(2*nReplicas + 1)*(2*nReplicas + 1);
  // instead of prefetchWaiting, we count through state->counters[0]
  sPrefetchState->counterArrays[0][0] = (2*nReplicas + 1)*(2*nReplicas + 1)*(2*nReplicas + 1);
  currentPrefetch = 0;

  completedActiveWalks = 0;
  activeWalks.reserve(2);
  prefetchAwi = addActiveWalk(sTopDown,sPrefetch,sPref,sPrefetchState);
  remoteGravityAwi = addActiveWalk(sTopDown,sGravity,sRemote,sRemoteGravityState);


  for(int x = -nReplicas; x <= nReplicas; x++) {
      for(int y = -nReplicas; y <= nReplicas; y++) {
	  for(int z = -nReplicas; z <= nReplicas; z++) {
	      if (child == NULL) {
		  nodeOwnership(prefetchRoots[0], first, last);
		  child = requestNode((first+last)>>1, prefetchRoots[0], 0,
				      encodeOffset(0, x, y, z), prefetchAwi, true);
		  }
	      if (child != NULL) {
		  // prefetch(child, encodeOffset(0, x, y, z));
#if CHANGA_REFACTOR_DEBUG > 1
                  CkPrintf("[%d] starting prefetch walk with currentPrefetch=%d, numPrefetchReq=%d (%d,%d,%d)\n", thisIndex, currentPrefetch, numPrefetchReq, x,y,z);
#endif
                  sTopDown->walk(child, sPrefetchState, currentPrefetch, encodeOffset(0,x,y,z), prefetchAwi);
		  }
	      }
	  }
      }

#if CHANGA_REFACTOR_DEBUG > 0
  CkPrintf("[%d]sending message to commence local gravity calculation\n", thisIndex);
#endif
  thisProxy[thisIndex].calculateGravityLocal();
  if (bEwald) thisProxy[thisIndex].EwaldInit();

  //if(verbosity > 1) {
      int piecesPerPe = numTreePieces/CmiNumPes();
      if(thisIndex % piecesPerPe == 0)
	CkPrintf("[%d]: CmiMaxMemoryUsage: %f M\n", CmiMyPe(),
		 (float)CmiMaxMemoryUsage()/(1 << 20));
 //}
}

void TreePiece::prefetch(GenericTreeNode *node, int offsetID) {
    CkAbort("prefetch: Shouldn't be in this part of the code\n");
    Vector3D<double> offset = decodeOffset(offsetID);
  ///@TODO: all the code that does the prefetching and the chunking
  CkAssert(node->getType() != Invalid);
  //printf("{%d-%d} prefetch %016llx in chunk %d\n",CkMyPe(),thisIndex,node->getKey(),currentPrefetch);

  if (_prefetch) {
    bool needOpened = false;
    for (unsigned int i=0; i<numPrefetchReq; ++i) {
	// Construct testNode for bounds check.
	// XXX Softening is not considered in the prefetch.
	BinaryTreeNode testNode;
	testNode.boundingBox = prefetchReq[i];
      if (openCriterionBucket(node, &testNode, offset, localIndex)) {
	needOpened = true;
	break;
      }
    }
    if (node->getType() != Internal && node->getType() != Bucket && needOpened) {
      if(node->getType() == CachedBucket || node->getType() == NonLocalBucket) {
	// Sending the request for all the particles at one go, instead of one by one
	if (/*requestParticles(node->getKey(),currentPrefetch,node->remoteIndex,node->firstParticle,node->lastParticle,-1,true) == NULL*/ true) {
          CkAbort("Shouldn't be in this part of the code\n");
	  prefetchWaiting ++;
	}
      } else if (node->getType() != CachedEmpty && node->getType() != Empty) {
	// Here the type is Cached, Boundary, Internal, NonLocal, which means the
	// node in the global tree has children (it is not a leaf), so we iterate
	// over them. If we get a NULL node, then we missed the cache and we request
	// it
	
	// Warning, since the cache returns nodes with pointers to other chare
	// elements trees, we could be actually traversing the tree of another chare
	// in this processor.
	
	// Use cachedWalkBucketTree() as callback
	GenericTreeNode *child;
	for (unsigned int i=0; i<node->numChildren(); ++i) {
	  child = node->getChildren(i); //requestNode(node->remoteIndex, node->getChildKey(i), req);
	  prefetchWaiting ++;
	  
	  if (child) {
	    prefetch(child, offsetID);
	  } else { //missed the cache 
	    // jetley child = requestNode(node->remoteIndex, node->getChildKey(i), currentPrefetch, offsetID, true);
	    if (child) { // means that node was on a local TreePiece
	      prefetch(child, offsetID);
	    }
	  }
	}
      }
    }
  }

  prefetchWaiting --;
  //if (prefetchWaiting==0) ckout <<"Waiting for "<<prefetchWaiting<<" more prefetches"<<endl;

  // this means we don't have any more nodes waiting for prefetching
  if (prefetchWaiting == 0) {
    startRemoteChunk();
  }
}

void TreePiece::prefetch(ExternalGravityParticle *node) {
  CkAbort("prefetch (CM): shouldn't be in this part of code\n");
  prefetchWaiting --;
  if (prefetchWaiting == 0) {
    startRemoteChunk();
  }
}

void TreePiece::startRemoteChunk() {
  ComputeChunkMsg *msg = new (8*sizeof(int)) ComputeChunkMsg(currentPrefetch);
  *(int*)CkPriorityPtr(msg) = numTreePieces * currentPrefetch + thisIndex + 1;
  CkSetQueueing(msg, CK_QUEUEING_IFIFO);

#if CHANGA_REFACTOR_DEBUG > 0
  CkPrintf("[%d] sending message to commence remote gravity\n", thisIndex);
#endif
  thisProxy[thisIndex].calculateGravityRemote(msg);
  
  // start prefetching next chunk
  if (++currentPrefetch < numChunks) {
    int first, last;
    
    // Nothing needs to be changed for this chunk - 
    // the prefetchReqs and their number remains the same
    // We only need to reassociate the tree walk with the 
    // prefetch compute object and the prefetch object wiht 
    // the prefetch opt object
    sTopDown->reassoc(sPrefetch);
    // prefetch walk isn't associated with any particular bucket
    // but the entire treepiece
    // this method invocation does nothing. indeed, nothing
    // needs to be done because sPrefetch is always associated with
    // sPref
    sPrefetch->reassoc(0,activeRung,sPref);
    
    // instead of the statement below, we have a sPrefetchState 
    // State *state = sPrefetch->getResumeState(-1);

    // instead of prefetchWaiting, we count through state->counters[0]
    //prefetchWaiting = (2*nReplicas + 1)*(2*nReplicas + 1)*(2*nReplicas + 1);
    sPrefetchState->counterArrays[0][0] = (2*nReplicas + 1)*(2*nReplicas + 1)*(2*nReplicas + 1);
    GenericTreeNode *child = dm->chunkRootToNode(prefetchRoots[currentPrefetch]);
    //GenericTreeNode *child = keyToNode(prefetchRoots[currentPrefetch]);
    for(int x = -nReplicas; x <= nReplicas; x++) {
	for(int y = -nReplicas; y <= nReplicas; y++) {
	    for(int z = -nReplicas; z <= nReplicas; z++) {
		if (child == NULL) {
		    nodeOwnership(prefetchRoots[currentPrefetch], first, last);
		    child = requestNode((first+last)>>1,
					prefetchRoots[currentPrefetch],
					currentPrefetch,
					encodeOffset(0, x, y, z), true,
                                        prefetchAwi);
		}
		if (child != NULL) {
		    //prefetch(child, encodeOffset(0, x, y, z));
#if CHANGA_REFACTOR_DEBUG > 1
                    CkPrintf("[%d] starting prefetch walk with currentPrefetch=%d, numPrefetchReq=%d (%d,%d,%d)\n", thisIndex, 
                                                                                                                    currentPrefetch, numPrefetchReq,
                                                                                                                    x,y,z);
#endif
                    sTopDown->walk(child, sPrefetchState, currentPrefetch, encodeOffset(0,x,y,z), prefetchAwi);

		}
            }
	}
    }

  }
}

/*
void TreePiece::startlb(CkCallback &cb){
  callback = cb;
  if(verbosity > 1)
    CkPrintf("[%d] TreePiece %d calling AtSync()\n",CkMyPe(),thisIndex);
  localCache->revokePresence(thisIndex);
  AtSync();
}
*/
  // jetley - contribute your centroid. AtSync is now called by the load balancer (broadcast) when it has
  // all centroids. 
void TreePiece::startlb(CkCallback &cb, int activeRung){
  callback = cb;
  if(verbosity > 1)
    CkPrintf("[%d] TreePiece %d calling AtSync()\n",CkMyPe(),thisIndex);
  // AtSync();
  
  if(!proxyValid || !proxySet){              // jetley
    proxyValid = true;
#if COSMO_MCLB > 1 
    CkPrintf("[%d : %d] !proxyValid, calling doAtSync()\n", CkMyPe(), thisIndex);
#endif 
    prevLARung = activeRung;
    doAtSync();
  }
  else{ 
    unsigned int numActiveParticles, i;
   
    if(activeRung == 0){
      numActiveParticles = myNumParticles;
    }
    else{
      for(numActiveParticles = 0, i = 0; i < myNumParticles; i++)
        if(myParticles[i+1].rung >= activeRung)
          numActiveParticles++;
    }
    LDObjHandle myHandle = myRec->getLdHandle();
    TaggedVector3D tv(savedCentroid, myHandle, numActiveParticles, myNumParticles, activeRung, prevLARung);

    // CkCallback(int ep, int whichProc, CkGroupID &gid)
    CkCallback cbk(CkIndex_MetisCosmoLB::receiveCentroids(NULL), 0, proxy); 
#if COSMO_MCLB > 1 
    CkPrintf("[%d : %d] proxyValid, contributing value (%f,%f,%f, %u,%u,%u : %d)\n", CkMyPe(), thisIndex, tv.vec.x, tv.vec.y, tv.vec.z, tv.numActiveParticles, tv.myNumParticles, tv.activeRung, tv.tag);
#endif
    contribute(sizeof(TaggedVector3D), (char *)&tv, CkReduction::concat, cbk);
    if(thisIndex == 0)
      CkPrintf("Changing prevLARung from %d to %d\n", prevLARung, activeRung);
    prevLARung = activeRung;
    //contribute(sizeof(TaggedVector3D), &tv, CkReduction::set, cbk);
  }
}

void TreePiece::doAtSync(){
  if(verbosity > 1)
      CkPrintf("[%d] TreePiece %d calling AtSync() at %g\n",CkMyPe(),thisIndex, CkWallTimer());
  AtSync();
}

void TreePiece::ResumeFromSync(){
  if(verbosity > 1)
    CkPrintf("[%d] TreePiece %d in ResumefromSync\n",CkMyPe(),thisIndex);
  contribute(0, 0, CkReduction::concat, callback);
}

GenericTreeNode *TreePiece::keyToNode(const Tree::NodeKey k) {
  NodeLookupType::iterator iter = nodeLookupTable.find(k);
  if (iter != nodeLookupTable.end()) return iter->second;
  else return NULL;
}

const GenericTreeNode *TreePiece::lookupNode(Tree::NodeKey key){
  return keyToNode(key);
};

const GravityParticle *TreePiece::lookupParticles(int begin) {
  return &myParticles[begin];
}

/*
 * For cached version we have 2 walks: one for on processor and one
 * that hits the cache. This does the local computation
 * When remote data is needed we go to the second version.
 */

#if INTERLIST_VER > 0

//Builds interaction lists for all levels of myTree starting from curNodeLocal to a bucket
//Returns on reaching the bucket of myTree
void TreePiece::preWalkInterTree(){

    //Start copying the checkList of previous level to the next level
    int level;
    GenericTreeNode *child;
    OffsetNode node;
    int flag=0;
    NodeType childType;
    
    while(1){
      level=curLevelLocal-1;
      checkListLocal[curLevelLocal].length()=0;
      prevListIterLocal=0;
      
      if(curNodeLocal!=root){
	assert(level >= 0);
        if(checkListLocal[level].length()!=0){
          node=checkListLocal[level][0];
          prevListIterLocal=1;
        }
        else node.node=NULL;
      }
      else{
        if (root->rungs < activeRung) {
          root->visitedL=true;
          myLocalCheckListEmpty=true;
          break;
        }

        GenericTreeNode *nd;
        for(int i=0;i<numChunks;i++){
          nd = dm->chunkRootToNode(prefetchRoots[i]);
          //nd = keyToNode(prefetchRoots[i]);
          if(nd!=NULL) {
	      OffsetNode ond;
	      ond.node = nd;
	    for(int x = -nReplicas; x <= nReplicas; x++) {
		for(int y = -nReplicas; y <= nReplicas; y++) {
		    for(int z = -nReplicas; z <= nReplicas; z++) {
			ond.offsetID = encodeOffset(0, x, y, z);
			undecidedListLocal.enq(ond);
			}
		    }
		}
	      }
        }
        CkAssert(!undecidedListLocal.isEmpty());
        node=undecidedListLocal.deq();
      }
    	
      //Walks the local tree for my current node
      if(node.node!=NULL){
        walkInterTree(node);
      }
      else{
        myLocalCheckListEmpty=true;
        curNodeLocal=curNodeLocal->parent;
        curLevelLocal--;
        break;
      }
      
      CkAssert(undecidedListLocal.isEmpty());
    
      //Loop breaking condition
      if(curNodeLocal->getType()==Bucket)
        break;
      
      //Finds my node on the next level which is not yet visited
      //This node must contain at least one particle currently active
      for(int i=0;i<curNodeLocal->numChildren();i++){
        child = curNodeLocal->getChildren(i);
      	CkAssert (child != NULL);
        childType = child->getType();
        if(child->visitedL==false){
          if(childType == NonLocal || childType == Cached || childType == NonLocalBucket || childType == CachedBucket || childType==Empty || childType==CachedEmpty || child->rungs < activeRung){
            child->visitedL=true;
          }
          else{
            flag=1;
            break;
          }
        }
      }
      if(flag==1){
        curNodeLocal=child;
        curLevelLocal++;
        flag=0;
      }
      else{
        CkPrintf("Exceptional case\n");
        CkAssert(curNodeLocal == root);
        curNodeLocal->visitedL=true;
        break;
      }
    }
}
#endif

#if INTERLIST_VER > 0

//Walk the local tree and build interaction list for curNodeLocal, which belongs to myTree
void TreePiece::walkInterTree(OffsetNode node) {
  
  GenericTreeNode *myNode = curNodeLocal;
  int level=curLevelLocal;
  NodeType nodeType = node.node->getType();
  
  int openValue=-2;

  if(nodeType == NonLocal || nodeType == NonLocalBucket) {
   /* DISABLED: this part of the walk is triggered directly by the CacheManager and prefetching
   */
  } else if(nodeType==Empty){
#ifdef CACHE_TREE 
    if (thisProxy[node.node->remoteIndex].ckLocal()!=NULL) {
#else
    if (node.node->remoteIndex==thisIndex) {
#endif
#if COSMO_STATS > 0
      numOpenCriterionCalls++;
#endif
#if COSMO_DEBUG > 1
      cellListLocal[level].push_back(node);
#endif
    }
    else{}
  } else if((openValue=openCriterionNode(node.node, myNode,
					 decodeOffset(node.offsetID), localIndex))==0) {
#if COSMO_STATS > 0
    numOpenCriterionCalls++;
#endif
    if(nodeType!=Boundary)
      cellListLocal[level].push_back(node);
  }  else if (nodeType != Empty) {
#if COSMO_STATS > 0
    numOpenCriterionCalls++;
#endif
    /*#if COSMO_STATS > 0
      if(myNode->getType()!=Bucket)
      numOpenCriterionCalls++;
      #endif*/
    CkAssert(openValue!=-2);
    // here the node can be Internal or Boundary or Bucket
    if(myNode->getType()==Bucket || openValue==1){
      if(nodeType==Bucket){
        TreePiece::LocalPartInfo pinfo;
        //pinfo.particles = &myParticles[node.node->firstParticle];
        pinfo.particles = node.node->particlePointer;
#if COSMO_DEBUG>1
        pinfo.nd=node.node;
#endif
        pinfo.numParticles = node.node->lastParticle - node.node->firstParticle + 1;
	pinfo.offset = decodeOffset(node.offsetID);
        particleListLocal[level].push_back(pinfo);
      }
      else{
        GenericTreeNode* childIterator;
        for(unsigned int i = 0; i < node.node->numChildren(); ++i) {
          childIterator = node.node->getChildren(i);
          if(childIterator) {
	      OffsetNode ond;
	      ond.node = childIterator;
	      ond.offsetID = node.offsetID;
	      undecidedListLocal.enq(ond);
	      }
        }
      }
    }
    else{
      checkListLocal[level].push_back(node);
    }
  }
  /*else if (nodeType == Empty) {
#if COSMO_STATS > 0
    numOpenCriterionCalls++;
#endif
#if COSMO_DEBUG > 1
  	cellListLocal[level].push_back(node);
#endif
  }*/
 
  //Call myself if there are still nodes in previous level checklist
  //or my undecided list
  if(myNode!=root){
    if(prevListIterLocal>0 && prevListIterLocal<checkListLocal[level-1].length()){
      prevListIterLocal++;
      walkInterTree(checkListLocal[level-1][prevListIterLocal-1]);
    }
    else if(!undecidedListLocal.isEmpty())
      walkInterTree(undecidedListLocal.deq());
    else
      return;
  }
  else{
    if(!undecidedListLocal.isEmpty())
      walkInterTree(undecidedListLocal.deq());
    else
      return;
  }
}

#endif


// Walk a node evalutating its force on a bucket
//
// @param node Node to be walked
// @param reqID request ID which encodes bucket to be worked on and
// a replica offset if we are using periodic boundary conditions
void TreePiece::walkBucketTree(GenericTreeNode* node, int reqID) {
    Vector3D<double> offset = decodeOffset(reqID);
    int reqIDlist = decodeReqID(reqID);
#if COSMO_STATS > 0
  myNumMACChecks++;
#endif
#if COSMO_PRINT > 0
  if (node->getKey() < numChunks) CkPrintf("[%d] walk: checking %016llx\n",thisIndex,node->getKey());
#endif
#if COSMO_STATS > 0
  numOpenCriterionCalls++;
#endif
  GenericTreeNode* reqnode = bucketList[reqIDlist];
  if(!openCriterionBucket(node, reqnode, offset, localIndex)) {
      // Node is well separated; evaluate the force
#if COSMO_STATS > 1
    MultipoleMoments m = node->moments;	
    for(int i = reqnode->firstParticle; i <= reqnode->lastParticle; ++i)
      myParticles[i].intcellmass += m.totalMass;
#endif
#if COSMO_PRINT > 1
    CkPrintf("[%d] walk bucket %s -> node %s\n",thisIndex,keyBits(bucketList[reqIDlist]->getKey(),63).c_str(),keyBits(node->getKey(),63).c_str());
#endif
#if COSMO_DEBUG > 1
    bucketcheckList[reqIDlist].insert(node->getKey());
    combineKeys(node->getKey(),reqIDlist);
#endif
#if COSMO_PRINT > 0
    if (node->getKey() < numChunks) CkPrintf("[%d] walk: computing %016llx\n",thisIndex,node->getKey());
#endif
#ifdef COSMO_EVENTS
    double startTimer = CmiWallTimer();
#endif
#ifdef HPM_COUNTER
    hpmStart(1,"node force");
#endif
    int computed = nodeBucketForce(node, reqnode, myParticles, offset, activeRung);
#ifdef HPM_COUNTER
    hpmStop(1);
#endif
#ifdef COSMO_EVENTS
    traceUserBracketEvent(nodeForceUE, startTimer, CmiWallTimer());
#endif
    nodeInterLocal += computed;
  } else if(node->getType() == Bucket) {
    int computed;
    for(int i = node->firstParticle; i <= node->lastParticle; ++i) {
#if COSMO_STATS > 1
      for(int j = reqnode->firstParticle; j <= reqnode->lastParticle; ++j) {
        //myParticles[j].intpartmass += myParticles[i].mass;
        myParticles[j].intpartmass += node->particlePointer[i-node->firstParticle].mass;
      }
#endif
#if COSMO_PRINT > 1
      //CkPrintf("[%d] walk bucket %s -> part %016llx\n",thisIndex,keyBits(reqnode->getKey(),63).c_str(),myParticles[i].key);
      CkPrintf("[%d] walk bucket %s -> part %016llx\n",thisIndex,keyBits(reqnode->getKey(),63).c_str(),node->particlePointer[i-node->firstParticle].key);
#endif
#ifdef COSMO_EVENTS
      double startTimer = CmiWallTimer();
#endif
#ifdef HPM_COUNTER
    hpmStart(2,"particle force");
#endif
      computed = partBucketForce(&node->particlePointer[i-node->firstParticle], reqnode,
                                 myParticles, offset, activeRung);
#ifdef HPM_COUNTER
    hpmStop(2);
#endif
#ifdef COSMO_EVENTS
      traceUserBracketEvent(partForceUE, startTimer, CmiWallTimer());
#endif
    }
    particleInterLocal += node->particleCount * computed;
#if COSMO_DEBUG > 1
    bucketcheckList[reqIDlist].insert(node->getKey());
    combineKeys(node->getKey(),reqIDlist);
#endif
  } else if (node->getType() == NonLocal || node->getType() == NonLocalBucket) {
    /* DISABLED: this part of the walk is triggered directly by the CacheManager and prefetching

    // Use cachedWalkBucketTree() as callback
    GenericTreeNode *pnode = requestNode(node->remoteIndex, node->getKey(), req);
    if(pnode) {
      cachedWalkBucketTree(pnode, req);
    }
    */
  } else if (node->getType() != Empty) {
    // here the node can be Internal or Boundary
#if COSMO_STATS > 0
    nodesOpenedLocal++;
#endif
#if COSMO_PRINT > 0
    if (node->getKey() < numChunks) CkPrintf("[%d] walk: opening %016llx\n",thisIndex,node->getKey());
#endif
    GenericTreeNode* childIterator;
    for(unsigned int i = 0; i < node->numChildren(); ++i) {
      childIterator = node->getChildren(i);
      if(childIterator)
	walkBucketTree(childIterator, reqID);
    }
  }
}

 /*
 * Cached version of Tree walk. One characteristic of the tree used is that once
 * we go into cached data, we cannot come back to internal data anymore. Thus we
 * can safely distinguish between local computation done by walkBucketTree and
 * remote computation done by cachedWalkBucketTree.
 */

#if INTERLIST_VER > 0

void TreePiece::cachedWalkInterTree(OffsetNode node) {
#ifdef CMK_VERSION_BLUEGENE
  if ((forProgress+=3) > 200) {
    forProgress = 0;
#ifdef COSMO_EVENTS
    traceUserEvent(networkProgressUE);
#endif
    CmiNetworkProgress();
  }
#endif

  int chunk = nChunk;
  GenericTreeNode* myNode = curNodeRemote;
  int level = curLevelRemote;

  NodeType nodeType = node.node->getType();
  CkAssert(nodeType != Invalid);

  int openValue=-2;
  
    // Changed for CACHE TREE
#ifdef CACHE_TREE
  if(nodeType == Bucket || nodeType == Internal || (nodeType == Empty && thisProxy[node.node->remoteIndex].ckLocal()!=NULL)) {
  }
#else
  if(node.node->remoteIndex==thisIndex && (nodeType == Bucket || nodeType == Internal || nodeType == Empty)) {
  }
#endif
  else if(nodeType == CachedEmpty || nodeType == Empty) {
    //Currently, Empty node is being pushed back just to match Filippos nodeBucketCalls
    //I'll remove it later
#if COSMO_DEBUG > 1
    cellList[level].push_back(node);
#endif
#if COSMO_STATS > 0
    numOpenCriterionCalls++;
#endif
  }
  else if((openValue=openCriterionNode(node.node, myNode,
				       decodeOffset(node.offsetID), localIndex))==0) {
#if COSMO_STATS > 0
    numOpenCriterionCalls++;
#endif
    cellList[level].push_back(node);
  } else if(nodeType == CachedBucket || nodeType == Bucket || nodeType == NonLocalBucket) {
    /*
     * Sending the request for all the particles at one go, instead of one by one
     */
#ifdef CACHE_TREE
    CkAssert(nodeType!=Bucket);
#endif
#if COSMO_STATS > 0
    numOpenCriterionCalls++;
#endif
    if(myNode->getType()==Bucket){
      /*
      ExternalGravityParticle *part
	  = requestParticles(node.node->getKey(), chunk,
			     node.node->remoteIndex, node.node->firstParticle,
			     node.node->lastParticle,
			     reEncodeOffset(myNode->bucketListIndex,
					    node.offsetID));
          */
      CkAbort("Please refrain from using the interaction list version of ChaNGa for the time being. This part of the code is being rewritten\n");
      //PROBLEM: how to form req in this case
      if(part != NULL){
      	TreePiece::RemotePartInfo pinfo;
      	pinfo.particles = part;
#if COSMO_DEBUG>1
        pinfo.nd=node;
#endif
      	pinfo.numParticles = node.node->lastParticle - node.node->firstParticle + 1;
	pinfo.offset = decodeOffset(node.offsetID);
      	particleList[level].push_back(pinfo);
      } else {
      	//remainingChunk[chunk] += node.node->lastParticle - node.node->firstParticle + 1;
        /*#if COSMO_DEBUG > 1
        bucketReqs[myNode->bucketListIndex].requestedNodes.push_back(node.node->getKey());
        #endif*/
      }
    }
    else if(openValue==1){
      calculateForces(node,myNode,level,chunk);
    }
    else{
      checkList[level].push_back(node);
    }
/*#ifdef CACHE_TREE
  } else if(nodeType==Boundary) {
#else
  } else if(node.node->remoteIndex==thisIndex && nodeType==Boundary) {
#endif
#if COSMO_STATS > 0
  numOpenCriterionCalls++;
#endif
    if(openValue==1 || myNode->getType()==Bucket){
      // here the node is Boundary
#if COSMO_STATS > 0
      nodesOpenedRemote++;
#endif
		  //Boundary node removed from the interaction list...we dont calculate force with boundary
      GenericTreeNode* childIterator;
      for(unsigned int i = 0; i < node.node->numChildren(); ++i) {
        childIterator = node->getChildren(i);
        CkAssert (childIterator != NULL);
	OffsetNode child;
	child.node = childIterator;
	child.offsetID = node.offsetID;
        undecidedList.enq(child);
      }
    }
    else{
      //intListIter++;
      checkList[level].push_back(node);
    }*/
  } else if (nodeType != CachedEmpty && nodeType != Empty) {
    // Here the type is Cached, Boundary, Internal, NonLocal, which means the
    // node in the global tree has children (it is not a leaf), so we iterate
    // over them. If we get a NULL node, then we missed the cache and we request
    // it

    // Warning, since the cache returns nodes with pointers to other chare
    // elements trees, we could be actually traversing the tree of another chare
    // in this processor.
#if COSMO_STATS > 0
    numOpenCriterionCalls++;
#endif

    // Use cachedWalkBucketTree() as callback
    if(myNode->getType()==Bucket){
      OffsetNode child;
      child.offsetID = node.offsetID;
      //BucketGravityRequest& req = bucketReqs[myNode->bucketListIndex];
      for (unsigned int i=0; i<node.node->numChildren(); ++i) {
	child.node = node.node->getChildren(i); //requestNode(node->remoteIndex, node->getChildKey(i), req);
      	if (child.node) {
          undecidedList.enq(child);
      	} else { //missed the cache
	  //PROBLEM: how to construct req
	  child.node = requestNode(node.node->remoteIndex,
				   node.node->getChildKey(i),
				   chunk, reEncodeOffset(myNode->bucketListIndex, node.offsetID));
	  if (child.node) { // means that node was on a local TreePiece
            undecidedList.enq(child);
	  } else { // we completely missed the cache, we will be called back
	    //remainingChunk[chunk] ++;
	  }
      	}
      }
    }
    else if(openValue==1){
      calculateForcesNode(node,myNode,level,chunk);
    }
    else{
      checkList[level].push_back(node);
    }
  }

  if(myNode!=root){
    if(prevListIter>0 && prevListIter<checkList[level-1].length()){
      prevListIter++;
      cachedWalkInterTree(checkList[level-1][prevListIter-1]);
    }
    else if(!undecidedList.isEmpty())
      cachedWalkInterTree(undecidedList.deq());
    else
      return;
  }
  else{
    if(!undecidedList.isEmpty())
      cachedWalkInterTree(undecidedList.deq());
    else
      return;
  }
}
#endif

#if INTERLIST_VER > 0
inline void TreePiece::calculateForces(OffsetNode node, GenericTreeNode *myNode,int level,int chunk){

  GenericTreeNode *tmpNode;
  int startBucket=myNode->startBucket;
  int lastBucket;
  int i;
  
  for(i=startBucket+1;i<numBuckets;i++){
    tmpNode = bucketList[i];
    if(tmpNode->lastParticle>myNode->lastParticle)
      break;
  }
  lastBucket=i-1;
  
  int test=0;
  
  for(i=startBucket;i<=lastBucket;i++){
      CkAbort("Please refrain from using the interaction list version of ChaNGa for the time being. This part of the code is being rewritten\n");
      /*
    ExternalGravityParticle *part
	= requestParticles(node.node->getKey(), chunk, node.node->remoteIndex,
			   node.node->firstParticle, node.node->lastParticle,
			   reEncodeOffset(i, node.offsetID));
        */
    if(part != NULL){
      CkAssert(test==0);
      TreePiece::RemotePartInfo pinfo;
      pinfo.particles = part;
#if COSMO_DEBUG>1
      pinfo.nd=node.node;
#endif
      pinfo.numParticles = node.node->lastParticle - node.node->firstParticle + 1;
      pinfo.offset = decodeOffset(node.offsetID);
      particleList[level].push_back(pinfo);
      break;
    } else {
      //remainingChunk[chunk] += node.node->lastParticle
	  - node.node->firstParticle + 1;
      /*#if COSMO_DEBUG > 1
      bucketReqs[i].requestedNodes.push_back(node->getKey());
      #endif*/
    }
    test++;
  }
}

inline void TreePiece::calculateForcesNode(OffsetNode node,
					   GenericTreeNode *myNode,
					   int level,int chunk){

  GenericTreeNode *tmpNode;
  int startBucket=myNode->startBucket;
  int k;
  
	/*for(k=startBucket+1;k<numBuckets;k++){
		tmpNode = bucketList[k];
		if(tmpNode->lastParticle>myNode->lastParticle)
			break;
  }
  lastBucket=k-1;*/
  
  OffsetNode child;
  child.offsetID = node.offsetID;
  for (unsigned int i=0; i<node.node->numChildren(); ++i) {
    child.node = node.node->getChildren(i); //requestNode(node->remoteIndex, node->getChildKey(i), req);
    k = startBucket;
    
    if (child.node) {
      undecidedList.enq(child);
    } else { //missed the cache
      //PROBLEM: how to construct req
      child.node = requestNode(node.node->remoteIndex,
			       node.node->getChildKey(i), chunk,
			       reEncodeOffset(k, node.offsetID));
      if (child.node) { // means that node was on a local TreePiece
        undecidedList.enq(child);
      } else { // we completely missed the cache, we will be called back
        //We have to queue the requests for all the buckets, all buckets will be called back later
        
        //remainingChunk[chunk] ++;
        for(k=startBucket+1;k<numBuckets;k++){
          tmpNode = bucketList[k];
          if(tmpNode->lastParticle>myNode->lastParticle) break;
          child.node = requestNode(node.node->remoteIndex,
				   node.node->getChildKey(i), chunk,
				   reEncodeOffset(k, child.offsetID));
          CkAssert(child.node==NULL);
          //remainingChunk[chunk] ++;
        }
      }
    }
  }
}

#endif

void TreePiece::cachedWalkBucketTree(GenericTreeNode* node, int chunk, int reqID) {
    int reqIDlist = decodeReqID(reqID);
    Vector3D<double> offset = decodeOffset(reqID);
  
  CkAbort("cachedWalkBucketTree: shouldn't be in this part of code\n");
  GenericTreeNode *reqnode = bucketList[reqIDlist];
#if COSMO_STATS > 0
  myNumMACChecks++;
#endif
#if COSMO_PRINT > 1
  CkPrintf("[%d] b=%d cachedWalkBucketTree called for %s with node %s of type %s (additional=%d)\n",thisIndex,reqIDlist,keyBits(bucketList[reqIDlist]->getKey(),63).c_str(),keyBits(node->getKey(),63).c_str(),getColor(node).c_str(),bucketReqs[reqID].numAdditionalRequests);
#endif
		
  CkAssert(node->getType() != Invalid);
	
#if COSMO_STATS > 0
  numOpenCriterionCalls++;
#endif
  if(!openCriterionBucket(node, reqnode, offset, localIndex)) {
#if COSMO_STATS > 1
    MultipoleMoments m = node->moments;
    for(int i = reqnode->firstParticle; i <= reqnode->lastParticle; ++i)
      myParticles[i].extcellmass += m.totalMass;
#endif
#if COSMO_PRINT > 1
  CkPrintf("[%d] cachedwalk bucket %s -> node %s\n",thisIndex,keyBits(reqnode->getKey(),63).c_str(),keyBits(node->getKey(),63).c_str());
#endif
#if COSMO_DEBUG > 1
  bucketcheckList[reqIDlist].insert(node->getKey());
  combineKeys(node->getKey(),reqIDlist);
#endif
#ifdef COSMO_EVENTS
    double startTimer = CmiWallTimer();
#endif
#ifdef HPM_COUNTER
    hpmStart(1,"node force");
#endif
    int computed = nodeBucketForce(node, reqnode, myParticles, offset, activeRung);
#ifdef HPM_COUNTER
    hpmStop(1);
#endif
#ifdef COSMO_EVENTS
    traceUserBracketEvent(nodeForceUE, startTimer, CmiWallTimer());
#endif
    nodeInterRemote[chunk] += computed;
  } else if(node->getType() == CachedBucket || node->getType() == Bucket || node->getType() == NonLocalBucket) {
    /*
     * Sending the request for all the particles at one go, instead of one by one
     */
    //printf("{%d-%d} cachewalk requests for %016llx in chunk %d\n",CkMyPe(),thisIndex,node->getKey(),chunk);
    CkAbort("Shouldn't be in this part of the code.\n");
    //ExternalGravityParticle *part = requestParticles(node->getKey(),chunk,node->remoteIndex,node->firstParticle,node->lastParticle,reqID);
    ExternalGravityParticle *part;
    if(part != NULL){
      int computed;
      for(int i = node->firstParticle; i <= node->lastParticle; ++i) {
#if COSMO_STATS > 1
        for(int j = reqnode->firstParticle; j <= reqnode->lastParticle; ++j) {
          myParticles[j].extpartmass += myParticles[i].mass;
        }
#endif
#if COSMO_PRINT > 1
        CkPrintf("[%d] cachedwalk bucket %s -> part %016llx\n",thisIndex,keyBits(reqnode->getKey(),63).c_str(),part[i-node->firstParticle].key);
#endif
#ifdef COSMO_EVENTS
        double startTimer = CmiWallTimer();
#endif
#ifdef HPM_COUNTER
    hpmStart(2,"particle force");
#endif
        computed = partBucketForce(&part[i-node->firstParticle], reqnode, myParticles,
                                   offset, activeRung);
#ifdef HPM_COUNTER
    hpmStop(2);
#endif
#ifdef COSMO_EVENTS
        traceUserBracketEvent(partForceUE, startTimer, CmiWallTimer());
#endif
      }
      particleInterRemote[chunk] += node->particleCount * computed;
#if COSMO_DEBUG > 1
      bucketcheckList[reqIDlist].insert(node->getKey());
      combineKeys(node->getKey(),reqIDlist);
#endif
    } else {
      //remainingChunk[chunk] += node->lastParticle - node->firstParticle + 1;
    }
  } else if (node->getType() != CachedEmpty && node->getType() != Empty) {
    // Here the type is Cached, Boundary, Internal, NonLocal, which means the
    // node in the global tree has children (it is not a leaf), so we iterate
    // over them. If we get a NULL node, then we missed the cache and we request
    // it

    // Warning, since the cache returns nodes with pointers to other chare
    // elements trees, we could be actually traversing the tree of another chare
    // in this processor.

#if COSMO_STATS > 0
    nodesOpenedRemote++;
#endif
    // Use cachedWalkBucketTree() as callback
    GenericTreeNode *child;
    for (unsigned int i=0; i<node->numChildren(); ++i) {
      child = node->getChildren(i); //requestNode(node->remoteIndex, node->getChildKey(i), req);
      if (child) {
	cachedWalkBucketTree(child, chunk, reqID);
      } else { //missed the cache
	// jetley child = requestNode(node->remoteIndex, node->getChildKey(i), chunk, reqID);
	if (child) { // means that node was on a local TreePiece
	  cachedWalkBucketTree(child, chunk, reqID);
	} else { // we completely missed the cache, we will be called back
	  //remainingChunk[chunk] ++;
	}
      }
    }
  }
}

#if 0
GenericTreeNode* TreePiece::requestNode(int remoteIndex, Tree::NodeKey key, int chunk, int reqID, bool isPrefetch) {

  // Call proxy on remote node
  CkAssert(remoteIndex < (int) numTreePieces);
  CkAssert(chunk < numChunks);
  //in the current form it is possible   
  //   assert(remoteIndex != thisIndex);
  if(_cache){
    CkAssert(localCache != NULL);
    /*
    if(localCache == NULL){
      localCache = cacheManagerProxy.ckLocalBranch();
    }
    */
#if COSMO_PRINT > 1
    CkPrintf("[%d] b=%d requesting node %s to %d for %s (additional=%d)\n",thisIndex,reqID,keyBits(key,63).c_str(),remoteIndex,keyBits(bucketList[reqID]->getKey(),63).c_str(),
            //bucketReqs[reqID].numAdditionalRequests);
#endif
    GenericTreeNode *res=localCache->requestNode(thisIndex,remoteIndex,chunk,key,reqID,isPrefetch);
    if(!res){
	if(!isPrefetch)
	    //bucketReqs[decodeReqID(reqID)].numAdditionalRequests++;
      //#if COSMO_STATS > 0
      //myNumProxyCalls++;
      //#endif
    }
    return res;
  }
  else{	
    CkAbort("Non cached version not anymore supported, feel free to fix it!");
    return NULL;
    /*
    //req.numAdditionalRequests++;
    streamingProxy[remoteIndex].fillRequestNode(thisIndex, key, req.identifier);
    myNumProxyCalls++;
    return NULL;
    */
  }
}
#endif

GenericTreeNode* TreePiece::requestNode(int remoteIndex, Tree::NodeKey key, int chunk, int reqID, int awi, bool isPrefetch) {

  CkAssert(remoteIndex < (int) numTreePieces);
  CkAssert(chunk < numChunks);
  
  if(_cache){
    //CkAssert(localCache != NULL);
#if COSMO_PRINT > 1
    CkPrintf("[%d] b=%d requesting node %s to %d for %s (additional=%d)\n",thisIndex,reqID,keyBits(key,63).c_str(),remoteIndex,keyBits(bucketList[reqID]->getKey(),63).c_str(),
            sRemoteGravityState->counterArrays[0][decodeReqID(reqID)] + sLocalGravityState->counterArrays[0][decodeReqID(reqID)]);
            //bucketReqs[reqID].numAdditionalRequests);
#endif
    //GenericTreeNode
    //*res=localCache->requestNode(thisIndex,remoteIndex,chunk,key,reqID,isPrefetch,awi);
    CProxyElement_ArrayElement thisElement(thisProxy[thisIndex]);
    CkCacheRequestorData request(thisElement, &EntryTypeGravityNode::callback, (((CmiUInt8)awi)<<32)+reqID);
    CkArrayIndexMax remIdx = CkArrayIndex1D(remoteIndex);
    GenericTreeNode *res = (GenericTreeNode *) streamingCache[CkMyPe()].requestData(key,remIdx,chunk,&gravityNodeEntry,request);
    //    if(!res){
	    //if(!isPrefetch)
	    //bucketReqs[decodeReqID(reqID)].numAdditionalRequests++;
	        //sRemoteGravityState->counterArrays[0][decodeReqID(reqID)]++;
        //}
    return res;
  }
  else{	
    CkAbort("Non cached version not anymore supported, feel free to fix it!");
  }
}

void TreePiece::receiveNode(GenericTreeNode &node, int chunk, unsigned int reqID)
{
    int reqIDlist = decodeReqID(reqID);
    
    CkAbort("receiveNode: shouldn't be in this part of code\n");
#if COSMO_PRINT > 1
  //CkPrintf("[%d] b=%d, receiveNode, additional=%d\n",thisIndex,reqID,bucketReqs[reqID].numAdditionalRequests);
#endif
  //bucketReqs[reqIDlist].numAdditionalRequests--;
  //remainingChunk[chunk] --;
  assert(node.getType() != Invalid);
  if(node.getType() != Empty)	{ // Node could be NULL
    assert((int) node.remoteIndex != thisIndex);
    cachedWalkBucketTree(&node, chunk, reqID);
  }else{
#if COSMO_DEBUG > 1
    bucketcheckList[reqIDlist].insert(node.getKey());
    combineKeys(node.getKey(),reqIDlist);
#endif
  }
    
  finishBucket(reqIDlist);
  //CkAssert(remainingChunk[chunk] >= 0);
  if (0/*remainingChunk[chunk] == 0*/) {
#ifdef COSMO_PRINT
    CkPrintf("[%d] Finished chunk %d with a node\n",thisIndex,chunk);
#endif
    streamingCache[CkMyPe()].finishedChunk(chunk, nodeInterRemote[chunk]+particleInterRemote[chunk]);
    if (chunk == numChunks-1) markWalkDone();
  }
}

void TreePiece::receiveNode_inline(GenericTreeNode &node, int chunk, unsigned int reqID){
        receiveNode(node,chunk,reqID);
}

ExternalGravityParticle *TreePiece::requestParticles(Tree::NodeKey key,int chunk,int remoteIndex,int begin,int end,int reqID, int awi, bool isPrefetch) {
  if (_cache) {
    //CkAssert(localCache != NULL);
    //ExternalGravityParticle *p = localCache->requestParticles(thisIndex,chunk,key,remoteIndex,begin,end,reqID,awi,isPrefetch);
    CProxyElement_ArrayElement thisElement(thisProxy[thisIndex]);
    CkCacheRequestorData request(thisElement, &EntryTypeGravityParticle::callback, (((u_int64_t)awi)<<32)+reqID);
    CkArrayIndexMax remIdx = CkArrayIndex1D(remoteIndex);
    CkCacheKey ckey = key<<1;
    CacheParticle *p = (CacheParticle *) streamingCache[CkMyPe()].requestData(ckey,remIdx,chunk,&gravityParticleEntry,request);
    if (p == NULL) {
#if COSMO_PRINT > 1
      CkPrintf("[%d] b=%d requestParticles: additional=%d\n",thisIndex,
	       decodeReqID(reqID),
               sRemoteGravityState->counterArrays[0][decodeReqID(reqID)] + sLocalGravityState->counterArrays[0][decodeReqID(reqID)]);
	       //bucketReqs[decodeReqID(reqID)].numAdditionalRequests);
#endif
      //      if(!isPrefetch) {
	      //  CkAssert(reqID >= 0);
      //          sRemoteGravityState->counterArrays[0][decodeReqID(reqID)] += end-begin+1;
      //}
      return NULL;
    }
    return p->part;
  } else {
    CkAbort("Non cached version not anymore supported, feel free to fix it!");
  }
};

ExternalGravityParticle *
TreePiece::requestSmoothParticles(Tree::NodeKey key,int chunk,int remoteIndex,
				  int begin,int end,int reqID, int awi,
				  bool isPrefetch) {
  if (_cache) {
    CProxyElement_ArrayElement thisElement(thisProxy[thisIndex]);
    CkCacheRequestorData request(thisElement, &EntryTypeSmoothParticle::callback, (((u_int64_t)awi)<<32)+reqID);
    CkArrayIndexMax remIdx = CkArrayIndex1D(remoteIndex);
    CkCacheKey ckey = key<<1;
    CacheParticle *p = (CacheParticle *) streamingCache[CkMyPe()].requestData(ckey,remIdx,chunk,&smoothParticleEntry,request);
    if (p == NULL) {
      return NULL;
    }
    return p->part;
  } else {
    CkAbort("Non cached version not anymore supported, feel free to fix it!");
  }
};

void TreePiece::receiveParticles(ExternalGravityParticle *part,int num,int chunk,
				 unsigned int reqID, Tree::NodeKey remoteBucketID)
{
    CkAbort("receiveParticles: shouldn't be in this part of code\n");
    Vector3D<double> offset = decodeOffset(reqID);
    int reqIDlist = decodeReqID(reqID);
  CkAssert(num > 0);
#if COSMO_PRINT > 1
  //CkPrintf("[%d] b=%d recvPart (additional=%d-%d)\n",thisIndex, reqIDlist,
	   //bucketReqs[reqIDlist].numAdditionalRequests,num);
#endif
  //bucketReqs[reqIDlist].numAdditionalRequests -= num;
  //remainingChunk[chunk] -= num;

  GenericTreeNode* reqnode = bucketList[reqIDlist];

  int computed;
  for(int i=0;i<num;i++){
#if COSMO_STATS > 1
    for(int j = reqnode->firstParticle; j <= reqnode->lastParticle; ++j) {
      myParticles[j].extpartmass += part[i].mass;
    }
#endif
#if COSMO_PRINT > 1
    CkPrintf("[%d] recvPart bucket %s -> part %016llx\n",thisIndex,keyBits(reqnode->getKey(),63).c_str(),part->key);
#endif
#ifdef COSMO_EVENTS
    double startTimer = CmiWallTimer();
#endif
#ifdef HPM_COUNTER
    hpmStart(2,"particle force");
#endif
    computed = partBucketForce(&part[i], reqnode, myParticles, offset, activeRung);
#ifdef HPM_COUNTER
    hpmStop(2);
#endif
#ifdef COSMO_EVENTS
    traceUserBracketEvent(partForceUE, startTimer, CmiWallTimer());
#endif
  }
  particleInterRemote[chunk] += computed * num;
#if COSMO_DEBUG > 1 || defined CHANGA_REFACTOR_WALKCHECK
  /*
  Key mask = Key(~0);
  Tree::NodeKey reqNodeKey;
  std::vector<Tree::NodeKey>::iterator iter;
  //std::vector<Tree::NodeKey> reqNodes = bucketReqs[reqID].requestedNodes;

  if(bucketReqs[reqID].requestedNodes.empty())
    CkPrintf("Error: [%d] bucket:%d has it's list as empty",thisIndex,reqID);
  for(iter = bucketReqs[reqID].requestedNodes.begin(); iter != bucketReqs[reqID].requestedNodes.end(); iter++){
    mask = Key(~0);
    reqNodeKey = (*iter);
    const Key subMask = Key(1) << 63;
    while(!(reqNodeKey & subMask)){
      reqNodeKey <<= 1;
      mask <<= 1;
    }
    reqNodeKey &= ~subMask;
    mask &= ~subMask;
    Key k = part[0].key & mask;
    
    if(k == reqNodeKey){
      break;
    }
  }
  CkAssert(iter!=bucketReqs[reqID].requestedNodes.end());
  bucketcheckList[reqID].insert(*iter);
  combineKeys(*iter,reqID);
  bucketReqs[reqID].requestedNodes.erase(iter);
  */
  bucketcheckList[reqIDlist].insert(remoteBucketID);
  combineKeys(remoteBucketID,reqIDlist);
#endif
  finishBucket(reqIDlist);
  //CkAssert(remainingChunk[chunk] >= 0);
  if (0/*remainingChunk[chunk] == 0*/) {
#ifdef COSMO_PRINT
    CkPrintf("[%d] Finished chunk %d with particle\n",thisIndex,chunk);
#endif
    streamingCache[CkMyPe()].finishedChunk(chunk, nodeInterRemote[chunk]+particleInterRemote[chunk]);
    if (chunk == numChunks-1) markWalkDone();
  }
}

void TreePiece::receiveParticles_inline(ExternalGravityParticle *part,int num,int chunk,
					unsigned int reqID, Tree::NodeKey remoteBucketID){
        receiveParticles(part,num,chunk,reqID,remoteBucketID);
}

#if COSMO_DEBUG > 1 || defined CHANGA_REFACTOR_WALKCHECK

//Recursive routine to combine keys -- Written only for Binary Trees
void TreePiece::combineKeys(Tree::NodeKey key,int bucket){

  Tree::NodeKey mask = Key(1);
  Tree::NodeKey lastBit = key & mask;
  Tree::NodeKey sibKey;
  
  if(lastBit==mask){
    sibKey = key >> 1;
    sibKey <<= 1;
  }
  else{
    sibKey = key | mask;
  }

  std::multiset<Tree::NodeKey>::iterator iter = (bucketcheckList[bucket]).find(sibKey);

  if(iter==bucketcheckList[bucket].end())
    return;
  else{//Sibling key has been found in the Binary tree
    bucketcheckList[bucket].erase(bucketcheckList[bucket].find(key));
    bucketcheckList[bucket].erase(iter);
    key >>= 1;
    bucketcheckList[bucket].insert(key);
    combineKeys(key,bucket);
  }
}

void TreePiece::checkWalkCorrectness(){

  Tree::NodeKey endKey = Key(1);
  int count = (2*nReplicas+1) * (2*nReplicas+1) * (2*nReplicas+1);
  CkPrintf("[%d(%d)]checking walk correctness...\n",thisIndex, CkMyPe());
  for(int i=0;i<numBuckets;i++){
    int wrong = 0;
    if(bucketcheckList[i].size()!=count) wrong = 1;
    for (std::multiset<Tree::NodeKey>::iterator iter = bucketcheckList[i].begin(); iter != bucketcheckList[i].end(); iter++) {
      if (*iter != endKey) wrong = 1;
    }
    if (wrong) {
      CkPrintf("Error: [%d] All the nodes not traversed by bucket no. %d\n",thisIndex,i);
      for (std::multiset<Tree::NodeKey>::iterator iter=bucketcheckList[i].begin(); iter != bucketcheckList[i].end(); iter++) {
	CkPrintf("       [%d] key %ld\n",thisIndex,*iter);
      }
      break;
    }
    else { bucketcheckList[i].clear(); }
  }
}
#endif

/********************************************************************/

void TreePiece::outputStatistics(Interval<unsigned int> macInterval, Interval<unsigned int> cellInterval, Interval<unsigned int> particleInterval, Interval<unsigned int> callsInterval, double totalmass, const CkCallback& cb) {

#if COSMO_STATS > 0
  if(verbosity > 1) {
    u_int64_t nodeInterRemoteTotal = 0;
    u_int64_t particleInterRemoteTotal = 0;
    for (int i=0; i<numChunks; ++i) {
      nodeInterRemoteTotal += nodeInterRemote[i];
      particleInterRemoteTotal += particleInterRemote[i];
    }
    ckerr << "TreePiece ";
    ckerr << thisIndex;
    ckerr << ": Statistics\nMy number of MAC checks: ";
    ckerr << myNumMACChecks << endl;
    ckerr << "My number of opened node: "
	  << nodesOpenedLocal << " local, " << nodesOpenedRemote << " remote." << endl;
    ckerr << "My number of particle-node interactions: "
	  << nodeInterLocal << " local, " << nodeInterRemoteTotal << " remote. Per particle: "
	  << (nodeInterLocal+nodeInterRemoteTotal)/(double) myNumParticles << endl;
    //	 << "\nCache cell interactions count: " << cachecellcount << endl;
    ckerr << "My number of particle-particle interactions: "
	  << particleInterLocal << " local, " << particleInterRemoteTotal
	  << " remote. Per Particle: "
	  << (particleInterLocal+particleInterRemoteTotal)/(double) myNumParticles << endl;
  }
#endif	

#if COSMO_STATS > 1
  /*
	double calmass,prevmass;

	for(int i=1;i<=myNumParticles;i++){
		calmass = (myParticles[i].intcellmass + myParticles[i].intpartmass + myParticles[i].extcellmass + myParticles[i].extpartmass);
		if(i>1)
			prevmass = (myParticles[i-1].intcellmass + myParticles[i-1].intpartmass + myParticles[i-1].extcellmass + myParticles[i-1].extpartmass);
		//CkPrintf("treepiece:%d ,mass:%lf, totalmass:%lf\n",thisIndex,calmass,totalmass);
		if(i>1)
			if(calmass != prevmass)
				CkPrintf("Tree piece:%d -- particles %d and %d differ in calculated total mass\n",thisIndex,i-1,i);
		if(calmass != totalmass)
				CkPrintf("Tree piece:%d -- particle %d differs from total mass\n",thisIndex,i);
	}

	CkPrintf("TreePiece:%d everything seems ok..\n",thisIndex);
  */

  /*
  if(thisIndex == 0) {
    macInterval.max = 0;
    macInterval.min = macInterval.max - 1;
    cellInterval = macInterval;
    particleInterval = macInterval;
    callsInterval = macInterval;
		
    if(verbosity > 2)
      ckerr << "TreePiece " << thisIndex << ": Writing headers for statistics files" << endl;
    fh.dimensions = 1;
    fh.code = TypeHandling::uint32;
    FILE* outfile = fopen((basefilename + ".MACs").c_str(), "wb");
    XDR xdrs;
    xdrstdio_create(&xdrs, outfile, XDR_ENCODE);
		
    unsigned int dummy;
    if(!xdr_template(&xdrs, &fh) || !xdr_template(&xdrs, &dummy) || !xdr_template(&xdrs, &dummy)) {
      ckerr << "TreePiece " << thisIndex << ": Could not write header to MAC file, aborting" << endl;
      CkAbort("Badness");
    }
    xdr_destroy(&xdrs);
    fclose(outfile);
		
    outfile = fopen((basefilename + ".cellints").c_str(), "wb");
    xdrstdio_create(&xdrs, outfile, XDR_ENCODE);
    if(!xdr_template(&xdrs, &fh) || !xdr_template(&xdrs, &dummy) || !xdr_template(&xdrs, &dummy)) {
      ckerr << "TreePiece " << thisIndex << ": Could not write header to cell-interactions file, aborting" << endl;
      CkAbort("Badness");
    }
    xdr_destroy(&xdrs);
    fclose(outfile);

    outfile = fopen((basefilename + ".partints").c_str(), "wb");
    xdrstdio_create(&xdrs, outfile, XDR_ENCODE);
    if(!xdr_template(&xdrs, &fh) || !xdr_template(&xdrs, &dummy) || !xdr_template(&xdrs, &dummy)) {
      ckerr << "TreePiece " << thisIndex << ": Could not write header to particle-interactions file, aborting" << endl;
      CkAbort("Badness");
    }
    xdr_destroy(&xdrs);
    fclose(outfile);

    outfile = fopen((basefilename + ".calls").c_str(), "wb");
    xdrstdio_create(&xdrs, outfile, XDR_ENCODE);
    if(!xdr_template(&xdrs, &fh) || !xdr_template(&xdrs, &dummy) || !xdr_template(&xdrs, &dummy)) {
      ckerr << "TreePiece " << thisIndex << ": Could not write header to entry-point calls file, aborting" << endl;
      CkAbort("Badness");
    }
    xdr_destroy(&xdrs);
    fclose(outfile);
  }
	
  if(verbosity > 3)
    ckerr << "TreePiece " << thisIndex << ": Writing my statistics to disk" << endl;

  FILE* outfile = fopen((basefilename + ".MACs").c_str(), "r+b");
  fseek(outfile, 0, SEEK_END);
  XDR xdrs;
  xdrstdio_create(&xdrs, outfile, XDR_ENCODE);

  for(unsigned int i = 1; i <= myNumParticles; ++i) {
    macInterval.grow(myParticles[i].numMACChecks);
    if(!xdr_template(&xdrs, &(myParticles[i].numMACChecks))) {
      ckerr << "TreePiece " << thisIndex << ": Error writing MAC checks to disk, aborting" << endl;
      CkAbort("Badness");
    }
  }
	
  if(thisIndex == (int) numTreePieces - 1) {
    if(verbosity > 3)
      ckerr << "MAC interval: " << macInterval << endl;
    if(!xdr_setpos(&xdrs, FieldHeader::sizeBytes) || !xdr_template(&xdrs, &macInterval.min) || !xdr_template(&xdrs, &macInterval.max)) {
      ckerr << "TreePiece " << thisIndex << ": Error going back to write the MAC bounds, aborting" << endl;
      CkAbort("Badness");
    }
    if(verbosity > 2)
      ckerr << "TreePiece " << thisIndex << ": Wrote the MAC bounds" << endl;
  }
	
  xdr_destroy(&xdrs);
  fclose(outfile);

  outfile = fopen((basefilename + ".cellints").c_str(), "r+b");
  fseek(outfile, 0, SEEK_END);
  xdrstdio_create(&xdrs, outfile, XDR_ENCODE);	
  for(unsigned int i = 1; i <= myNumParticles; ++i) {
    cellInterval.grow(myParticles[i].numCellInteractions);
    if(!xdr_template(&xdrs, &(myParticles[i].numCellInteractions))) {
      ckerr << "TreePiece " << thisIndex << ": Error writing cell interactions to disk, aborting" << endl;
      CkAbort("Badness");
    }
  }
  if(thisIndex == (int) numTreePieces - 1) {
    if(verbosity > 3)
      ckerr << "Cell interactions interval: " << cellInterval << endl;
    if(!xdr_setpos(&xdrs, FieldHeader::sizeBytes) || !xdr_template(&xdrs, &cellInterval.min) || !xdr_template(&xdrs, &cellInterval.max)) {
      ckerr << "TreePiece " << thisIndex << ": Error going back to write the cell interaction bounds, aborting" << endl;
      CkAbort("Badness");
    }
    if(verbosity > 2)
      ckerr << "TreePiece " << thisIndex << ": Wrote the cell interaction bounds" << endl;
  }
  xdr_destroy(&xdrs);
  fclose(outfile);

  outfile = fopen((basefilename + ".calls").c_str(), "r+b");
  fseek(outfile, 0, SEEK_END);
  xdrstdio_create(&xdrs, outfile, XDR_ENCODE);	
  for(unsigned int i = 1; i <= myNumParticles; ++i) {
    callsInterval.grow(myParticles[i].numEntryCalls);
    if(!xdr_template(&xdrs, &(myParticles[i].numEntryCalls))) {
      ckerr << "TreePiece " << thisIndex << ": Error writing entry calls to disk, aborting" << endl;
      CkAbort("Badness");
    }
  }
  if(thisIndex == (int) numTreePieces - 1) {
    if(verbosity > 3)
      ckerr << "Entry call interval: " << callsInterval << endl;
    if(!xdr_setpos(&xdrs, FieldHeader::sizeBytes) || !xdr_template(&xdrs, &callsInterval.min) || !xdr_template(&xdrs, &callsInterval.max)) {
      ckerr << "TreePiece " << thisIndex << ": Error going back to write the entry call bounds, aborting" << endl;
      CkAbort("Badness");
    }
    if(verbosity > 2)
      ckerr << "TreePiece " << thisIndex << ": Wrote the entry call bounds" << endl;
  }
  xdr_destroy(&xdrs);
  fclose(outfile);

  outfile = fopen((basefilename + ".partints").c_str(), "r+b");
  fseek(outfile, 0, SEEK_END);
  xdrstdio_create(&xdrs, outfile, XDR_ENCODE);	
  for(unsigned int i = 1; i <= myNumParticles; ++i) {
    particleInterval.grow(myParticles[i].numParticleInteractions);
    if(!xdr_template(&xdrs, &(myParticles[i].numParticleInteractions))) {
      ckerr << "TreePiece " << thisIndex << ": Error writing particle interactions to disk, aborting" << endl;
      CkAbort("Badness");
    }
  }
  if(thisIndex == (int) numTreePieces - 1) {
    if(verbosity > 3)
      ckerr << "Particle interactions interval: " << particleInterval << endl;
    if(!xdr_setpos(&xdrs, FieldHeader::sizeBytes) || !xdr_template(&xdrs, &particleInterval.min) || !xdr_template(&xdrs, &particleInterval.max)) {
      ckerr << "TreePiece " << thisIndex << ": Error going back to write the particle interaction bounds, aborting" << endl;
      CkAbort("Badness");
    }
    if(verbosity > 2)
      ckerr << "TreePiece " << thisIndex << ": Wrote the particle interaction bounds" << endl;
  }		
  xdr_destroy(&xdrs);
  fclose(outfile);
  */
#endif
	
  if(thisIndex != (int) numTreePieces - 1)
    pieces[thisIndex + 1].outputStatistics(macInterval, cellInterval, particleInterval, callsInterval, totalmass, cb);
  if(thisIndex == (int) numTreePieces - 1) cb.send();
}

/*
void TreePiece::outputRelativeErrors(Interval<double> errorInterval, const CkCallback& cb) {
  if(thisIndex == 0) {
    if(verbosity > 2)
      ckerr << "TreePiece " << thisIndex << ": Writing header for errors file" << endl;
    FILE* outfile = fopen((basefilename + ".error").c_str(), "wb");
    XDR xdrs;
    xdrstdio_create(&xdrs, outfile, XDR_ENCODE);
    fh.code = float64;
    fh.dimensions = 1;
    if(!xdr_template(&xdrs, &fh) || !xdr_template(&xdrs, &errorInterval.min) || !xdr_template(&xdrs, &errorInterval.max)) {
      ckerr << "TreePiece " << thisIndex << ": Could not write header to errors file, aborting" << endl;
      CkAbort("Badness");
    }
    xdr_destroy(&xdrs);
    fclose(outfile);
  }
	
  if(verbosity > 3)
    ckerr << "TreePiece " << thisIndex << ": Writing my errors to disk" << endl;
	
  FILE* outfile = fopen((basefilename + ".error").c_str(), "r+b");
  fseek(outfile, 0, SEEK_END);
  XDR xdrs;
  xdrstdio_create(&xdrs, outfile, XDR_ENCODE);
	
  double error;
	
  for(unsigned int i = 1; i <= myNumParticles; ++i) {
    error = (myParticles[i].treeAcceleration - myParticles[i].acceleration).length() / myParticles[i].acceleration.length();
    errorInterval.grow(error);
    if(!xdr_template(&xdrs, &error)) {
      ckerr << "TreePiece " << thisIndex << ": Error writing errors to disk, aborting" << endl;
      CkAbort("Badness");
    }
  }
	
  if(thisIndex == (int) numTreePieces - 1) {
    if(!xdr_setpos(&xdrs, FieldHeader::sizeBytes) || !xdr_template(&xdrs, &errorInterval.min) || !xdr_template(&xdrs, &errorInterval.max)) {
      ckerr << "TreePiece " << thisIndex << ": Error going back to write the error bounds, aborting" << endl;
      CkAbort("Badness");
    }
    if(verbosity > 2)
      ckerr << "TreePiece " << thisIndex << ": Wrote the error bounds" << endl;
    ckerr << "Error Bounds:" << errorInterval.min << ", "
	 << errorInterval.max << endl;
    cb.send();
  }
	
  xdr_destroy(&xdrs);
  fclose(outfile);
	
  if(thisIndex != (int) numTreePieces - 1)
    pieces[thisIndex + 1].outputRelativeErrors(errorInterval, cb);
}
*/

/// @TODO Fix pup routine to handle correctly the tree
void TreePiece::pup(PUP::er& p) {
  CBase_TreePiece::pup(p);
  
  // jetley
  p | proxy;
  p | proxyValid;
  p | proxySet;
  p | savedCentroid;
  p | prevLARung;

  p | numTreePieces;
  p | callback;
  p | myNumParticles;
  if(p.isUnpacking()) {
    myParticles = new GravityParticle[myNumParticles + 2];
  }
  for(unsigned int i=0;i<myNumParticles+2;i++){
    p | myParticles[i];
  }
  //p | numSplitters;
  //if(p.isUnpacking())
    //splitters = new Key[numSplitters];
  //p(splitters, numSplitters);
  p | pieces;
  //p | streamingProxy;
  p | basefilename;
  p | boundingBox;
  p | tipsyHeader;
  p | fh;
  p | started;
  p | iterationNo;
  if(p.isUnpacking()){
    switch (useTree) {
    case Binary_Oct:
      root = new BinaryTreeNode(1, Tree::Boundary, 0, myNumParticles+1, 0);
      break;
    case Binary_ORB:
      root = new BinaryTreeNode(1, Tree::Boundary, 0, myNumParticles+1, 0);
      break;
    case Oct_Oct:
      //root = new OctTreeNode(1, Tree::Boundary, 0, myNumParticles+1, 0);
      break;
    default:
      CkAbort("We should have never reached here!");
    }
  }

  //PUP components for ORB decomposition
  p | chunkRootLevel;
  if(p.isUnpacking()){
    boxes = new OrientedBox<float>[chunkRootLevel+1];
    splitDims = new char[chunkRootLevel+1];
  }
  for(unsigned int i=0;i<chunkRootLevel;i++){
    p | boxes[i];
    p | splitDims[i];
  }
  p | nSetupWriteStage;

  // Periodic variables
  p | nReplicas;
  p | fPeriod;
  p | bEwald;
  p | fEwCut;
  p | dEwhCut;
  p | bPeriodic;
  p | nMaxEwhLoop;
  if (p.isUnpacking() && bEwald) {
    ewt = new EWT[nMaxEwhLoop];
  }

  p | prefetchWaiting;
  p | currentPrefetch;
  p | numBuckets;
  p | currentBucket;
  p | currentRemoteBucket;
  p | ewaldCurrentBucket;
#if COSMO_STATS > 0
  //p | myNumParticleInteractions;
  //p | myNumCellInteractions;
  p | myNumMACChecks;
  p | nodesOpenedLocal;
  p | nodesOpenedRemote;
  p | numOpenCriterionCalls;
  p | piecemass;
#endif
  // the counters do not need to be pupped!
  //p | nodeInterLocal;
  //p | nodeInterRemote;
  //p | particleInterLocal;
  //p | particleInterRemote;
  if (p.isUnpacking()) {
    particleInterRemote = NULL;
    nodeInterRemote = NULL;

    switch(domainDecomposition) {
    case SFC_dec:
    case SFC_peano_dec:
      numPrefetchReq = 2;
      prefetchReq = new OrientedBox<double>[2];
      break;
    case Oct_dec:
    case ORB_dec:
      numPrefetchReq = 1;
      prefetchReq = new OrientedBox<double>[1];
      break;
    default:
      CmiAbort("Pupper has wrong domain decomposition type!\n");
    }
  }

  if(p.isUnpacking()){
    //localCache = cacheManagerProxy.ckLocalBranch();
    dm = NULL;
    myPlace = -1;

    // reconstruct the data for prefetching
    /* OLD, moved to the cache and startIteration
    numChunks = root->getNumChunks(_numChunks);
    //remainingChunk = new int[numChunks];
    root->getChunks(_numChunks, prefetchRoots);
    */
  }

  int notNull = (root==NULL)?0:1;
  p | notNull;
  if (notNull == 1) {
    p | (*root);
    if(p.isUnpacking()){
      //  nodeLookupTable[root->getKey()]=root;
      //}

      // reconstruct the nodeLookupTable and the bucketList
      reconstructNodeLookup(root);
    }
  }

  if (verbosity) {
    ckout << "TreePiece " << thisIndex << ": Getting PUP'd!";
    if (p.isSizing()) ckout << " size: " << ((PUP::sizer*)&p)->size();
    ckout << endl;
  }

  /*
  if(!(p.isUnpacking())) {
	
    //Pack nodeLookup here
    int num=0;
    for (NodeLookupType::iterator iter=nodeLookupTable.begin();iter!=nodeLookupTable.end();iter++){
      if(iter->second != root && iter->second != NULL){
	num++;
      }	
    }
    p(num);
    for (NodeLookupType::iterator iter=nodeLookupTable.begin();iter!=nodeLookupTable.end();iter++){
      if(iter->second != root && iter->second != NULL){
	Key k = iter->first;
	p | k;
	p | (*(iter->second));
      }	
    }
  }else{
    int num;
    p(num);
    for(int i=0;i<num;i++){
      Key k;
      GenericTreeNode *n = root->createNew();
      p | k;
      p | *n;
      nodeLookupTable[k] = n;
      if(n->getType() == Bucket){
	bucketList.push_back(n);
      }
    }
    int count=0;
    rebuildSFCTree(root,NULL,&count);
    sort(bucketList.begin(),bucketList.end(),compBucket);
    if(verbosity)
			CkPrintf("[%d] TreePiece %d bucketList size %d numBuckets %d nodelookupsize %d count %d\n",CkMyPe(),thisIndex,bucketList.size(),numBuckets,num,count);
  }
  */
}

void TreePiece::reconstructNodeLookup(GenericTreeNode *node) {
  nodeLookupTable[node->getKey()] = node;
  node->particlePointer = &myParticles[node->firstParticle];
  if (node->getType() == Bucket) bucketList.push_back(node);
  GenericTreeNode *child;
  for (unsigned int i=0; i<node->numChildren(); ++i) {
    child = node->getChildren(i);
    if (child != NULL) reconstructNodeLookup(child);
  }
}

/*
void TreePiece::rebuildSFCTree(GenericTreeNode *node,GenericTreeNode *parent,int *count){
  if(node == NULL){
    return;
  }
  (*count)++;
  node->parent = (GenericTreeNode *)parent;
  for (unsigned int i=0; i<node->numChildren(); ++i) {
    GenericTreeNode *child = nodeLookupTable[node->getChildKey(i)];
    switch (useTree) {
    case Binary_Oct:
      ((BinaryTreeNode*)node)->children[i] = (BinaryTreeNode*)child;
      break;
    case Oct_Oct:
      ((OctTreeNode*)node)->children[i] = (OctTreeNode*)child;
      break;
    default:
      CkAbort("We should have never reached here!");
    }
    rebuildSFCTree(child,node,count);
  }
}
bool compBucket(GenericTreeNode *ln,GenericTreeNode *rn){
  return (ln->firstParticle < rn->firstParticle);
}
*/

/** Check that all the particles in the tree are really in their boxes.
    Because the keys are made of only the first 21 out of 23 bits of the
    floating point representation, there can be particles that are outside
    their box by tiny amounts.  Whether this is bad is not yet known. */
void TreePiece::checkTree(GenericTreeNode* node) {
  if(node->getType() == Empty) return;
  if(node->getType() == Bucket) {
    for(unsigned int iter = node->firstParticle; iter <= node->lastParticle; ++iter) {
      if(!node->boundingBox.contains(myParticles[iter].position)) {
	ckerr << "Not in the box: Box: " << node->boundingBox << " Position: " << myParticles[iter].position << "\nNode key: " << keyBits(node->getKey(), 63).c_str() << "\nParticle key: " << keyBits(myParticles[iter].key, 63).c_str() << endl;
      }
    }
  } else if(node->getType() != NonLocal && node->getType() != NonLocalBucket) {
    GenericTreeNode* childIterator;
    for(unsigned int i = 0; i < node->numChildren(); ++i) {
      childIterator = node->getChildren(i);
      if(childIterator)
	checkTree(childIterator);
    }
  }
}

/// Color a node
string getColor(GenericTreeNode* node) {
  ostringstream oss;
  switch(node->getType()) {
  case Bucket:
  case Internal:
    oss << "black";
    break;
  case NonLocal:
  case NonLocalBucket:
    oss << "red";
    break;
  case Boundary:
    oss << "purple";
    break;
  default:
    oss << "yellow";
  }
  return oss.str();
}

/// Make a label for a node
string makeLabel(GenericTreeNode* node) {
  ostringstream oss;
  oss << keyBits(node->getKey(), 63) << "\\n";
  switch(node->getType()) {
  case Invalid:
    oss << "Invalid";
    break;
  case Bucket:
    //oss << "Bucket: " << (node->endParticle - node->beginParticle) << " particles";
    oss << "Bucket";
    break;
  case Internal:
    oss << "Internal";
    break;
  case NonLocal:
    oss << "NonLocal: Chare " << node->remoteIndex;
    break;
  case NonLocalBucket:
    oss << "NonLocalBucket: Chare " << node->remoteIndex;
    break;
  case Empty:
    oss << "Empty";
    break;
  case Boundary:
    oss << "Boundary: Total N " << node->remoteIndex;
    break;
  case Top:
    oss << "Top";
    break;
  default:
    oss << "Unknown NodeType!";
  }
  return oss.str();
}

/// Print a text version of a tree
void TreePiece::printTree(GenericTreeNode* node, ostream& os) {
  if(node == 0)
    return;
	
  string nodeID = keyBits(node->getKey(), 63);
  os << nodeID << " ";
  //os << "\tnode [color=\"" << getColor(node) << "\"]\n";
  //os << "\t\"" << nodeID << "\" [label=\"" << makeLabel(node) << "\\nCM: " << (node->moments.cm) << "\\nM: " << node->moments.totalMass << "\\nN_p: " << (node->endParticle - node->beginParticle) << "\\nOwners: " << node->numOwners << "\"]\n";
  //os << "\t\"" << nodeID << "\" [label=\"" << makeLabel(node) << "\\nLocal N: " << (node->endParticle - node->beginParticle) << "\\nOwners: " << node->numOwners << "\"]\n";
  //os << "\t\"" << nodeID << "\" [label=\"" << keyBits(node->getKey(), 63) << "\\n";
  int first, last;
  switch(node->getType()) {
  case Bucket:
    os << "Bucket: Size=" << (node->lastParticle - node->firstParticle + 1) << "(" << node->firstParticle << "-" << node->lastParticle << ")";
    break;
  case Internal:
    os << "Internal: Size=" << (node->lastParticle - node->firstParticle + 1) << "(" << node->firstParticle << "-" << node->lastParticle << ")";
    break;
  case NonLocal:
    //os << "NonLocal: Chare=" << node->remoteIndex << "\\nRemote N under: " << (node->lastParticle - node->firstParticle + 1) << "\\nOwners: " << node->numOwners;
    nodeOwnership(node->getKey(), first, last);
    os << "NonLocal: Chare=" << node->remoteIndex << ", Owners=" << first << "-" << last;
    break;
  case NonLocalBucket:
    //os << "NonLocal: Chare=" << node->remoteIndex << "\\nRemote N under: " << (node->lastParticle - node->firstParticle + 1) << "\\nOwners: " << node->numOwners;
    nodeOwnership(node->getKey(), first, last);
    CkAssert(first == last);
    os << "NonLocalBucket: Chare=" << node->remoteIndex << ", Owner=" << first << ", Size=" << node->particleCount << "(" << node->firstParticle << "-" << node->lastParticle << ")";
    break;
  case Boundary:
    nodeOwnership(node->getKey(), first, last);
    os << "Boundary: Totalsize=" << node->particleCount << ", Localsize=" << (node->lastParticle - node->firstParticle) << "(" << node->firstParticle + (node->firstParticle==0?1:0) << "-" << node->lastParticle - (node->lastParticle==myNumParticles+1?1:0) << "), Owners=" << first << "-" << last;
    break;
  case Empty:
    os << "Empty "<<node->remoteIndex;
    break;
  }
#ifndef HEXADECAPOLE
  if (node->getType() == Bucket || node->getType() == Internal || node->getType() == Boundary || node->getType() == NonLocal || node->getType() == NonLocalBucket) 
    os << " V "<<node->moments.radius<<" "<<node->moments.soft<<" "<<node->moments.cm.x<<" "<<node->moments.cm.y<<" "<<node->moments.cm.z<<" "<<node->moments.xx<<" "<<node->moments.xy<<" "<<node->moments.xz<<" "<<node->moments.yy<<" "<<node->moments.yz<<" "<<node->moments.zz<<" "<<node->boundingBox;
#endif
  os << "\n";
	
  //if(node->parent)
  //  os << "\t\"" << keyBits(node->parent->getKey(), 63) << "\" -> \"" << nodeID << "\";\n";
	
  if(node->getType() == NonLocal || node->getType() == NonLocalBucket || node->getType() == Bucket || node->getType() == Empty)
    return;

  GenericTreeNode* childIterator;
  for(unsigned int i = 0; i < node->numChildren(); ++i) {
    childIterator = node->getChildren(i);
    if(childIterator)
      printTree(childIterator, os);
    else {
      os << "\tnode [color=\"green\"]\n";
      os << "\t\"" << nodeID << i << "\" [label=\"None\"]\n";
      os << "\t\"" << nodeID << "\" -> \"" << nodeID << i << "\";\n";
    }
  }
}

/// Print a graphviz version of A tree
void TreePiece::printTreeViz(GenericTreeNode* node, ostream& os) {
  if(node == 0)
    return;
	
  string nodeID = keyBits(node->getKey(), 63);
  os << "\tnode [color=\"" << getColor(node) << "\"]\n";
  //os << "\t\"" << nodeID << "\" [label=\"" << makeLabel(node) << "\\nCM: " << (node->moments.cm) << "\\nM: " << node->moments.totalMass << "\\nN_p: " << (node->endParticle - node->beginParticle) << "\\nOwners: " << node->numOwners << "\"]\n";
  //os << "\t\"" << nodeID << "\" [label=\"" << makeLabel(node) << "\\nLocal N: " << (node->endParticle - node->beginParticle) << "\\nOwners: " << node->numOwners << "\"]\n";
  os << "\t\"" << nodeID << "\" [label=\"" << keyBits(node->getKey(), 63) << "\\n";
  int first, last;
  switch(node->getType()) {
  case Bucket:
    os << "Bucket\\nSize: " << (node->lastParticle - node->firstParticle + 1);
    break;
  case Internal:
    os << "Internal\\nSize: " << (node->lastParticle - node->firstParticle + 1);
    break;
  case NonLocal:
    nodeOwnership(node->getKey(), first, last);
    os << "NonLocal: Chare " << node->remoteIndex << "\\nOwners: " << (last-first+1) << "\\nRemote size: " << (node->lastParticle - node->firstParticle + 1);
    //os << "NonLocal: Chare=" << node->remoteIndex; //<< ", Owners=" << first << "-" << last;
    break;
  case NonLocalBucket:
    //os << "NonLocal: Chare=" << node->remoteIndex << "\\nRemote N under: " << (node->lastParticle - node->firstParticle + 1) << "\\nOwners: " << node->numOwners;
    nodeOwnership(node->getKey(), first, last);
    //CkAssert(first == last);
    os << "NonLocalBucket: Chare " << node->remoteIndex << "\\nOwner: " << first << "\\nSize: " << node->particleCount; //<< "(" << node->firstParticle << "-" << node->lastParticle << ")";
    break;
  case Boundary:
    //nodeOwnership(node->getKey(), first, last);
    os << "Boundary\\nTotalsize: " << node->particleCount << "\\nLocalsize: " << (node->lastParticle - node->firstParticle);
    break;
  case Empty:
    os << "Empty "<<node->remoteIndex;
    break;
  }
  //if (node->getType() == Bucket || node->getType() == Internal || node->getType() == Boundary || node->getType() == NonLocal || node->getType() == NonLocalBucket) 
  //  os << " V "<<node->moments.radius<<" "<<node->moments.soft<<" "<<node->moments.cm.x<<" "<<node->moments.cm.y<<" "<<node->moments.cm.z<<" "<<node->moments.xx<<" "<<node->moments.xy<<" "<<node->moments.xz<<" "<<node->moments.yy<<" "<<node->moments.yz<<" "<<node->moments.zz;

  os << "\"]\n";
	
  if(node->parent)
    os << "\t\"" << keyBits(node->parent->getKey(), 63) << "\" -> \"" << nodeID << "\";\n";
	
  if(node->getType() == NonLocal || node->getType() == NonLocalBucket || node->getType() == Bucket || node->getType() == Empty)
    return;

  GenericTreeNode* childIterator;
  for(unsigned int i = 0; i < node->numChildren(); ++i) {
    childIterator = node->getChildren(i);
    if(childIterator)
      printTreeViz(childIterator, os);
    else {
      os << "\tnode [color=\"green\"]\n";
      os << "\t\"" << nodeID << i << "\" [label=\"None\"]\n";
      os << "\t\"" << nodeID << "\" -> \"" << nodeID << i << "\";\n";
    }
  }
}

/// Write a file containing a graphviz dot graph of my tree
void TreePiece::report() {
  ostringstream outfilename;
  outfilename << "tree." << thisIndex << "." << iterationNo << ".dot";
  ofstream os(outfilename.str().c_str());

  os << "digraph G" << thisIndex << " {\n";
  os << "\tcenter = \"true\"\n";
  os << "\tsize = \"7.5,10\"\n";
  //os << "\tratio = \"fill\"\n";
  //os << "\tfontname = \"Courier\"\n";
  os << "\tnode [style=\"bold\"]\n";
  os << "\tlabel = \"Piece: " << thisIndex << "\\nParticles: " 
     << myNumParticles << "\"\n";
  /*	os << "\tlabel = \"Piece: " << thisIndex << "\\nParticles: " 
	<< myNumParticles << "\\nLeft Splitter: " << keyBits(myParticles[0].key, 63)
	<< "\\nLeftmost Key: " << keyBits(myParticles[1].key, 63) 
	<< "\\nRightmost Key: " << keyBits(myParticles[myNumParticles].key, 63) 
	<< "\\nRight Splitter: " << keyBits(myParticles[myNumParticles + 1].key, 63) << "\";\n";
  */
  os << "\tfontname = \"Helvetica\"\n";
  printTreeViz(root, os);
  os << "}" << endl;
	
  os.close();
	
  //checkTree(root);
		
  //contribute(0, 0, CkReduction::concat, cb);
}

/// Print a text version of a tree
void printGenericTree(GenericTreeNode* node, ostream& os) {
  if(node == 0)
    return;
        
  string nodeID = keyBits(node->getKey(), 63);
  os << nodeID << " ";
  //os << "\tnode [color=\"" << getColor(node) << "\"]\n";
  //os << "\t\"" << nodeID << "\" [label=\"" << makeLabel(node) << "\\nCM: " << (node->moments.cm) << "\\nM: " << node->moments.totalMass << "\\nN_p: " << (node->endParticle - node->beginParticle) << "\\nOwners: " << node->numOwners << "\"]\n";
  //os << "\t\"" << nodeID << "\" [label=\"" << makeLabel(node) << "\\nLocal N: " << (node->endParticle - node->beginParticle) << "\\nOwners: " << node->numOwners << "\"]\n";
  //os << "\t\"" << nodeID << "\" [label=\"" << keyBits(node->getKey(), 63) << "\\n";
  int first, last;
  switch(node->getType()) {
  case Bucket:
    os << "Bucket: Size=" << (node->lastParticle - node->firstParticle + 1) << "(" << node->firstParticle << "-" << node->lastParticle << ")";
    break;
  case Internal:
    os << "Internal: Size=" << (node->lastParticle - node->firstParticle + 1) << "(" << node->firstParticle << "-" << node->lastParticle << ")";
    break;
  case NonLocal:
    //os << "NonLocal: Chare=" << node->remoteIndex << "\\nRemote N under: " << (node->lastParticle - node->firstParticle + 1) << "\\nOwners: " << node->numOwners;
    //nodeOwnership(node->getKey(), first, last);
    os << "NonLocal: Chare=" << node->remoteIndex; //<< ", Owners=" << first << "-" << last;
    break;
  case NonLocalBucket:
    //os << "NonLocal: Chare=" << node->remoteIndex << "\\nRemote N under: " << (node->lastParticle - node->firstParticle + 1) << "\\nOwners: " << node->numOwners;
    //nodeOwnership(node->getKey(), first, last);
    //CkAssert(first == last);
    os << "NonLocalBucket: Chare=" << node->remoteIndex << ", Owner=" << first << ", Size=" << node->particleCount; //<< "(" << node->firstParticle << "-" << node->lastParticle << ")";
    break;
  case Boundary:
    //nodeOwnership(node->getKey(), first, last);
    os << "Boundary: Totalsize=" << node->particleCount << ", Localsize=" << (node->lastParticle - node->firstParticle) << "(" << node->firstParticle << "-" << node->lastParticle;
    break;
  case Empty:
    os << "Empty "<<node->remoteIndex;
    break;
  }
#ifndef HEXADECAPOLE
  if (node->getType() == Bucket || node->getType() == Internal || node->getType() == Boundary || node->getType() == NonLocal || node->getType() == NonLocalBucket) 
    os << " V "<<node->moments.radius<<" "<<node->moments.soft<<" "<<node->moments.cm.x<<" "<<node->moments.cm.y<<" "<<node->moments.cm.z<<" "<<node->moments.xx<<" "<<node->moments.xy<<" "<<node->moments.xz<<" "<<node->moments.yy<<" "<<node->moments.yz<<" "<<node->moments.zz;
#endif

  os << "\n";
        
  //if(node->parent)
  //  os << "\t\"" << keyBits(node->parent->getKey(), 63) << "\" -> \"" << nodeID << "\";\n";
        
  if(node->getType() == NonLocal || node->getType() == NonLocalBucket || node->getType() == Bucket || node->getType() == Empty)
    return;

  GenericTreeNode* childIterator;
  for(unsigned int i = 0; i < node->numChildren(); ++i) {
    childIterator = node->getChildren(i);
    if(childIterator)
      printGenericTree(childIterator, os);
    else {
      os << "\tnode [color=\"green\"]\n";
      os << "\t\"" << nodeID << i << "\" [label=\"None\"]\n";
      os << "\t\"" << nodeID << "\" -> \"" << nodeID << i << "\";\n";
    }
  }
}

/*
void TreePiece::getPieceValues(piecedata *totaldata){
#if COSMO_STATS > 0
  totaldata->modifypiecedata(myNumCellInteractions,myNumParticleInteractions,myNumMACChecks,piecemass);
  if(thisIndex != (int) numTreePieces - 1)
  	pieces[thisIndex + 1].getPieceValues(totaldata);
  else {
    CkCallback& cb= totaldata->getcallback();
    cb.send(totaldata);
  }
#endif
}
*/

CkReduction::reducerType TreePieceStatistics::sum;

/*
 * Collect treewalking statistics across all TreePieces
 */

void TreePiece::collectStatistics(CkCallback& cb) {
#if COSMO_DEBUG > 1 || defined CHANGA_REFACTOR_WALKCHECK

checkWalkCorrectness();
#endif

#if COSMO_STATS > 0
  u_int64_t nodeInterRemoteTotal = 0;
  u_int64_t particleInterRemoteTotal = 0;
  for (int i=0; i<numChunks; ++i) {
    nodeInterRemoteTotal += nodeInterRemote[i];
    particleInterRemoteTotal += particleInterRemote[i];
  }
  TreePieceStatistics tps(nodesOpenedLocal, nodesOpenedRemote, numOpenCriterionCalls,
      nodeInterLocal, nodeInterRemoteTotal, particleInterLocal, particleInterRemoteTotal, nActive);
  contribute(sizeof(TreePieceStatistics), &tps, TreePieceStatistics::sum, cb);
#else
  CkAbort("Invalid call, only valid if COSMO_STATS is defined");
#endif
}

GenericTreeNode *TreePiece::nodeMissed(int reqID, int remoteIndex, Tree::NodeKey &key, int chunk, bool isPrefetch, int awi){
  GenericTreeNode *gtn = requestNode(remoteIndex, key, chunk, reqID, awi, isPrefetch); 
  return gtn;
}

ExternalGravityParticle *TreePiece::particlesMissed(Tree::NodeKey &key, int chunk, int remoteIndex, int firstParticle, int lastParticle, int reqID, bool isPrefetch, int awi){
  return requestParticles(key, chunk, remoteIndex,firstParticle,lastParticle,reqID, awi, isPrefetch);
}

// This is invoked when a remote node is received from the CacheManager
// It sets up a tree walk starting at node and initiates it
void TreePiece::receiveNodeCallback(GenericTreeNode *node, int chunk, int reqID, int awi){
  void *computeEntity;
  int reqIDlist = decodeReqID(reqID);
  Vector3D<double> offset = decodeOffset(reqID);

  TreeWalk *tw;
  Compute *compute;
  State *state;

  // retrieve the activewalk record
  CkAssert(awi < activeWalks.size());
  
  ActiveWalk &a = activeWalks[awi];
  tw = a.tw;
  compute = a.c;
  state = a.s;

  // reassociate objects with each other
  tw->reassoc(compute);
  // FIXME - this is not right - assumes that we can only have buckets for computeEntities when
  // restarting walks. Some abstraction is missing here.
  // This works for prefetch walks only because the PrefetchCompute::reassoc
  // function doesn't do anything at all
  compute->reassoc((void *)bucketList[reqIDlist], activeRung, a.o);
  // state was retrieved from activeWalks list instead
  //state = compute->getResumeState(reqIDlist);

  // resume walk
  tw->walk(node, state, chunk, reqID, awi);

  compute->nodeRecvdEvent(this,chunk,state,reqIDlist);
}

void TreePiece::receiveParticlesCallback(ExternalGravityParticle *egp, int num, int chunk, int reqID, Tree::NodeKey &remoteBucket, int awi){
  //TreeWalk *tw;
  Compute *c;
  State *state;

  int reqIDlist = decodeReqID(reqID);

  // retrieve the activewalk record
  ActiveWalk &a = activeWalks[awi];
  //tw = a.tw;
  c = a.c;
  state = a.s;
  // no need to reassociate objects with each other - we will not resume any walks
  //tw->reassoc(c);
  //c->reassoc(reqIDlist, this, activeRung, a.o);
  c->reassoc((void *)bucketList[reqIDlist], activeRung, a.o);
  // state was retrieved from activeWalks list instead
  //state = c->getResumeState(reqIDlist);
  
  c->recvdParticles(egp,num,chunk,reqID,state,this);
}

int TreePiece::addActiveWalk(TreeWalk *tw, Compute *c, Opt *o, State *s){
  return activeWalks.push_back_v(ActiveWalk(tw,c,o,s));
}

void TreePiece::freeWalkObjects(){
  for(int i = 0; i < activeWalks.length(); i++){
    State *state = activeWalks[i].s;
    for(int j = 0; j < state->counterArrays.length(); j++)
      state->counterArrays[j].free();
  }
    
  activeWalks.free();

  delete sTopDown;
  if(sGravity) {
      delete sGravity;
      delete sRemote;
      delete sRemoteGravityState;
      delete sLocal;
      delete sLocalGravityState;
      sGravity = NULL;
      }
  if(sPrefetch) {
      delete sPrefetch;
      delete sPref;
      delete sPrefetchState;
      sPrefetch = NULL;
      }
  if(sSmooth) {
      delete sSmooth;
      delete optSmooth;
      delete sSmoothState;
      sSmooth = NULL;
      }
}

void TreePiece::markWalkDone() {
  if (++completedActiveWalks == activeWalks.size()) {
    freeWalkObjects();
    contribute(0, 0, CkReduction::concat, callback);
  }
}
