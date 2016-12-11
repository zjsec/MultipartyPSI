#include "OPPRF_Tests.h"

#include "Common.h"
#include "Network/BtEndpoint.h"
#include "Common/Defines.h"
#include "OPPRF/OPPRFReceiver.h"
#include "OPPRF/OPPRFSender.h"
#include "Common/Log.h"

#include "NChooseOne/KkrtNcoOtReceiver.h"
#include "NChooseOne/KkrtNcoOtSender.h"


#include "NChooseOne/Oos/OosNcoOtReceiver.h"
#include "NChooseOne/Oos/OosNcoOtSender.h"

#include "Hashing/CuckooHasher1.h"
#include "Hashing/BitPosition.h"
#include "Common/Log.h"
#include "Common/Log1.h"
#include <array>

using namespace osuCrypto;

void testPointer(std::vector<block>* test)
{
	//int length = test->size();
	//std::cout << length << std::endl;
	

	AES ncoInputHasher;
	
		ncoInputHasher.setKey(_mm_set1_epi64x(112434));
		ncoInputHasher.ecbEncBlocks((*test).data() , test->size() - 1, (*test).data() );
	//Log::out << "mHashingSeed: " << mHashingSeed << Log::endl;




}
void testPointer2(block * test, int size)
{
	PRNG prng(_mm_set_epi32(4253465, 3434565, 234435, 23987045));
	auto myHashSeed = prng.get<block>();

	AES ncoInputHasher;
	ncoInputHasher.setKey(myHashSeed);
//	for (u64 i = 0; i < size-1; i++)
//	{
		ncoInputHasher.ecbEncBlocks(test, size-1,test);
//	}

}

void Bit_Position_Test_Impl()
{
	u64 setSize = 1<<4;
	std::vector<block> testSet(setSize);
	PRNG prng(_mm_set_epi32(4253465, 3434565, 234435, 23987045));
	

	for (u64 i = 0; i < setSize; ++i)
	{
		testSet[i] = prng.get<block>();
	}
	for (u64 i = 0; i < setSize; ++i)
	{
		std::cout << testSet[i] << std::endl;
	}
	std::cout << std::endl;
	testPointer(&testSet);
	for (u64 i = 0; i < setSize; ++i)
	{
		std::cout << testSet[i] << std::endl;
	}
#if 0
	block test = ZeroBlock;
	test.m128i_i8[0] = 31;
	BitPosition b;
	b.init(5);
	for (int i = 0; i < 3; ++i) b.mPos.insert(i);
	b.mPos.insert(6);
	b.mPos.insert(7);
	std::cout << static_cast<int16_t>(b.map(test)) <<std::endl;
	std::cout << static_cast<int16_t>(b.map2(test));


	BitPosition b2;
	b2.init(setSize);
	//std::cout << "size: " << b2.mSize << std::endl;

	std::set<u8> masks;
//	b2.findPos(testSet, masks);
	//std::cout << "\nmNumTrial: " << b2.mNumTrial << std::endl;
	

	for (u8 i = 0; i < masks.size(); i++)
	{
		//std::cout << static_cast<int16_t>(masks[i]) << std::endl;
	}
#endif
}



void Bit_Position_Recursive_Test_Impl()
{
	u64 setSize = 1 << 4;
	std::vector<block> testSet(setSize);
	PRNG prng(_mm_set_epi32(4253465, 3434565, 234435, 23987045));

	for (u64 i = 0; i < setSize; ++i)
	{
		testSet[i] = prng.get<block>();
	}
	for (u64 i = 0; i < setSize; ++i)
	{
		//testSet[i].m128i_u8[i/8] = 1 << (i%8);
	}
	
	BitPosition b;

#if 0
	block test = ZeroBlock;
	//test.m128i_i8[0] = 126;
	
	BitPosition b;
	b.init(5);
	for (int i = 0; i < 3; ++i) b.mPos.insert(i);
	b.mPos.insert(6);
	b.mPos.insert(7);
	for (size_t i = 0; i < 8; i++)
	{
		b.setBit(test, i);
		std::cout << b.isSet(testSet[10], i) << " ";
	}
	/*std::vector<int> cnt(128);
	int idx = 0;
	int mid = setSize / 2;
	for (size_t j = 0; j < 128; j++)
	{
	cnt[j] = 0;
	for (size_t i = 0; i < setSize; i++)
	{
	if (b.TestBitN(testSet[i], j))
	cnt[j]++;
	}
	if (cnt[j] < setSize / 2 && mid < cnt[j])
	{
	mid = cnt[j];
	idx = j;
	}
	std::cout << j << ": " << cnt[j] << std::endl;
	}*/
	//auto const it = std::lower_bound(cnt.begin(), cnt.end(), mid);
	//if (it == cnt.end())
	//{
	//	idx = 1;
	//}
	//else
	//	idx = *(it);
#endif
	std::set<int> rs;
	b.init(setSize);
	b.getIdxs(testSet, 128,rs,b.mSize);

	std::set<int>::iterator iter;
	for (iter = rs.begin(); iter != rs.end(); ++iter) {
		std::cout<<(*iter) <<" "<< std::endl;
	}
}



void OPPRF_CuckooHasher_Test_Impl()
{
#if 0
    u64 setSize = 10000;

    u64 h = 2;
    std::vector<u64> _hashes(setSize * h + 1);
    MatrixView<u64> hashes(_hashes.begin(), _hashes.end(), h);
    PRNG prng(ZeroBlock);

    for (u64 i = 0; i < hashes.size()[0]; ++i)
    {
        for (u64 j = 0; j < h; ++j)
        {
            hashes[i][j] = prng.get<u64>();
        }
    }

    CuckooHasher hashMap0;
    CuckooHasher hashMap1;
    CuckooHasher::Workspace w(1);

    hashMap0.init(setSize, 40,1, true);
    hashMap1.init(setSize, 40,1, true);


    for (u64 i = 0; i < setSize; ++i)
    {
        //if (i == 6) hashMap0.print();

        hashMap0.insert(i, hashes[i]);

        std::vector<u64> tt{ i };
        MatrixView<u64> mm(hashes[i].data(), 1, 2, false);
        hashMap1.insertBatch(tt, mm, w);


        //if (i == 6) hashMap0.print();
        //if (i == 6) hashMap1.print();

        //if (hashMap0 != hashMap1)
        //{
        //    std::cout << i << std::endl;

        //    throw UnitTestFail();
        //}

    }

    if (hashMap0 != hashMap1)
    {
        throw UnitTestFail();
    }
#endif
}



void OPPRF_EmptrySet_Test_Impl()
{
    u64 setSize = 2<<8, psiSecParam = 40, bitSize = 128;
    PRNG prng(_mm_set_epi32(4253465, 3434565, 234435, 23987045));

    std::vector<block> sendSet(setSize), recvSet(setSize);
    for (u64 i = 0; i < setSize; ++i)
    {
        sendSet[i] = prng.get<block>();
		recvSet[i] = prng.get<block>();
		//recvSet[i] = sendSet[i];
    }
	for (u64 i = 1; i < 3; ++i)
	{
		recvSet[i] = sendSet[i];
	}

    std::string name("psi");

    BtIOService ios(0);
    BtEndpoint ep0(ios, "localhost", 1212, true, name);
    BtEndpoint ep1(ios, "localhost", 1212, false, name);


    std::vector<Channel*> recvChl{ &ep1.addChannel(name, name) };
    std::vector<Channel*> sendChl{ &ep0.addChannel(name, name) };

    KkrtNcoOtReceiver otRecv;
    KkrtNcoOtSender otSend;


    //u64 baseCount = 128 * 4;
    //std::vector<std::array<block, 2>> sendBlks(baseCount);
    //std::vector<block> recvBlks(baseCount);
    //BitVector choices(baseCount);
    //choices.randomize(prng);

    //for (u64 i = 0; i < baseCount; ++i)
    //{
    //    sendBlks[i][0] = prng.get<block>();
    //    sendBlks[i][1] = prng.get<block>();
    //    recvBlks[i] = sendBlks[i][choices[i]];
    //}

    //otRecv.setBaseOts(sendBlks);
    //otSend.setBaseOts(recvBlks, choices);

    //for (u64 i = 0; i < baseCount; ++i)
    //{
    //    sendBlks[i][0] = prng.get<block>();
    //    sendBlks[i][1] = prng.get<block>();
    //    recvBlks[i] = sendBlks[i][choices[i]];
    //}


    OPPRFSender send;
	OPPRFReceiver recv;
    std::thread thrd([&]() {


        send.init(setSize, psiSecParam, bitSize, sendChl, otSend, prng.get<block>());
        send.sendInput(sendSet.data(), sendSet.size(), sendChl);
		//Log::out << sendSet[0] << Log::endl;
	//	send.mBins.print();
		

    });

    recv.init(setSize, psiSecParam, bitSize, recvChl, otRecv, ZeroBlock);
    recv.sendInput(recvSet.data(), recvSet.size(), recvChl);
	//Log::out << recvSet[0] << Log::endl;
	//recv.mBins.print();
	//recv.mBins.print();


    thrd.join();

    sendChl[0]->close();
    recvChl[0]->close();

    ep0.stop();
    ep1.stop();
    ios.stop();
}

#if 0
void OPPRF_FullSet_Test_Impl()
{
    setThreadName("CP_Test_Thread");
    u64 setSize = 8, psiSecParam = 40, numThreads(1), bitSize = 128;
    PRNG prng(_mm_set_epi32(4253465, 3434565, 234435, 23987045));


    std::vector<block> sendSet(setSize), recvSet(setSize);
    for (u64 i = 0; i < setSize; ++i)
    {
        sendSet[i] = recvSet[i] = prng.get<block>();
    }

    std::shuffle(sendSet.begin(), sendSet.end(), prng);


    std::string name("psi");

    BtIOService ios(0);
    BtEndpoint ep0(ios, "localhost", 1212, true, name);
    BtEndpoint ep1(ios, "localhost", 1212, false, name);


    std::vector<Channel*> sendChls(numThreads), recvChls(numThreads);
    for (u64 i = 0; i < numThreads; ++i)
    {
        sendChls[i] = &ep1.addChannel("chl" + std::to_string(i), "chl" + std::to_string(i));
        recvChls[i] = &ep0.addChannel("chl" + std::to_string(i), "chl" + std::to_string(i));
    }


    KkrtNcoOtReceiver otRecv;
    KkrtNcoOtSender otSend;

    OPPRFSender send;
	OPPRFReceiver recv;
    std::thread thrd([&]() {


        send.init(setSize, psiSecParam, bitSize, sendChls, otSend, prng.get<block>());
       // send.sendInput(sendSet, sendChls);
    });

    recv.init(setSize, psiSecParam, bitSize, recvChls, otRecv, ZeroBlock);
   // recv.sendInput(recvSet, recvChls);


   /* if (recv.mIntersection.size() != setSize)
        throw UnitTestFail();*/

    thrd.join();

    for (u64 i = 0; i < numThreads; ++i)
    {
        sendChls[i]->close();
        recvChls[i]->close();
    }

    ep0.stop();
    ep1.stop();
    ios.stop();

}

void OPPRF_SingltonSet_Test_Impl()
{
    setThreadName("Sender");
    u64 setSize = 128, psiSecParam = 40, bitSize = 128;

    PRNG prng(_mm_set_epi32(4253465, 34354565, 234435, 23987045));

    std::vector<block> sendSet(setSize), recvSet(setSize);
    for (u64 i = 0; i < setSize; ++i)
    {
        sendSet[i] = prng.get<block>();
        recvSet[i] = prng.get<block>();
    }

    sendSet[0] = recvSet[0];

    std::string name("psi");
    BtIOService ios(0);
    BtEndpoint ep0(ios, "localhost", 1212, true, name);
    BtEndpoint ep1(ios, "localhost", 1212, false, name);


    Channel& recvChl = ep1.addChannel(name, name);
    Channel& sendChl = ep0.addChannel(name, name);

    KkrtNcoOtReceiver otRecv;
    KkrtNcoOtSender otSend;

    OPPRFSender send;
    OPPRFReceiver recv;
    std::thread thrd([&]() {


        send.init(setSize, psiSecParam, bitSize, sendChl, otSend, prng.get<block>());
        //send.sendInput(sendSet, sendChl);
    });

    recv.init(setSize, psiSecParam, bitSize, recvChl, otRecv, ZeroBlock);
   // recv.sendInput(recvSet, recvChl);

    thrd.join();

    /*if (recv.mIntersection.size() != 1 ||
        recv.mIntersection[0] != 0)
        throw UnitTestFail();*/


    //std::cout << gTimer << std::endl;

    sendChl.close();
    recvChl.close();

    ep0.stop();
    ep1.stop();
    ios.stop();
}
#endif