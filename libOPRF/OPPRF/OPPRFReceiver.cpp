#include "OPPRFReceiver.h"
#include <future>

#include "Crypto/PRNG.h"
#include "Crypto/Commit.h"

#include "Common/Log.h"
#include "Common/Log1.h"
#include "Base/naor-pinkas.h"
#include <unordered_map>

#include "TwoChooseOne/IknpOtExtReceiver.h"
#include "TwoChooseOne/IknpOtExtSender.h"
#include "Hashing/BitPosition.h"


//#define PRINT
namespace osuCrypto
{
    OPPRFReceiver::OPPRFReceiver()
    {
    }

    OPPRFReceiver::~OPPRFReceiver()
    {
    }

    void OPPRFReceiver::init(u32 opt, u64 numParties,
        u64 n,
        u64 statSec,
        u64 inputBitSize,
        Channel & chl0, u64 otCounts,
		NcoOtExtReceiver& otRecv,
		NcoOtExtSender& otSend,
        block seed, bool isOtherDirection)
    {
        init(opt,numParties,n, statSec, inputBitSize, { &chl0 }, otCounts, otRecv, otSend, seed, isOtherDirection);
    }

	
    void OPPRFReceiver::init(u32 opt, u64 numParties,
			u64 n,
			u64 statSecParam,
			u64 inputBitSize,
			const std::vector<Channel*>& chls, u64 otCounts,
			NcoOtExtReceiver& otRecv,
			NcoOtExtSender& otSend,
			block seed, bool isOtherDirection)
	{
		
		//testReceiver();
		// this is the offline function for doing binning and then performing the OtPsi* between the bins.
		mParties = numParties;
		mStatSecParam = statSecParam;
		mN = n;

		

		// must be a multiple of 128...
		u64 baseOtCount;// = 128 * CodeWordSize;
		u64 compSecParam = 128;

		otSend.getParams(
			false,
			compSecParam, statSecParam, inputBitSize, mN, //  input
			mNcoInputBlkSize, baseOtCount); // output

											//mOtMsgBlkSize = (baseOtCount + 127) / 128;
		if (opt == 3)
		{		
			//######create hash
			mBFHasher.resize(mNumBFhashs);
			for (u64 i = 0; i < mBFHasher.size(); ++i)
				mBFHasher[i].setKey(_mm_set1_epi64x(i));// ^ mHashingSeed);
		}
		

		gTimer.setTimePoint("Init.recv.start");
		mPrng.SetSeed(seed);
		auto& prng = mPrng;

		auto myHashSeed = prng.get<block>();

		auto& chl0 = *chls[0];

		// we need a random hash function, so we will both commit to a seed and then later decommit. 
		//This is the commitments phase
		//Commit comm(myHashSeed), theirComm;
		//chl0.asyncSend(comm.data(), comm.size());
		//chl0.recv(theirComm.data(), theirComm.size());

		//// ok, now decommit to the seed.
		//chl0.asyncSend(&myHashSeed, sizeof(block));
		//block theirHashingSeed;
		//chl0.recv(&theirHashingSeed, sizeof(block));

		//gTimer.setTimePoint("Init.recv.hashSeed");

		//// compute the hashing seed as the xor of both of ours seeds.
		//mHashingSeed = myHashSeed ^ theirHashingSeed;


		// how many OTs we need in total.
		u64 otCountSend = otCounts;// mSimpleBins.mBins.size();
		u64 otCountRecv = otCounts; //mCuckooBins.mBins.size();


		gTimer.setTimePoint("Init.recv.baseStart");
		// since we are doing mmlicious PSI, we need OTs going in both directions. 
		// This will hold the send OTs

		if (otRecv.hasBaseOts() == false ||
			(otSend.hasBaseOts() == false && isOtherDirection))
		{
			// first do 128 public key OTs (expensive)
			std::array<block, gOtExtBaseOtCount> kosSendBase;
			BitVector choices(gOtExtBaseOtCount); choices.randomize(prng);
			NaorPinkas base;
			base.receive(choices, kosSendBase, prng, chl0, 2);


			// now extend these to enough recv OTs to seed the send Kco and the send Kos ot extension
			u64 dualBaseOtCount = gOtExtBaseOtCount;
			if (!isOtherDirection) //if it is not dual, number extend OT is 128
				dualBaseOtCount = 0;

			IknpOtExtSender iknpSend;
			iknpSend.setBaseOts(kosSendBase, choices);
			std::vector<std::array<block, 2>> sendBaseMsg(baseOtCount + dualBaseOtCount);
			iknpSend.send(sendBaseMsg, prng, chl0);


			// Divide these OT mssages between the Kco and Kos protocols
			ArrayView<std::array<block, 2>> kcoRecvBase(
				sendBaseMsg.begin(),
				sendBaseMsg.begin() + baseOtCount);
			// now set these ~800 OTs as the base of our N choose 1 OTs.
			otRecv.setBaseOts(kcoRecvBase);

			if (isOtherDirection) {
				ArrayView<std::array<block, 2>> kosRecvBase(
					sendBaseMsg.begin() + baseOtCount,
					sendBaseMsg.end());

				BitVector recvChoice(baseOtCount); recvChoice.randomize(prng);
				std::vector<block> kcoSendBase(baseOtCount);
				IknpOtExtReceiver iknp;
				iknp.setBaseOts(kosRecvBase);
				iknp.receive(recvChoice, kcoSendBase, prng, chl0);
				// now set these ~800 OTs as the base of our N choose 1 OTs.
				otSend.setBaseOts(kcoSendBase, recvChoice);
			}
			
		}


		gTimer.setTimePoint("Init.recv.ExtStart");




		auto sendOtRoutine = [&](u64 tIdx, u64 total, NcoOtExtSender& ots, Channel& chl)
		{
			auto start = (tIdx     *otCountSend / total) ;
			auto end = ((tIdx + 1) * otCountSend / total);

			ots.init(end - start);
		};

		auto recvOtRoutine = [&](u64 tIdx, u64 total, NcoOtExtReceiver& ots, Channel& chl)
		{
			auto start = (tIdx     * otCountRecv / total) ;
			auto end = ((tIdx + 1) * otCountRecv / total) ;

			ots.init(end - start);
		};


		// compute how amny threads we want to do for each direction.
		// the current thread will do one of the OT receives so -1 for that.
		u64 numThreads = chls.size() - 1;
		u64 numRecvThreads, numSendThreads;

		if (isOtherDirection) {
			 numRecvThreads = numThreads / 2;
			numSendThreads = numThreads - numRecvThreads;
		}
		else {
			numRecvThreads = numThreads;
			numSendThreads = 0;
		}
		// where we will store the threads that are doing the extension
		std::vector<std::thread> thrds(numThreads);

		// some iters to help giving out resources.
		auto thrdIter = thrds.begin();
		auto chlIter = chls.begin() + 1;

		mOtRecvs.resize(chls.size());

		// now make the threads that will to the extension
		for (u64 i = 0; i < numRecvThreads; ++i)
		{
			mOtRecvs[i + 1] = std::move(otRecv.split());

			// spawn the thread and call the routine.
			*thrdIter++ = std::thread([&, i, chlIter]()
			{
				recvOtRoutine(i + 1, numRecvThreads + 1, *mOtRecvs[i + 1], **chlIter);
			});

			++chlIter;
		}
		mOtRecvs[0] = std::move(otRecv.split());
		// now use this thread to do a recv routine.
		recvOtRoutine(0, numRecvThreads + 1, *mOtRecvs[0], chl0);



		mOtSends.resize(chls.size());
		// do the same thing but for the send OT extensions
		for (u64 i = 0; i < numSendThreads; ++i)
		{

			mOtSends[i] = std::move(otSend.split());

			*thrdIter++ = std::thread([&, i, chlIter]()
			{
				sendOtRoutine(i, numSendThreads, *mOtSends[i], **chlIter);
			});

			++chlIter;
		}

		// if the caller doesnt want to do things in parallel
		// the we will need to do the send OT Ext now...
		if (numSendThreads == 0 && isOtherDirection)
		{
			mOtSends[0] = std::move(otSend.split());
			sendOtRoutine(0, 1, *mOtSends[0], chl0);
		}

		// join any threads that we created.
		for (auto& thrd : thrds)
			thrd.join();

		gTimer.setTimePoint("Init.recv.done");

	}



	void OPPRFReceiver::getOPRFkeys(u64 IdxParty, binSet& bins, Channel & chl, bool isOtherDirectionGetOPRF)
	{
		
		if (bins.mOpt == 0 || bins.mOpt == 1)
			getOPRFkeysSeperated(IdxParty, bins, { &chl }, isOtherDirectionGetOPRF);
		else
			getOPRFkeysCombined(IdxParty, bins, { &chl }, isOtherDirectionGetOPRF);
	}

	void OPPRFReceiver::getOPRFkeys(u64 IdxParty, binSet& bins, const std::vector<Channel*>& chls, bool isOtherDirectionGetOPRF)
	{
		if(bins.mOpt==0 || bins.mOpt == 1)
			getOPRFkeysSeperated(IdxParty, bins,chls , isOtherDirectionGetOPRF);
		else
			getOPRFkeysCombined(IdxParty, bins, chls , isOtherDirectionGetOPRF);
		
	}

	void OPPRFReceiver::sendSS(u64 IdxParty, binSet& bins, std::vector<block>& plaintexts, Channel & chl)
	{
		if (bins.mOpt == 0)
			sendSSTableBased(IdxParty, bins, plaintexts, { &chl });
		else if (bins.mOpt == 1)
			sendSSPolyBased(IdxParty, bins, plaintexts, { &chl });
		else if (bins.mOpt == 2)
			sendFullPolyBased(IdxParty, bins, plaintexts, { &chl });
		else if (bins.mOpt == 3)
			sendBFBased(IdxParty, bins, plaintexts, { &chl });

	}
	void OPPRFReceiver::recvSS(u64 IdxParty, binSet& bins, std::vector<block>& plaintexts, Channel & chl)
	{
		if (bins.mOpt == 0)
			recvSSTableBased(IdxParty, bins, plaintexts, { &chl });
		else if (bins.mOpt == 1)
			recvSSPolyBased(IdxParty, bins, plaintexts, { &chl });
		else if (bins.mOpt == 2)
			recvFullPolyBased(IdxParty, bins, plaintexts, { &chl });
		else if (bins.mOpt == 3)
			recvBFBased(IdxParty, bins, plaintexts, { &chl });
	}

	void OPPRFReceiver::sendSS(u64 IdxParty, binSet& bins, std::vector<block>& plaintexts, const std::vector<Channel*>& chls)
	{
		if (bins.mOpt == 0)
			sendSSTableBased(IdxParty, bins, plaintexts, chls);
		else if (bins.mOpt == 1)
			sendSSPolyBased(IdxParty, bins, plaintexts, chls);
		else if (bins.mOpt == 2)
			sendFullPolyBased(IdxParty, bins, plaintexts, chls);
		else if (bins.mOpt == 3)
			sendBFBased(IdxParty, bins, plaintexts, chls);

	}

	void OPPRFReceiver::recvSS(u64 IdxParty, binSet& bins, std::vector<block>& plaintexts, const std::vector<Channel*>& chls)
	{
		if (bins.mOpt == 0)
			recvSSTableBased(IdxParty, bins, plaintexts, chls);
		else if (bins.mOpt == 1)
			recvSSPolyBased(IdxParty, bins, plaintexts, chls);
		else if (bins.mOpt == 2)
			recvFullPolyBased(IdxParty, bins, plaintexts, chls);
		else if (bins.mOpt == 3)
			recvBFBased(IdxParty, bins, plaintexts, chls);
	}




	void  OPPRFReceiver::getOPRFkeysSeperated(u64 IdxP, binSet& bins, const std::vector<Channel*>& chls, bool isOtherDirectionGetOPRF)
	{
#if 1
		// this is the online phase.
		gTimer.setTimePoint("online.recv.start");

		
		std::vector<std::thread>  thrds(chls.size());
		//  std::vector<std::thread>  thrds(1);

		// fr each thread, spawn it.
		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
		{
			auto seed = mPrng.get<block>();
			thrds[tIdx] = std::thread([&, tIdx, seed]()
			{

				if (tIdx == 0) gTimer.setTimePoint("online.recv.thrdStart");

				
				
				auto& chl = *chls[tIdx];
				
				if (tIdx == 0) gTimer.setTimePoint("online.recv.insertDone");

				const u64 stepSize = 16;

				std::vector<block> ncoInput(bins.mNcoInputBlkSize);

#if 1
#pragma region compute Recv Bark-OPRF

				//####################
				//#######Recv role
				//####################
				auto& otRecv = *mOtRecvs[tIdx];

				auto otCountRecv = bins.mCuckooBins.mBins.size();
				// get the region of the base OTs that this thread should do.
				auto binStart = tIdx       * otCountRecv / thrds.size();
				auto binEnd = (tIdx + 1) * otCountRecv / thrds.size();

				for (u64 bIdx = binStart; bIdx < binEnd;)
				{
					u64 currentStepSize = std::min(stepSize, binEnd - bIdx);

					for (u64 stepIdx = 0; stepIdx < currentStepSize; ++bIdx, ++stepIdx)
					{
						auto& bin = bins.mCuckooBins.mBins[bIdx];

						if (!bin.isEmpty())
						{
							u64 inputIdx = bin.idx();

							for (u64 j = 0; j < ncoInput.size(); ++j)
								ncoInput[j] = bins.mNcoInputBuff[j][inputIdx];

							otRecv.encode(
								bIdx,      // input
								ncoInput,             // input
								bin.mValOPRF[IdxP]); // output
						}
						else
							otRecv.zeroEncode(bIdx);
					}
					otRecv.sendCorrection(chl, currentStepSize);
				}

				  if (tIdx == 0) gTimer.setTimePoint("online.recv.otRecv.finalOPRF");



#pragma endregion
#endif

#if 1
#pragma region compute Send Bark-OPRF				
				//####################
				//#######Sender role
				//####################
				  if (isOtherDirectionGetOPRF) {
					  auto& otSend = *mOtSends[tIdx];
					  auto otCountSend = bins.mSimpleBins.mBins.size();

					  binStart = tIdx       * otCountSend / thrds.size();
					  binEnd = (tIdx + 1) * otCountSend / thrds.size();


					  if (tIdx == 0) gTimer.setTimePoint("online.send.OT");

					  for (u64 bIdx = binStart; bIdx < binEnd;)
					  {
						  u64 currentStepSize = std::min(stepSize, binEnd - bIdx);
						  otSend.recvCorrection(chl, currentStepSize);

						  for (u64 stepIdx = 0; stepIdx < currentStepSize; ++bIdx, ++stepIdx)
						  {

							  auto& bin = bins.mSimpleBins.mBins[bIdx];

							  if (bin.mIdx.size() > 0)
							  {
								  bin.mValOPRF[IdxP].resize(bin.mIdx.size());

								  //std::cout << "s-" << bIdx << ", ";
								  for (u64 i = 0; i < bin.mIdx.size(); ++i)
								  {

									  u64 inputIdx = bin.mIdx[i];

									  for (u64 j = 0; j < mNcoInputBlkSize; ++j)
									  {
										  ncoInput[j] = bins.mNcoInputBuff[j][inputIdx];
									  }

									  otSend.encode(
										  bIdx, //each bin has 1 OT
										  ncoInput,
										  bin.mValOPRF[IdxP][i]);

								  }

								  //#####################
								  //######Finding bit locations
								  //#####################

							  //	std::cout << bin.mValOPRF[IdxP][0];

								  //diff max bin size for first mSimpleBins.mBinCount and 
								  // mSimpleBins.mBinStashCount
								  if (bIdx < bins.mSimpleBins.mBinCount[0])
									  bin.mBits[IdxP].init(/*bin.mIdx.size(),*/ bins.mSimpleBins.mNumBits[0]);
								  else
									  bin.mBits[IdxP].init(/*bin.mIdx.size(),*/ bins.mSimpleBins.mNumBits[1]);

								  //Table-based-OPPRF
								  if(bins.mOpt=0)
									 bin.mBits[IdxP].getPos1(bin.mValOPRF[IdxP], 128);
								  //bin.mBits[IdxP].getMasks(bin.mValOPRF[IdxP]);
								  //std::cout << ", "
								  //	<< static_cast<int16_t>(bin.mBits[IdxP].mMaps[0]) << std::endl;
							  }
						  }
					  }
					  if (tIdx == 0) gTimer.setTimePoint("online.send.otSend.finalOPRF");
					  otSend.check(chl);
				  }
#pragma endregion
#endif
				otRecv.check(chl);				
			});
		}

		// join the threads.
		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
			thrds[tIdx].join();

		gTimer.setTimePoint("online.recv.exit");

		//std::cout << gTimer;
#endif
	}

	void  OPPRFReceiver::getOPRFkeysCombined(u64 IdxP, binSet& bins, const std::vector<Channel*>& chls, bool isOtherDirectionGetOPRF)
	{
#if 1
		// this is the online phase.
		gTimer.setTimePoint("online.recv.start");


		std::vector<std::thread>  thrds(chls.size());
		//  std::vector<std::thread>  thrds(1);

		// fr each thread, spawn it.
		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
		{
			auto seed = mPrng.get<block>();
			thrds[tIdx] = std::thread([&, tIdx, seed]()
			{

				if (tIdx == 0) gTimer.setTimePoint("online.recv.thrdStart");



				auto& chl = *chls[tIdx];

				if (tIdx == 0) gTimer.setTimePoint("online.recv.insertDone");

				const u64 stepSize = 16;

				std::vector<block> ncoInput(bins.mNcoInputBlkSize);

#if 1
#pragma region compute Recv Bark-OPRF

				//####################
				//#######Recv role
				//####################
				auto& otRecv = *mOtRecvs[tIdx];

				auto otCountRecv = bins.mCuckooBins.mBins.size();
				// get the region of the base OTs that this thread should do.
				auto binStart = tIdx       * otCountRecv / thrds.size();
				auto binEnd = (tIdx + 1) * otCountRecv / thrds.size();

				for (u64 bIdx = binStart; bIdx < binEnd;)
				{
					u64 currentStepSize = std::min(stepSize, binEnd - bIdx);

					for (u64 stepIdx = 0; stepIdx < currentStepSize; ++bIdx, ++stepIdx)
					{
						auto& bin = bins.mCuckooBins.mBins[bIdx];

						if (!bin.isEmpty())
						{
							u64 inputIdx = bin.idx();

							for (u64 j = 0; j < ncoInput.size(); ++j)
								ncoInput[j] = bins.mNcoInputBuff[j][inputIdx];

							otRecv.encode(
								bIdx,      // input
								ncoInput,             // input
								bin.mValOPRF[IdxP]); // output
						}
						else
							otRecv.zeroEncode(bIdx);
					}
					otRecv.sendCorrection(chl, currentStepSize);
				}

				if (tIdx == 0) gTimer.setTimePoint("online.recv.otRecv.finalOPRF");



#pragma endregion
#endif

#if 1
#pragma region compute Send Bark-OPRF				
				//####################
				//#######Sender role
				//####################
				if (isOtherDirectionGetOPRF) {
					auto& otSend = *mOtSends[tIdx];
					auto otCountSend = bins.mSimpleBins.mBins.size();

					binStart = tIdx       * otCountSend / thrds.size();
					binEnd = (tIdx + 1) * otCountSend / thrds.size();


					if (tIdx == 0) gTimer.setTimePoint("online.send.OT");

					for (u64 bIdx = binStart; bIdx < binEnd;)
					{
						u64 currentStepSize = std::min(stepSize, binEnd - bIdx);
						otSend.recvCorrection(chl, currentStepSize);

						for (u64 stepIdx = 0; stepIdx < currentStepSize; ++bIdx, ++stepIdx)
						{

							auto& bin = bins.mSimpleBins.mBins[bIdx];

							if (bin.mIdx.size() > 0)
							{
								bin.mValOPRF[IdxP].resize(bin.mIdx.size());

								//std::cout << "s-" << bIdx << ", ";
								for (u64 i = 0; i < bin.mIdx.size(); ++i)
								{

									u64 inputIdx = bin.mIdx[i];

									for (u64 j = 0; j < mNcoInputBlkSize; ++j)
									{
										ncoInput[j] = bins.mNcoInputBuff[j][inputIdx];
									}

									otSend.encode(
										bIdx, //each bin has 1 OT
										ncoInput,
										bins.mSimpleBins.mOprfs[IdxP][inputIdx][bin.hIdx[i]]);//put oprf by inputIdx
										//bin.mValOPRF[IdxP][i]);

								}
							}
						}
					}
					if (tIdx == 0) gTimer.setTimePoint("online.send.otSend.finalOPRF");
					otSend.check(chl);
				}
#pragma endregion
#endif
				otRecv.check(chl);
			});
		}

		// join the threads.
		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
			thrds[tIdx].join();

		gTimer.setTimePoint("online.recv.exit");

		//std::cout << gTimer;
#endif
	}
	
	void  OPPRFReceiver::recvSSTableBased(u64 IdxP, binSet& bins, std::vector<block>& plaintexts, const std::vector<Channel*>& chls)
	{

		// this is the online phase.
		gTimer.setTimePoint("online.recv.start");

	


		std::vector<std::thread>  thrds(chls.size());
		// this mutex is used to guard inserting things into the intersection vector.
		std::mutex mInsertMtx;

		// fr each thread, spawn it.
		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
		{
			auto seed = mPrng.get<block>();
			thrds[tIdx] = std::thread([&, tIdx, seed]()
			{
				if (tIdx == 0) gTimer.setTimePoint("online.recv.thrdStart");

				auto& chl = *chls[tIdx];
				const u64 stepSize = 16;

				if (tIdx == 0) gTimer.setTimePoint("online.recv.recvShare");

				//2 type of bins: normal bin in inital step + stash bin
				for (auto bIdxType = 0; bIdxType < 2; bIdxType++)
				{
					auto binCountRecv = bins.mCuckooBins.mBinCount[bIdxType];
					bins.mMaskSize = roundUpTo(mStatSecParam + std::log(bins.mSimpleBins.mNumBits[bIdxType]), 8) / 8;


					u64 binStart, binEnd;
					if (bIdxType == 0)
					{
						binStart = tIdx       * binCountRecv / thrds.size();
						binEnd = (tIdx + 1) * binCountRecv / thrds.size();
					}
					else
					{
						binStart = tIdx       * binCountRecv / thrds.size() + bins.mCuckooBins.mBinCount[0];
						binEnd = (tIdx + 1) * binCountRecv / thrds.size() + bins.mCuckooBins.mBinCount[0];
					}

						

					//use the params of the simple hashing as their params
					u64 mTheirBins_mMaxBinSize = bins.mSimpleBins.mMaxBinSize[bIdxType];
					u64 mTheirBins_mNumBits= bins.mSimpleBins.mNumBits[bIdxType];
					for (u64 bIdx = binStart; bIdx < binEnd;)
					{
						u64 curStepSize = std::min(stepSize, binEnd - bIdx);

						MatrixView<u8> maskView;
						ByteStream maskBuffer;
						chl.recv(maskBuffer);
						//maskView = maskBuffer.getMatrixView<u8>(mTheirBins_mMaxBinSize * mMaskSize + mTheirBins_mNumBits * sizeof(u8));
						maskView = maskBuffer.getMatrixView<u8>(mTheirBins_mMaxBinSize * bins.mMaskSize + mTheirBins_mNumBits * sizeof(u8));
						if (maskView.size()[0] != curStepSize)
							throw std::runtime_error("size not expedted");

						for (u64 stepIdx = 0; stepIdx < curStepSize; ++bIdx, ++stepIdx)
						{

							auto& bin = bins.mCuckooBins.mBins[bIdx];
							if (!bin.isEmpty())
							{
								u64 baseMaskIdx = stepIdx;
								auto mask = maskView[baseMaskIdx];
								BaseOPPRF b;
								b.mMaxBitSize = mTheirBins_mNumBits;
								for (u64 i = 0; i < b.mMaxBitSize; i++)
								{
									int idxPos = 0;
									memcpy(&idxPos, maskView[baseMaskIdx].data() + i, sizeof(u8));
									b.mPos.push_back(idxPos);
								}
#ifdef PRINT
								Log::out << "RBin #" << bIdx << Log::endl;
								Log::out << "    cc_mPos= ";
								for (u64 idxPos = 0; idxPos < b.mPos.size(); idxPos++)
								{
									Log::out << static_cast<int16_t>(b.mPos[idxPos]) << " ";
								}
								Log::out << Log::endl;
#endif
								u64 inputIdx = bin.idx();
								auto myMask = bin.mValOPRF[IdxP];
								//	u8 myMaskPos = 0;
								b.getMask(myMask, bin.mValMap[IdxP]);

								u64	MaskIdx = bin.mValMap[IdxP]* bins.mMaskSize + mTheirBins_mNumBits;

								auto theirMask = ZeroBlock;
								memcpy(&theirMask, maskView[baseMaskIdx].data() + MaskIdx, bins.mMaskSize);

								//if (!memcmp((u8*)&myMask, &theirMask, mMaskSize))
								//{
								//Log::out << "inputIdx: " << inputIdx << Log::endl;
								//	Log::out << "myMask: " << myMask << Log::endl;
								//Log::out << "theirMask: " << theirMask << " " << Log::endl;		

								plaintexts[inputIdx] = myMask^theirMask;


								//}
							}
						}
					}
				}
							

			});
		//	if (tIdx == 0) gTimer.setTimePoint("online.recv.done");
		}
		// join the threads.
		for (auto& thrd : thrds)
			thrd.join();
		
		// check that the number of inputs is as expected.
		if (plaintexts.size() != mN)
			throw std::runtime_error(LOCATION);

		

	}
	void  OPPRFReceiver::recvSSPolyBased(u64 IdxP, binSet& bins, std::vector<block>& plaintexts, const std::vector<Channel*>& chls)
	{	

		// this is the online phase.
		gTimer.setTimePoint("online.recv.start");


		std::vector<std::thread>  thrds(chls.size());
		// this mutex is used to guard inserting things into the intersection vector.
		std::mutex mInsertMtx;
		
		BaseOPPRF poly;


		// fr each thread, spawn it.
		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
		{
			auto seed = mPrng.get<block>();
			thrds[tIdx] = std::thread([&, tIdx, seed]()
			{
				if (tIdx == 0) gTimer.setTimePoint("online.recv.thrdStart");

				auto& chl = *chls[tIdx];
				const u64 stepSize = 16;

				if (tIdx == 0) gTimer.setTimePoint("online.recv.recvShare");

				//2 type of bins: normal bin in inital step + stash bin
				bins.mMaskSize = roundUpTo(mStatSecParam + std::log(bins.mSimpleBins.mMaxBinSize[1]) - 1, 8) / 8;
				poly.poly_init(bins.mMaskSize);
				for (auto bIdxType = 0; bIdxType < 2; bIdxType++)
				{
					auto binCountRecv = bins.mCuckooBins.mBinCount[bIdxType];
					

					
					u64 binStart, binEnd;
					if (bIdxType == 0)
					{
						binStart = tIdx       * binCountRecv / thrds.size();
						binEnd = (tIdx + 1) * binCountRecv / thrds.size();
					}
					else
					{
						binStart = tIdx       * binCountRecv / thrds.size() + bins.mCuckooBins.mBinCount[0];
						binEnd = (tIdx + 1) * binCountRecv / thrds.size() + bins.mCuckooBins.mBinCount[0];

					}

					
					
					//use the params of the simple hashing as their params
					u64 mTheirBins_mMaxBinSize = bins.mSimpleBins.mMaxBinSize[bIdxType];

					for (u64 bIdx = binStart; bIdx < binEnd;)
					{
						u64 curStepSize = std::min(stepSize, binEnd - bIdx);

						MatrixView<u8> maskView;
						ByteStream maskBuffer;
						chl.recv(maskBuffer);

						maskView = maskBuffer.getMatrixView<u8>(mTheirBins_mMaxBinSize*bins.mMaskSize);

						if (maskView.size()[0] != curStepSize)
							throw std::runtime_error("size not expedted");

						for (u64 stepIdx = 0; stepIdx < curStepSize; ++bIdx, ++stepIdx)
						{

							auto& bin = bins.mCuckooBins.mBins[bIdx];
							if (!bin.isEmpty())
							{
								bin.mCoeffs[IdxP].resize(mTheirBins_mMaxBinSize);

								u64 baseMaskIdx = stepIdx;

								u64 inputIdx = bin.idx();
								
								//compute p(x*)

								for (u64 i = 0; i < mTheirBins_mMaxBinSize; i++)
								{
									memcpy(&bin.mCoeffs[IdxP][i],maskView[baseMaskIdx].data()+i*bins.mMaskSize, bins.mMaskSize);

									if (bIdx == 0 && i==3)
									{
										Log::out << "r["<< IdxP<<"]-coeffs[" << i << "] #" << bin.mCoeffs[IdxP][i] << Log::endl;
										
									}
								}
							
								block blkY;
								poly.evalPolynomial(bin.mCoeffs[IdxP], bin.mValOPRF[IdxP], blkY);

								plaintexts[inputIdx] = bin.mValOPRF[IdxP] ^ blkY;

								/*if (bIdx == 0)
								{
									std::cout << "r["<<IdxP<<"]-bin.mValOPRF[" << bIdx << "] " << bin.mValOPRF[IdxP];
									std::cout << "-----------" << blkY << std::endl;
								}*/

							}
						}
					}
				}


			});
			//	if (tIdx == 0) gTimer.setTimePoint("online.recv.done");
		}
		// join the threads.
		for (auto& thrd : thrds)
			thrd.join();

		// check that the number of inputs is as expected.
		//if (plaintexts.size() != mN)
		//	throw std::runtime_error(LOCATION);
	

	}
	void OPPRFReceiver::recvFullPolyBased(u64 IdxP, binSet& bins, std::vector<block>& plaintexts, const std::vector<Channel*>& chls)
	{

		// this is the online phase.
		gTimer.setTimePoint("online.recv.start");

		 
		u32 numHashes = bins.mSimpleBins.mNumHashes[0] + bins.mSimpleBins.mNumHashes[1];

		bins.mMaskSize= roundUpTo(mStatSecParam + 2 * std::log(mN) - 1, 8) / 8;

		if (bins.mMaskSize > sizeof(block))
			throw std::runtime_error("masked are stored in blocks, so they can exceed that size");


		std::vector<std::thread>  thrds(chls.size());
		// this mutex is used to guard inserting things into the intersection vector.
		std::mutex mInsertMtx;
		
		auto& chl = *chls[0];

		ByteStream maskBuffer;
		chl.recv(maskBuffer);
		BaseOPPRF b;
		b.poly_init(bins.mMaskSize);
		NTL::GF2E e;
		std::vector<NTL::GF2EX> polynomial(numHashes);

		auto maskView = maskBuffer.getMatrixView<u8>(bins.mMaskSize);

			//std::cout << "maskView.size()" << maskView.size()[0] << "\n";
			//std::cout << "totalMask: " << totalMask << "\n";

		//std::cout << "\nr[" << IdxP << "]-coeffs[3]" << maskView[3] << "\n";

		block blkCoff;
		for (u64 hIdx = 0; hIdx < numHashes; ++hIdx)
			for (u64 i = 0; i < mN; ++i)
			{

				memcpy(&blkCoff, maskView[hIdx*mN+i].data() , bins.mMaskSize);
				if(i==3 && hIdx==1)
					std::cout << "\nr[" << IdxP << "]-coeffs][1][3]" << blkCoff << "\n";
				b.GF2EFromBlock(e, blkCoff, bins.mMaskSize);
				NTL::SetCoeff(polynomial[hIdx], i, e); //build res_polynomial
			}

#if 1

		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
		{
			auto seed = mPrng.get<block>();
			thrds[tIdx] = std::thread([&, tIdx, seed]()
			{
				if (tIdx == 0) gTimer.setTimePoint("online.recv.thrdStart");

				auto& chl = *chls[tIdx];
				const u64 stepSize = 16;

				if (tIdx == 0) gTimer.setTimePoint("online.recv.recvShare");

				//2 type of bins: normal bin in inital step + stash bin
				for (auto bIdxType = 0; bIdxType < 2; bIdxType++)
				{
					auto binCountRecv = bins.mCuckooBins.mBinCount[bIdxType];

					u64 binStart, binEnd;
					if (bIdxType == 0)
					{
						binStart = tIdx       * binCountRecv / thrds.size();
						binEnd = (tIdx + 1) * binCountRecv / thrds.size();
					}
					else
					{
						binStart = tIdx       * binCountRecv / thrds.size() + bins.mCuckooBins.mBinCount[0];
						binEnd = (tIdx + 1) * binCountRecv / thrds.size() + bins.mCuckooBins.mBinCount[0];
					}


					for (u64 bIdx = binStart; bIdx < binEnd;)
					{
						u64 curStepSize = std::min(stepSize, binEnd - bIdx);

						for (u64 stepIdx = 0; stepIdx < curStepSize; ++bIdx, ++stepIdx)
						{
							auto& bin = bins.mCuckooBins.mBins[bIdx];
							if (!bin.isEmpty())
							{
								u64 inputIdx = bin.idx();
								u64 hIdx = bin.hashIdx();
								block blkY;
								b.GF2EFromBlock(e, bins.mXsets[inputIdx], bins.mMaskSize);
								e = NTL::eval(polynomial[hIdx], e); //get y=f(x) in GF2E
								b.BlockFromGF2E(blkY, e, bins.mMaskSize);

								if (inputIdx == 0)
								{
									std::cout << "inputIdx[" << inputIdx << "]-hIdx[" << hIdx << "]-OPRF" << bin.mValOPRF[IdxP];
									std::cout << "\n----" << blkY << std::endl;
								}
								plaintexts[inputIdx] = bin.mValOPRF[IdxP] ^ blkY;
							}
						}
					}
				}


			});
			//	if (tIdx == 0) gTimer.setTimePoint("online.recv.done");
		}
		// join the threads.
		for (auto& thrd : thrds)
			thrd.join();

#endif // 0


	}
	void  OPPRFReceiver::recvBFBased(u64 IdxP, binSet& bins, std::vector<block>& plaintexts, const std::vector<Channel*>& chls)
	{
		
		// this is the online phase.
		gTimer.setTimePoint("online.recv.start");

		//u64 mMaskSize = sizeof(block);// roundUpTo(mStatSecParam + 2 * std::log(mN) - 1, 8) / 8;

		u64 nNN = mN * (bins.mCuckooBins.mParams.mNumHashes[0] + bins.mCuckooBins.mParams.mNumHashes[1]);
		mBfSize = mNumBFhashs * nNN / std::log(2);
		
		bins.mMaskSize = roundUpTo(mStatSecParam + std::log(mN) + std::log(nNN) - 1, 8) / 8;		

		
		//u64 mMaskSize = sizeof(block);

		if (bins.mMaskSize > sizeof(block))
			throw std::runtime_error("masked are stored in blocks, so they can exceed that size");

		
		

		std::vector<std::thread>  thrds(chls.size());
		// this mutex is used to guard inserting things into the intersection vector.
		std::mutex mInsertMtx;

		auto& chl = *chls[0];

		ByteStream maskBuffer;
		chl.recv(maskBuffer);

		auto maskBFView = maskBuffer.getMatrixView<u8>(bins.mMaskSize);

		std::cout << "\nr[" << IdxP << "]-maskBFView.size() " << maskBFView.size()[0] << "\n";
	//	std::cout << "\nr[" << IdxP << "]-mBfBitCount " << mBfSize << "\n";
		//std::cout << "totalMask: " << totalMask << "\n";

	//	std::cout << "\nr[" << IdxP << "]-maskBFView[3]" << maskBFView[3] << "\n";

#if 1

		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
		{
			auto seed = mPrng.get<block>();
			thrds[tIdx] = std::thread([&, tIdx, seed]()
			{
				if (tIdx == 0) gTimer.setTimePoint("online.recv.thrdStart");

				auto& chl = *chls[tIdx];
				const u64 stepSize = 16;

				if (tIdx == 0) gTimer.setTimePoint("online.recv.recvShare");

				//2 type of bins: normal bin in inital step + stash bin
				for (auto bIdxType = 0; bIdxType < 2; bIdxType++)
				{
					auto binCountRecv = bins.mCuckooBins.mBinCount[bIdxType];

					u64 binStart, binEnd;
					if (bIdxType == 0)
					{
						binStart = tIdx       * binCountRecv / thrds.size();
						binEnd = (tIdx + 1) * binCountRecv / thrds.size();
					}
					else
					{
						binStart = tIdx       * binCountRecv / thrds.size() + bins.mCuckooBins.mBinCount[0];
						binEnd = (tIdx + 1) * binCountRecv / thrds.size() + bins.mCuckooBins.mBinCount[0];
					}


					for (u64 bIdx = binStart; bIdx < binEnd;)
					{
						u64 curStepSize = std::min(stepSize, binEnd - bIdx);

						for (u64 stepIdx = 0; stepIdx < curStepSize; ++bIdx, ++stepIdx)
						{
							auto& bin = bins.mCuckooBins.mBins[bIdx];
							if (!bin.isEmpty())
							{								
								u64 inputIdx = bin.idx();
								
								block blkY=ZeroBlock;

								//NOTE that it is fine to compute BF on (oprf(x),y) as long as receiver reconstruct y*=BF(oprf(x*))
								for (u64 hashIdx = 0; hashIdx < mBFHasher.size(); ++hashIdx)
								{
									block hashOut = mBFHasher[hashIdx].ecbEncBlock(bin.mValOPRF[IdxP]);
									u64& idx = *(u64*)&hashOut;
									idx %= mBfSize;
									auto theirBFMask = ZeroBlock;
									memcpy(&theirBFMask, maskBFView[idx].data(), bins.mMaskSize);

									blkY = blkY ^ theirBFMask;
								}								

								if (bIdx == 0)
								{
									std::cout << "r[" << IdxP << "]-bin.mValOPRF[" << bIdx << "] " << bin.mValOPRF[IdxP];
									std::cout << "-----------" << blkY << std::endl;
								}
								plaintexts[inputIdx] = bin.mValOPRF[IdxP] ^ blkY;
							}
						}
					}
				}


			});
			//	if (tIdx == 0) gTimer.setTimePoint("online.recv.done");
		}
		// join the threads.
		for (auto& thrd : thrds)
			thrd.join();

#endif // 0


	}

	void  OPPRFReceiver::sendSSTableBased(u64 IdxP, binSet& bins, std::vector<block>& plaintexts, const std::vector<Channel*>& chls)
	{
		if (plaintexts.size() != mN)
			throw std::runtime_error(LOCATION);

		std::vector<std::thread>  thrds(chls.size());
		// std::vector<std::thread>  thrds(1);        

		std::mutex mtx;


		gTimer.setTimePoint("online.send.spaw");

		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
		{
			auto seed = mPrng.get<block>();
			thrds[tIdx] = std::thread([&, tIdx, seed]() {

				PRNG prng(seed);

				if (tIdx == 0) gTimer.setTimePoint("online.send.thrdStart");

				auto& chl = *chls[tIdx];
				const u64 stepSize = 16;

#pragma region sendShare
#if 1
				if (tIdx == 0) gTimer.setTimePoint("online.send.sendShare");

				//2 type of bins: normal bin in inital step + stash bin
				for (auto bIdxType = 0; bIdxType < 2; bIdxType++)
				{
					bins.mMaskSize = roundUpTo(mStatSecParam + std::log(bins.mSimpleBins.mNumBits[bIdxType]), 8) / 8;


					auto binCountSend = bins.mSimpleBins.mBinCount[bIdxType];
					u64 binStart, binEnd;
					if (bIdxType == 0)
					{
						binStart = tIdx       * binCountSend / thrds.size();
						binEnd = (tIdx + 1) * binCountSend / thrds.size();
					}
					else
					{
						binStart = tIdx       * binCountSend / thrds.size() + bins.mSimpleBins.mBinCount[0];
						binEnd = (tIdx + 1) * binCountSend / thrds.size() + bins.mSimpleBins.mBinCount[0];
					}

					if (tIdx == 0) gTimer.setTimePoint("online.send.masks.init.step");

					for (u64 bIdx = binStart; bIdx < binEnd;)
					{
						u64 currentStepSize = std::min(stepSize, binEnd - bIdx);
						uPtr<Buff> sendMaskBuff(new Buff);
						sendMaskBuff->resize(currentStepSize * (bins.mSimpleBins.mMaxBinSize[bIdxType] * bins.mMaskSize + bins.mSimpleBins.mNumBits[bIdxType] * sizeof(u8)));
						auto maskView = sendMaskBuff->getMatrixView<u8>(bins.mSimpleBins.mMaxBinSize[bIdxType] * bins.mMaskSize + bins.mSimpleBins.mNumBits[bIdxType] * sizeof(u8));

						for (u64 stepIdx = 0; stepIdx < currentStepSize; ++bIdx, ++stepIdx)
						{
							//Log::out << "sBin #" << bIdx << Log::endl;

							auto& bin = bins.mSimpleBins.mBins[bIdx];
							u64 baseMaskIdx = stepIdx;
							int MaskIdx = 0;

							if (bin.mIdx.size() > 0)
							{
								//copy bit locations in which all OPRF values are distinct
								//	Log::out << "    c_mPos= ";

								if (bin.mBits[IdxP].mPos.size() != bins.mSimpleBins.mNumBits[bIdxType])
								{
#ifdef PRINT
									Log::out << "bin.mBits[IdxP].mPos.size() != bins.mSimpleBins.mNumBits[bIdxType]" << Log::endl;
									Log::out << "Party: " << IdxP << Log::endl;
									Log::out << "bIdx: " << bIdx << Log::endl;
									Log::out << "bin.mBits[IdxP].mPos.size(): " << bin.mBits[IdxP].mPos.size() << Log::endl;
									Log::out << "mSimpleBins.mNumBits[bIdxType]: " << bins.mSimpleBins.mNumBits[bIdxType] << Log::endl;
#endif // PRINT
									throw std::runtime_error("bin.mBits.mPos.size()!= mBins.mNumBits");
								}
								//copy bit positions
								for (u64 idxPos = 0; idxPos < bin.mBits[IdxP].mPos.size(); idxPos++)
								{
									//	Log::out << static_cast<int16_t>(bin.mBits[IdxP].mPos[idxPos]) << " ";
									memcpy(
										maskView[baseMaskIdx].data() + idxPos,
										(u8*)&bin.mBits[IdxP].mPos[idxPos], sizeof(u8));
							}
								//Log::out << Log::endl;

								for (u64 i = 0; i < bin.mIdx.size(); ++i)
								{
									u64 inputIdx = bin.mIdx[i];
									block encr = bin.mValOPRF[IdxP][i] ^ plaintexts[inputIdx];

									//Log::out << "    c_idx=" << inputIdx;
									//Log::out << "    c_OPRF=" << encr;
									//Log::out << "    c_Map=" << static_cast<int16_t>(bin.mBits.mMaps[i]);

									MaskIdx = bin.mBits[IdxP].mMaps[i] * bins.mMaskSize + bins.mSimpleBins.mNumBits[bIdxType];

									memcpy(
										maskView[baseMaskIdx].data() + MaskIdx,
										(u8*)&encr,
										bins.mMaskSize);

									//	Log::out << Log::endl;
								}

								//#####################
								//######Filling dummy mask
								//#####################

								for (u64 i = 0; i < bins.mSimpleBins.mMaxBinSize[bIdxType]; ++i)
								{
									if (std::find(bin.mBits[IdxP].mMaps.begin(), bin.mBits[IdxP].mMaps.end(), i) == bin.mBits[IdxP].mMaps.end())
									{
										MaskIdx = i* bins.mMaskSize + bins.mSimpleBins.mNumBits[bIdxType];
										//	Log::out << "    cc_Map=" << i << Log::endl;
										memcpy(
											maskView[baseMaskIdx].data() + MaskIdx,
											(u8*)&ZeroBlock,  //make randome
											bins.mMaskSize);
									}
								}
						}
							else //pad all dummy
							{
								//bit positions
								std::vector<u8> dummyPos;
								auto idxDummyPos = 0;
								while (dummyPos.size()<bins.mSimpleBins.mNumBits[bIdxType])
								{
									u64 rand = std::rand() % 128; //choose randome bit location
									if (std::find(dummyPos.begin(), dummyPos.end(), rand) == dummyPos.end())
									{
										dummyPos.push_back(rand);
										memcpy(
											maskView[baseMaskIdx].data() + idxDummyPos,
											(u8*)&rand, sizeof(u8));
										idxDummyPos++;
									}
								}

								for (u64 i = 0; i < bins.mSimpleBins.mMaxBinSize[bIdxType]; ++i)
								{
									MaskIdx = i* bins.mMaskSize + bins.mSimpleBins.mNumBits[bIdxType];
									//	Log::out << "    cc_Map=" << i << Log::endl;
									memcpy(
										maskView[baseMaskIdx].data() + MaskIdx,
										(u8*)&ZeroBlock,  //make randome
										bins.mMaskSize);

								}

							}


					}

#ifdef PRINT
						Log::out << "maskSize: ";
						for (size_t i = 0; i < maskView.size()[0]; i++)
						{
							for (size_t j = 0; j < mSimpleBins.mNumBits[bIdxType]; j++)
							{
								Log::out << static_cast<int16_t>(maskView[i][j]) << " ";
							}
							Log::out << Log::endl;

							for (size_t j = 0; j < mSimpleBins.mMaxBinSize[bIdxType]; j++) {
								auto theirMask = ZeroBlock;
								memcpy(&theirMask, maskView[i].data() + j*maskSize + mSimpleBins.mNumBits[bIdxType], maskSize);
								if (theirMask != ZeroBlock)
								{
									Log::out << theirMask << " " << Log::endl;
								}
							}
						}
#endif
						chl.asyncSend(std::move(sendMaskBuff));

					}
				}
				if (tIdx == 0) gTimer.setTimePoint("online.send.sendMask");

				//	otSend.check(chl);



				/* if (tIdx == 0)
				chl.asyncSend(std::move(sendMaskBuff));*/

				if (tIdx == 0) gTimer.setTimePoint("online.send.finalMask");
#endif
#pragma endregion

							});
						}

		for (auto& thrd : thrds)
			thrd.join();

		//    permThrd.join();



				}
	void  OPPRFReceiver::sendSSPolyBased(u64 IdxP, binSet& bins, std::vector<block>& plaintexts, const std::vector<Channel*>& chls)
	{
		if (plaintexts.size() != mN)
			throw std::runtime_error(LOCATION);

		std::vector<std::thread>  thrds(chls.size());
		// std::vector<std::thread>  thrds(1);        

		BaseOPPRF mPoly;

		std::mutex mtx;
		NTL::vec_GF2E x; NTL::vec_GF2E y;
		NTL::GF2E e;

		gTimer.setTimePoint("online.send.spaw");

		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
		{
			auto seed = mPrng.get<block>();
			thrds[tIdx] = std::thread([&, tIdx, seed]() {

				PRNG prng(seed);

				if (tIdx == 0) gTimer.setTimePoint("online.send.thrdStart");

				auto& chl = *chls[tIdx];
				const u64 stepSize = 16;

#pragma region sendShare
#if 1
				if (tIdx == 0) gTimer.setTimePoint("online.send.sendShare");

				//2 type of bins: normal bin in inital step + stash bin

				bins.mMaskSize = roundUpTo(mStatSecParam + std::log(bins.mSimpleBins.mMaxBinSize[1]) - 1, 8) / 8;
				mPoly.poly_init(bins.mMaskSize);

				for (auto bIdxType = 0; bIdxType < 2; bIdxType++)
				{


					auto binCountSend = bins.mSimpleBins.mBinCount[bIdxType];
					u64 binStart, binEnd;
					if (bIdxType == 0)
					{
						binStart = tIdx       * binCountSend / thrds.size();
						binEnd = (tIdx + 1) * binCountSend / thrds.size();

					}
					else
					{
						binStart = tIdx       * binCountSend / thrds.size() + bins.mSimpleBins.mBinCount[0];
						binEnd = (tIdx + 1) * binCountSend / thrds.size() + bins.mSimpleBins.mBinCount[0];

					}


					if (tIdx == 0) gTimer.setTimePoint("online.send.masks.init.step");

					for (u64 bIdx = binStart; bIdx < binEnd;)
					{
						u64 currentStepSize = std::min(stepSize, binEnd - bIdx);
						uPtr<Buff> sendMaskBuff(new Buff);
						sendMaskBuff->resize(currentStepSize * (bins.mSimpleBins.mMaxBinSize[bIdxType] * bins.mMaskSize));
						auto maskView = sendMaskBuff->getMatrixView<u8>(bins.mSimpleBins.mMaxBinSize[bIdxType] * bins.mMaskSize);

						for (u64 stepIdx = 0; stepIdx < currentStepSize; ++bIdx, ++stepIdx)
						{
							//Log::out << "sBin #" << bIdx << Log::endl;

							auto& bin = bins.mSimpleBins.mBins[bIdx];
							u64 baseMaskIdx = stepIdx;
							int MaskIdx = 0;

							//	Log::out << "bin.mIdx[" << bIdx << "]: " <<  Log::endl;

							if (bin.mIdx.size() > 0)
							{

								//get y[i]
								std::vector<block> setY(bin.mIdx.size());
								for (u64 i = 0; i < bin.mIdx.size(); ++i)
								{
									u64 inputIdx = bin.mIdx[i];
									//NOTE that it is fine to compute p(oprf(x[i]))=y[i] as long as receiver reconstruct y*=p(oprf(x*))

									setY[i] = plaintexts[inputIdx] ^ bin.mValOPRF[IdxP][i];
									if (bIdx == 0)
									{
										std::cout << "s bin.mValOPRF[" << bIdx << "] " << bin.mValOPRF[IdxP][i];
										std::cout << "-----------" << setY[i] << std::endl;
									}
								}

								std::vector<block> coeffs;
								//computes coefficients (in blocks) of p such that p(x[i]) = y[i]
								//NOTE that it is fine to compute p(oprf(x[i]))=y[i] as long as receiver reconstruct y*=p(oprf(x*))

								mPoly.getBlkCoefficients(bins.mSimpleBins.mMaxBinSize[bIdxType],
									bin.mValOPRF[IdxP], setY, coeffs);

								if (bIdx == 0)
								{
									//Log::out << "coeffs.size(): " << coeffs.size()<< Log::endl;

									for (u64 i = 0; i < bins.mSimpleBins.mMaxBinSize[bIdxType]; ++i)
										if (i == 3)
											Log::out << IdxP << "s-coeffs[" << i << "] #" << coeffs[i] << Log::endl;
								}

								//it already contain a dummy item
								for (u64 i = 0; i < bins.mSimpleBins.mMaxBinSize[bIdxType]; ++i)
								{
									memcpy(
										maskView[baseMaskIdx].data() + i* bins.mMaskSize,
										(u8*)&coeffs[i],  //make randome
														  //	(u8*)&ZeroBlock,  //make randome
										bins.mMaskSize);
								}

							}
							else //pad all dummy
							{
								for (u64 i = 0; i < bins.mSimpleBins.mMaxBinSize[bIdxType]; ++i)
								{
									memcpy(
										maskView[baseMaskIdx].data() + i* bins.mMaskSize,
										(u8*)&ZeroBlock,  //make randome
										bins.mMaskSize);
								}
							}
						}

#ifdef PRINT
						Log::out << "maskSize: ";
						for (size_t i = 0; i < maskView.size()[0]; i++)
						{
							for (size_t j = 0; j < mSimpleBins.mNumBits[bIdxType]; j++)
							{
								Log::out << static_cast<int16_t>(maskView[i][j]) << " ";
							}
							Log::out << Log::endl;

							for (size_t j = 0; j < mSimpleBins.mMaxBinSize[bIdxType]; j++) {
								auto theirMask = ZeroBlock;
								memcpy(&theirMask, maskView[i].data() + j*maskSize + mSimpleBins.mNumBits[bIdxType], maskSize);
								if (theirMask != ZeroBlock)
								{
									Log::out << theirMask << " " << Log::endl;
								}
							}
						}
#endif
						chl.asyncSend(std::move(sendMaskBuff));

					}
				}
				if (tIdx == 0) gTimer.setTimePoint("online.send.sendMask");

				//	otSend.check(chl);



				/* if (tIdx == 0)
				chl.asyncSend(std::move(sendMaskBuff));*/

				if (tIdx == 0) gTimer.setTimePoint("online.send.finalMask");
#endif
#pragma endregion

			});
		}

		for (auto& thrd : thrds)
			thrd.join();

		//    permThrd.join();



	}
	void  OPPRFReceiver::sendFullPolyBased(u64 IdxP, binSet& bins, std::vector<block>& plaintexts, const std::vector<Channel*>& chls)
	{
		if (plaintexts.size() != mN)
			throw std::runtime_error(LOCATION);

		bins.mMaskSize = roundUpTo(mStatSecParam + 2 * std::log(mN) + std::log(bins.mSimpleBins.mNumHashes[0] + bins.mSimpleBins.mNumHashes[1]) - 1, 8) / 8;

		if (bins.mMaskSize > sizeof(block))
			throw std::runtime_error("masked are stored in blocks, so they can exceed that size");

		std::vector<std::thread>  thrds(chls.size());
		// std::vector<std::thread>  thrds(1);        

		std::mutex mtx;
		NTL::vec_GF2E vec_GF2E_X; NTL::vec_GF2E vec_GF2E_Y;
		u64 size_vec_GF2E_X = 0;
		NTL::GF2E e;
		BaseOPPRF base_poly;
		base_poly.poly_init(bins.mMaskSize);

		gTimer.setTimePoint("online.send.spaw");

		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
		{
			auto seed = mPrng.get<block>();
			thrds[tIdx] = std::thread([&, tIdx, seed]() {

				PRNG prng(seed);

				if (tIdx == 0) gTimer.setTimePoint("online.send.thrdStart");

				auto& chl = *chls[tIdx];
				const u64 stepSize = 16;

#pragma region sendShare
#if 1
				if (tIdx == 0) gTimer.setTimePoint("online.send.sendShare");

				//2 type of bins: normal bin in inital step + stash bin
				for (auto bIdxType = 0; bIdxType < 2; bIdxType++)
				{
					auto binCountSend = bins.mSimpleBins.mBinCount[bIdxType];
					u64 binStart, binEnd;
					if (bIdxType == 0)
					{
						binStart = tIdx       * binCountSend / thrds.size();
						binEnd = (tIdx + 1) * binCountSend / thrds.size();
					}
					else
					{
						binStart = tIdx       * binCountSend / thrds.size() + bins.mSimpleBins.mBinCount[0];
						binEnd = (tIdx + 1) * binCountSend / thrds.size() + bins.mSimpleBins.mBinCount[0];
					}

					if (tIdx == 0) gTimer.setTimePoint("online.send.masks.init.step");

					for (u64 bIdx = binStart; bIdx < binEnd;)
					{
						u64 currentStepSize = std::min(stepSize, binEnd - bIdx);

						for (u64 stepIdx = 0; stepIdx < currentStepSize; ++bIdx, ++stepIdx)
						{
							//Log::out << "sBin #" << bIdx << Log::endl;

							auto& bin = bins.mSimpleBins.mBins[bIdx];
							u64 baseMaskIdx = stepIdx;
							int MaskIdx = 0;

							//	Log::out << "bin.mIdx[" << bIdx << "]: " <<  Log::endl;

							if (bin.mIdx.size() > 0)
							{

								for (u64 i = 0; i < bin.mIdx.size(); ++i)
								{
									u64 inputIdx = bin.mIdx[i];
									//NOTE that it is fine to compute p(oprf(x[i]))=y[i] as long as receiver reconstruct y*=p(oprf(x*))

									block y = plaintexts[inputIdx] ^ bin.mValOPRF[IdxP][i];
									base_poly.GF2EFromBlock(e, y, bins.mMaskSize);

									//TODO: current test is single thread, make safe when running multi-thread
									vec_GF2E_Y.append(e);

									base_poly.GF2EFromBlock(e, bin.mValOPRF[IdxP][i], bins.mMaskSize);

									//TODO: current test is single thread, make safe when running multi-thread
									vec_GF2E_X.append(e);
									size_vec_GF2E_X++;

									if (bIdx == 0)
									{
										std::cout << "s bin.mValOPRF[" << bIdx << "] " << bin.mValOPRF[IdxP][i];
										std::cout << "-----------" << y << std::endl;
									}
								}
							}

						}

					}
				}

				if (tIdx == 0) gTimer.setTimePoint("online.compute x y");
#endif
#pragma endregion

			});
		}

		for (auto& thrd : thrds)
			thrd.join();
		std::cout << bins.mN << " - " << bins.mSimpleBins.mNumHashes[0] << " " << bins.mSimpleBins.mNumHashes[1] << "\n";

		u64 totalMask = bins.mN*(bins.mSimpleBins.mNumHashes[0] + bins.mSimpleBins.mNumHashes[1]);

		//ADDING DUMMY
		//because 2 h(x1) and h(x2) might have the same value
		for (u32 i = 0; i <totalMask - size_vec_GF2E_X; i++)
		{
			NTL::random(e);
			vec_GF2E_X.append(e);
			NTL::random(e);
			vec_GF2E_Y.append(e);
		}


		//get Blk Coefficients and send it to receiver
		std::vector<block> coeffs;
		//computes coefficients (in blocks) of p such that p(x[i]) = y[i]
		//NOTE that it is fine to compute p(oprf(x[i]))=y[i] as long as receiver reconstruct y*=p(oprf(x*))

		base_poly.getBlkCoefficients(vec_GF2E_X, vec_GF2E_Y, coeffs);

		//	std::cout << "coeffs.size()" << coeffs.size() << "\n";		
		//	std::cout << "totalMask: " << totalMask << "\n";


		uPtr<Buff> sendMaskBuff(new Buff);
		sendMaskBuff->resize(totalMask* bins.mMaskSize);
		auto maskView = sendMaskBuff->getMatrixView<u8>(totalMask * bins.mMaskSize);

#if 1

		//it already contain a dummy item
		for (u64 i = 0; i < coeffs.size(); ++i)
		{
			memcpy(
				maskView[0].data() + i* bins.mMaskSize,
				(u8*)&coeffs[i],  //make randome
								  //(u8*)&ZeroBlock,  //make randome
				bins.mMaskSize);
		}
		std::cout << "s[" << IdxP << "]-coeffs[3]" << coeffs[3] << "\n";


		auto& chl = *chls[0];
		chl.asyncSend(std::move(sendMaskBuff));

#endif // 0


		}
	void  OPPRFReceiver::sendBFBased(u64 IdxP, binSet& bins, std::vector<block>& plaintexts, const std::vector<Channel*>& chls)
	{
		if (plaintexts.size() != mN)
			throw std::runtime_error(LOCATION);

		u64 extend_mN = mN * (bins.mCuckooBins.mParams.mNumHashes[0] + bins.mCuckooBins.mParams.mNumHashes[1]);
		mBfSize = mNumBFhashs * extend_mN / std::log(2);

		bins.mMaskSize = roundUpTo(mStatSecParam + std::log(mN) + std::log(extend_mN) - 1, 8) / 8;

		//TODO: double check
		//	u64 maskSize = sizeof(block);//roundUpTo(mStatSecParam + 2 * std::log(mN) - 1, 8) / 8;
		//u64 maskSize = sizeof(block); //roundUpTo(mStatSecParam + std::log(mN) + std::log(mBfSize) - 1, 8) / 8;

		//u64 maskSize = 7;
		if (bins.mMaskSize > sizeof(block))
			throw std::runtime_error("masked are stored in blocks, so they can exceed that size");

		std::vector<std::thread>  thrds(chls.size());
		// std::vector<std::thread>  thrds(1);        





		uPtr<Buff> sendMaskBuff(new Buff);
		sendMaskBuff->resize(mBfSize* bins.mMaskSize);
		auto maskBFView = sendMaskBuff->getMatrixView<u8>(bins.mMaskSize);

		std::vector<block> GarbleBF(mBfSize);

		gTimer.setTimePoint("online.send.spaw");

		for (u64 tIdx = 0; tIdx < thrds.size(); ++tIdx)
		{
			auto seed = mPrng.get<block>();
			thrds[tIdx] = std::thread([&, tIdx, seed]() {

				PRNG prng(seed);

				if (tIdx == 0) gTimer.setTimePoint("online.send.thrdStart");

				auto& chl = *chls[tIdx];
				const u64 stepSize = 16;

#pragma region sendShare
#if 1
				if (tIdx == 0) gTimer.setTimePoint("online.send.sendShare");

				//2 type of bins: normal bin in inital step + stash bin
				for (auto bIdxType = 0; bIdxType < 2; bIdxType++)
				{
					auto binCountSend = bins.mSimpleBins.mBinCount[bIdxType];
					u64 binStart, binEnd;
					if (bIdxType == 0)
					{
						binStart = tIdx       * binCountSend / thrds.size();
						binEnd = (tIdx + 1) * binCountSend / thrds.size();
					}
					else
					{
						binStart = tIdx       * binCountSend / thrds.size() + bins.mSimpleBins.mBinCount[0];
						binEnd = (tIdx + 1) * binCountSend / thrds.size() + bins.mSimpleBins.mBinCount[0];
					}

					if (tIdx == 0) gTimer.setTimePoint("online.send.masks.init.step");

					for (u64 bIdx = binStart; bIdx < binEnd;)
					{
						u64 currentStepSize = std::min(stepSize, binEnd - bIdx);

						for (u64 stepIdx = 0; stepIdx < currentStepSize; ++bIdx, ++stepIdx)
						{
							//Log::out << "sBin #" << bIdx << Log::endl;

							auto& bin = bins.mSimpleBins.mBins[bIdx];
							u64 baseMaskIdx = stepIdx;
							int MaskIdx = 0;

							//	Log::out << "bin.mIdx[" << bIdx << "]: " <<  Log::endl;

							if (bin.mIdx.size() > 0)
							{
								std::set<u64> idxs;
								for (u64 i = 0; i < bin.mIdx.size(); ++i)
								{
									u64 inputIdx = bin.mIdx[i];
									u64 firstFreeIdx(-1);
									block sum = ZeroBlock;
									idxs.clear();

									//NOTE that it is fine to compute BF on (oprf(x),y) as long as receiver reconstruct y*=BF(oprf(x*))
									for (u64 hashIdx = 0; hashIdx < mBFHasher.size(); ++hashIdx)
									{
										block hashOut = mBFHasher[hashIdx].ecbEncBlock(bin.mValOPRF[IdxP][i]);
										u64& idx = *(u64*)&hashOut;
										idx %= mBfSize;
										idxs.emplace(idx);
									}

									for (auto idx : idxs)
									{
										if (eq(GarbleBF[idx], ZeroBlock))
										{
											if (firstFreeIdx == u64(-1))
											{
												firstFreeIdx = idx;
												//	std::cout << "firstFreeIdx: " << firstFreeIdx << std::endl;

											}
											else
											{
												GarbleBF[idx] = mPrng.get<block>();
												memcpy(maskBFView[idx].data(), (u8*)&GarbleBF[idx], bins.mMaskSize);
												//	std::cout << garbledBF[idx] <<"\n";
												sum = sum ^ GarbleBF[idx];
												//std::cout << idx << " " << maskBFView[idx] << std::endl;
											}
										}
										else
										{
											sum = sum ^ GarbleBF[idx];
											//	std::cout << idx << " " << maskBFView[idx] << std::endl;
										}
									}

									GarbleBF[firstFreeIdx] = sum^plaintexts[inputIdx] ^ bin.mValOPRF[IdxP][i];
									memcpy(maskBFView[firstFreeIdx].data(), (u8*)&GarbleBF[firstFreeIdx], bins.mMaskSize);
								}
							}

						}
					}
				}

				if (tIdx == 0) gTimer.setTimePoint("online.compute x y");
#endif
#pragma endregion

			});
		}

		for (auto& thrd : thrds)
			thrd.join();

		for (u64 i = 0; i < GarbleBF.size(); ++i)
		{
			if (eq(GarbleBF[i], ZeroBlock))
			{

				GarbleBF[i] = mPrng.get<block>();
				memcpy(maskBFView[i].data(), (u8*)&GarbleBF[i], bins.mMaskSize);

			}
		}

		std::cout << "\ns[" << IdxP << "]-maskBFView.size() " << maskBFView.size()[0] << "\n";
		std::cout << "\ns[" << IdxP << "]-mBfSize " << mBfSize << "\n";

		//	std::cout << "\ns[" << IdxP << "]-maskBFView[3]" << maskBFView[3] << "\n";


#if 1

		auto& chl = *chls[0];
		chl.asyncSend(std::move(sendMaskBuff));

#endif // 0

	}


	
}