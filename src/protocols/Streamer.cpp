#include <assert.h>
#include "FPGA_common.h"
#include "LMS7002M.h"
#include <ciso646>
#include "Logger.h"
#include "Streamer.h"
#include "IConnection.h"

namespace lime
{

StreamChannel::StreamChannel(Streamer* streamer, StreamConfig conf) :
    mActive(false)
{
    mStreamer = streamer;
    this->config = conf;
    overflow = 0;
    underflow = 0;
    pktLost = 0;

    if (this->config.bufferLength == 0) //default size
        this->config.bufferLength = 1024*8*SamplesPacket::maxSamplesInPacket;
    else
    {
        size_t fifoSize = 64;
        while(fifoSize < conf.bufferLength/SamplesPacket::maxSamplesInPacket)
            fifoSize <<= 1;
        this->config.bufferLength = fifoSize*SamplesPacket::maxSamplesInPacket;
    }
    fifo = new RingFIFO(this->config.bufferLength);
}

StreamChannel::~StreamChannel()
{
    for(auto& i : mStreamer->mRxStreams)
        if(i==this)
            i = nullptr;  
    
    for(auto& i : mStreamer->mTxStreams)
        if(i==this)
            i = nullptr;  
    
    delete fifo;
}

int StreamChannel::Write(const void* samples, const uint32_t count, const Metadata *meta, const int32_t timeout_ms)
{
    int pushed = 0;
    if(config.format == StreamConfig::FMT_FLOAT32 && config.isTx)
    {
        const float* samplesFloat = (const float*)samples;
        int16_t* samplesShort = new int16_t[2*count];
        for(size_t i=0; i<2*count; ++i)
            samplesShort[i] = samplesFloat[i]*32767.0f;
        const complex16_t* ptr = (const complex16_t*)samplesShort ;
        pushed = fifo->push_samples(ptr, count, 1, meta->timestamp, timeout_ms, meta->flags);
        delete[] samplesShort;
    }
    else
    {
        const complex16_t* ptr = (const complex16_t*)samples;
        pushed = fifo->push_samples(ptr, count, 1, meta->timestamp, timeout_ms, meta->flags);
    }
    return pushed;
}

int StreamChannel::Read(void* samples, const uint32_t count, Metadata* meta, const int32_t timeout_ms)
{
    int popped = 0;
    if(config.format == StreamConfig::FMT_FLOAT32 && !config.isTx)
    {
        //in place conversion
        complex16_t* ptr = (complex16_t*)samples;
        int16_t* samplesShort = (int16_t*)samples;
        float* samplesFloat = (float*)samples;
        popped = fifo->pop_samples(ptr, count, 1, &meta->timestamp, timeout_ms, &meta->flags);
        for(int i=2*popped-1; i>=0; --i)
            samplesFloat[i] = (float)samplesShort[i]/32767.0f;
    }
    else
    {
        complex16_t* ptr = (complex16_t*)samples;
        popped = fifo->pop_samples(ptr, count, 1, &meta->timestamp, timeout_ms, &meta->flags);
    }
    return popped;
}

StreamChannel::Info StreamChannel::GetInfo()
{
    Info stats;
    memset(&stats,0,sizeof(stats));
    RingFIFO::BufferInfo info = fifo->GetInfo();
    stats.fifoSize = info.size;
    stats.fifoItemsCount = info.itemsFilled;
    stats.active = mActive;
    stats.droppedPackets = pktLost;
    stats.overrun = overflow;
    stats.overrun = underflow;
    pktLost = 0;
    overflow = 0;
    underflow = 0;
    if(config.isTx)
        stats.linkRate = mStreamer->txDataRate_Bps.load();
    else
        stats.linkRate = mStreamer->rxDataRate_Bps.load();
    return stats;
}

int StreamChannel::GetStreamSize()
{
    return mStreamer->GetStreamSize(config.isTx);
}

bool StreamChannel::IsActive() const
{
    return mActive;
}

int StreamChannel::Start()
{
    mActive = true;
    fifo->Clear();
    overflow = 0;
    underflow = 0;
    pktLost = 0;
    return mStreamer->UpdateThreads();
}

int StreamChannel::Stop()
{
    mActive = false;
    return mStreamer->UpdateThreads();
}

Streamer::Streamer(FPGA* f, LMS7002M* chip, int id) : fpga(f),lms(chip),chipId(id)
{
    dataPort = f->GetConnection();
    mTimestampOffset = 0;
    rxLastTimestamp = 0;
    terminateRx = false;
    terminateTx = false;
    rxDataRate_Bps = 0;
    txDataRate_Bps = 0;
    txBatchSize = 1;
    rxBatchSize = 1;
    streamSize = 1;
    for(auto& i : mTxStreams)
        i = nullptr;
    for(auto& i : mRxStreams)
        i = nullptr;
}

Streamer::~Streamer()
{
    for(auto& i : mTxStreams)
        if (i != nullptr)
            CloseStream(i);
    for(auto& i : mRxStreams)
        if (i != nullptr)
            CloseStream(i);
    terminateTx.store(true);
    if (txThread.joinable())
        txThread.join();
    terminateRx.store(true);
    if (rxThread.joinable())
        rxThread.join();
}


StreamChannel* Streamer::SetupStream(const StreamConfig& config)
{
    const int ch = config.channelID&1;
    
    if ((config.isTx && mTxStreams[ch]) || (!config.isTx && mRxStreams[ch]))
    {
        lime::error("Setup Stream: Channel already in use");
        return nullptr;
    }
    
    if ((!mTxStreams[ch]) && (!mRxStreams[ch]) && (txThread.joinable() || rxThread.joinable()))
    {
        lime::error("Stream cannot be set up while streaming is running");
        return nullptr;
    }
              
    StreamChannel* stream = new StreamChannel(this,config);
    //TODO check for duplicate streams
    if(config.isTx)
        mTxStreams[ch] = stream;
    else
        mRxStreams[ch] = stream;
    
    double rate = lms->GetSampleRate(config.isTx,LMS7002M::ChA)/1e6;
    streamSize = (mTxStreams[0]||mRxStreams[0]) + (mTxStreams[1]||mRxStreams[1]);

    rate = (rate + 5) * config.performanceLatency * streamSize;
    for (int batch = 1; batch < rate; batch <<= 1)
        if (config.isTx)
            txBatchSize = batch;
        else
            rxBatchSize = batch;

    return stream; //success
}

int Streamer::CloseStream(StreamChannel* streamID)
{
    for(auto& i : mRxStreams)
        if(i==streamID)
        {
            delete i;
            i = nullptr;  
            return 0;  
        }
    
    for(auto& i : mTxStreams)
        if(i==streamID)
        {
            delete i;
            i = nullptr;  
            return 0;  
        }
    return 0;
}

int Streamer::GetStreamSize(bool tx)
{
    int batchSize = (tx ? txBatchSize : rxBatchSize)/streamSize;
    for(auto i : mRxStreams)
        if(i && i->config.format != StreamConfig::FMT_INT12)
            return samples16InPkt*batchSize;
    
    for(auto i : mTxStreams)
        if(i && i->config.format != StreamConfig::FMT_INT12)
            return samples16InPkt*batchSize;

    return samples12InPkt*batchSize;
}

uint64_t Streamer::GetHardwareTimestamp(void)
{
    if(!(rxThread.joinable() || txThread.joinable()))
    {
        //stop streaming just in case the board has not been configured
        dataPort->WriteRegister(0xFFFF, 1 << chipId);
        fpga->StopStreaming();
        fpga->ResetTimestamp();
        mTimestampOffset = 0;
        return 0;
    }
    else
    {
        return rxLastTimestamp.load()+mTimestampOffset;
    }
}

void Streamer::SetHardwareTimestamp(const uint64_t now)
{
    mTimestampOffset = now - rxLastTimestamp.load();
}

int Streamer::UpdateThreads(bool stopAll)
{
    bool needTx = false;
    bool needRx = false;

    //check which threads are needed
    if (!stopAll)
    {
        for(auto i : mRxStreams)
            if(i && i->IsActive())
            {
                needRx = true;
                break;
            }
        for(auto i : mTxStreams)
            if(i && i->IsActive())
            {
                needTx = true;
                break;
            }
    }

    //stop threads if not needed
    if((!needTx) && txThread.joinable())
    {
        terminateTx.store(true);
        txThread.join();
    }
    if((!needRx) && rxThread.joinable())
    {
        terminateRx.store(true);
        rxThread.join();
    }
    dataPort->WriteRegister(0xFFFF, 1 << chipId);
    //configure FPGA on first start, or disable FPGA when not streaming
    if((needTx || needRx) && (!txThread.joinable()) && (!rxThread.joinable()))
    {
        //enable FPGA streaming
        fpga->StopStreaming();
        fpga->ResetTimestamp();
        rxLastTimestamp.store(0);
        //Clear device stream buffers
        dataPort->ResetStreamBuffers();

        //enable MIMO mode, 12 bit compressed values
        dataLinkFormat = StreamConfig::FMT_INT12;
        //by default use 12 bit compressed, adjust link format for stream

        for(auto i : mRxStreams)
            if(i && i->config.format != StreamConfig::FMT_INT12)
            {
                dataLinkFormat = StreamConfig::FMT_INT16;
                break;
            }
        
        for(auto i : mTxStreams)
            if(i && i->config.format != StreamConfig::FMT_INT12)
            {
                dataLinkFormat = StreamConfig::FMT_INT16;
                break;
            }

        for(auto i : mRxStreams)
            if (i)
                i->config.linkFormat = dataLinkFormat;
        for(auto i : mTxStreams)
            if (i)
                i->config.linkFormat = dataLinkFormat;

        const uint16_t smpl_width = dataLinkFormat == StreamConfig::FMT_INT12 ? 2 : 0; 
        uint16_t mode = 0x0100;

        if (lms->Get_SPI_Reg_bits(LMS7param(LML1_SISODDR)))
            mode = 0x0040;
        else if (lms->Get_SPI_Reg_bits(LMS7param(LML1_TRXIQPULSE)))
            mode = 0x0180;

        dataPort->WriteRegister(0x0008, mode | smpl_width);

        const uint16_t channelEnables = (mRxStreams[0]||mTxStreams[0]) + 2 * (mRxStreams[1]||mTxStreams[1]);

        dataPort->WriteRegister(0x0007, channelEnables);

        lms->Modify_SPI_Reg_bits(LMS7param(LML1_MODE), 0);
        lms->Modify_SPI_Reg_bits(LMS7param(LML2_MODE), 0);
        lms->Modify_SPI_Reg_bits(LMS7param(LML1_FIDM), 0);
        lms->Modify_SPI_Reg_bits(LMS7param(LML2_FIDM), 0);
        
        lms->Modify_SPI_Reg_bits(LMS7param(PD_RX_AFE1), 0);
        lms->Modify_SPI_Reg_bits(LMS7param(PD_TX_AFE1), 0);
        lms->Modify_SPI_Reg_bits(LMS7param(PD_RX_AFE2), (channelEnables&2 ? 0 : 1));
        lms->Modify_SPI_Reg_bits(LMS7param(PD_TX_AFE2), (channelEnables&2 ? 0 : 1));

        if (lms->Get_SPI_Reg_bits(LMS7_MASK, true) == 0)
        {
            lms->Modify_SPI_Reg_bits(LMS7param(LML2_S0S), 1);
            lms->Modify_SPI_Reg_bits(LMS7param(LML2_S1S), 0);
            lms->Modify_SPI_Reg_bits(LMS7param(LML2_S2S), 3);
            lms->Modify_SPI_Reg_bits(LMS7param(LML2_S3S), 2);
        }
        else
        {
            lms->Modify_SPI_Reg_bits(LMS7param(LML2_S0S), 0);
            lms->Modify_SPI_Reg_bits(LMS7param(LML2_S1S), 1);
            lms->Modify_SPI_Reg_bits(LMS7param(LML2_S2S), 2);
            lms->Modify_SPI_Reg_bits(LMS7param(LML2_S3S), 3);
        }

        if(channelEnables & 0x2) //enable MIMO
        {
            uint16_t macBck = lms->Get_SPI_Reg_bits(LMS7param(MAC));
            lms->Modify_SPI_Reg_bits(LMS7param(MAC), 1);
            lms->Modify_SPI_Reg_bits(LMS7param(EN_NEXTRX_RFE), 1);
            lms->Modify_SPI_Reg_bits(LMS7param(EN_NEXTTX_TRF), 1);
            lms->Modify_SPI_Reg_bits(LMS7param(MAC), macBck);
        }

        uint32_t reg9;
        dataPort->ReadRegister(0x0009, reg9);
        const uint32_t addr[] = {0x0009, 0x0009};
        const uint32_t data[] = {reg9 | (5 << 1), reg9 & ~(5 << 1)};
        fpga->StartStreaming();
        dataPort->WriteRegisters(addr, data, 2);
    }
    else if(not needTx and not needRx)
    {
        //disable FPGA streaming
        fpga->StopStreaming();
    }

    //FPGA should be configured and activated, start needed threads
    if(needRx && (!rxThread.joinable()))
    {
        terminateRx.store(false);
        auto RxLoopFunction = std::bind(&Streamer::ReceivePacketsLoop, this);
        rxThread = std::thread(RxLoopFunction);
    }
    if(needTx && (!txThread.joinable()))
    {
        terminateTx.store(false);
        auto TxLoopFunction = std::bind(&Streamer::TransmitPacketsLoop, this);
        txThread = std::thread(TxLoopFunction);
    }
    return 0;
}

void Streamer::TransmitPacketsLoop()
{
    //at this point FPGA has to be already configured to output samples
    const uint8_t maxChannelCount = 2;
    const uint8_t chCount = streamSize;
    const bool packed = dataLinkFormat == StreamConfig::FMT_INT12;
    const int epIndex = chipId;
    const uint8_t buffersCount = dataPort->GetBuffersCount();
    const uint8_t packetsToBatch = dataPort->CheckStreamSize(rxBatchSize);
    const uint32_t bufferSize = packetsToBatch*sizeof(FPGA_DataPacket);
    const uint32_t popTimeout_ms = 500;

    const int maxSamplesBatch = (packed ? samples12InPkt:samples16InPkt)/chCount;
    std::vector<int> handles(buffersCount, 0);
    std::vector<bool> bufferUsed(buffersCount, 0);
    std::vector<uint32_t> bytesToSend(buffersCount, 0);
    std::vector<complex16_t> samples[maxChannelCount];
    std::vector<char> buffers;
    try
    {
        for(int i=0; i<chCount; ++i)
            samples[i].resize(maxSamplesBatch);
        buffers.resize(buffersCount*bufferSize, 0);
    }
    catch (const std::bad_alloc& ex) //not enough memory for buffers
    {
        return lime::error("Error allocating Tx buffers, not enough memory");
    }

    long totalBytesSent = 0;
    auto t1 = std::chrono::high_resolution_clock::now();
    auto t2 = t1;

    uint8_t bi = 0; //buffer index
    while (terminateTx.load() != true)
    {
        if (bufferUsed[bi])
        {
            if (dataPort->WaitForSending(handles[bi], 1000) == true)
            {
                unsigned bytesSent = dataPort->FinishDataSending(&buffers[bi*bufferSize], bytesToSend[bi], handles[bi]);
	    
                if (bytesSent != bytesToSend[bi])
                {
                    for (auto value : mTxStreams)
                        if (value && value->mActive)
                            value->overflow++;
                }
                else 
                    totalBytesSent += bytesSent;
                bufferUsed[bi] = false;
            }
            else
            {
                txDataRate_Bps.store(totalBytesSent);
                totalBytesSent = 0;
                continue;
            }
        }
        int i=0;

        while(i<packetsToBatch)
        {
            bool end_burst = false;
            StreamChannel::Metadata meta = {0, 0};
            FPGA_DataPacket* pkt = reinterpret_cast<FPGA_DataPacket*>(&buffers[bi*bufferSize]);
            for(int ch=0; ch<maxChannelCount; ++ch)
            {
                if (!mTxStreams[ch])
                    continue;
                const int ind = chCount == maxChannelCount ? ch : 0;
                if (mTxStreams[ch]->mActive==false)
                {
                    memset(&samples[ind][0],0,maxSamplesBatch*sizeof(complex16_t));
                    continue;
                }
                int samplesPopped = mTxStreams[ch]->Read(samples[ind].data(), maxSamplesBatch, &meta, popTimeout_ms);
                if (samplesPopped != maxSamplesBatch)
                {
                    if (!(meta.flags & RingFIFO::END_BURST)) 
                    {
                        mTxStreams[ch]->underflow++;
                        lime::warning("popping from TX, samples popped %i/%i", samplesPopped, maxSamplesBatch);
                    }
                    end_burst = true;   
                    memset(&samples[ind][samplesPopped],0,(maxSamplesBatch-samplesPopped)*sizeof(complex16_t));
                    continue;
                }
            }

            pkt[i].counter = meta.timestamp;
            pkt[i].reserved[0] = 0;
            //by default ignore timestamps
            const int ignoreTimestamp = !(meta.flags & RingFIFO::SYNC_TIMESTAMP);
            pkt[i].reserved[0] |= ((int)ignoreTimestamp << 4); //ignore timestamp

            std::vector<complex16_t*> src(chCount);
            for(uint8_t c=0; c<chCount; ++c)
                src[c] = (samples[c].data());
            uint8_t* const dataStart = (uint8_t*)pkt[i].data;
            FPGA::Samples2FPGAPacketPayload(src.data(), maxSamplesBatch, chCount==2, packed, dataStart);
            ++i;
            if (end_burst)
                break;
        }
        
        if(terminateTx.load() == true) //early termination
            break;

        bytesToSend[bi] = i*sizeof(FPGA_DataPacket);
        handles[bi] = dataPort->BeginDataSending(&buffers[bi*bufferSize], bytesToSend[bi], epIndex);
        bufferUsed[bi] = true;

        t2 = std::chrono::high_resolution_clock::now();
        auto timePeriod = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        if (timePeriod >= 1000)
        {
            //total number of bytes sent per second
            float dataRate = 1000.0*totalBytesSent / timePeriod;
            txDataRate_Bps.store(dataRate);
            totalBytesSent = 0;
            t1 = t2;
#ifndef NDEBUG
            printf("Tx: %.3f MB/s\n", dataRate / 1000000.0);
#endif
        }
        bi = (bi + 1) & (buffersCount-1);
    }

    // Wait for all the queued requests to be cancelled
    dataPort->AbortSending(epIndex);
    txDataRate_Bps.store(0);
}

/** @brief Function dedicated for receiving data samples from board
    @param stream a pointer to an active receiver stream
*/
void Streamer::ReceivePacketsLoop()
{
    //at this point FPGA has to be already configured to output samples
    const uint8_t maxChannelCount = 2;
    const uint8_t chCount = streamSize;
    const bool packed = dataLinkFormat == StreamConfig::FMT_INT12;
    const uint32_t samplesInPacket = (packed  ? samples12InPkt : samples16InPkt)/chCount;
    
    const int epIndex = chipId;
    const uint8_t buffersCount = dataPort->GetBuffersCount();
    const uint8_t packetsToBatch = dataPort->CheckStreamSize(rxBatchSize);
    const uint32_t bufferSize = packetsToBatch*sizeof(FPGA_DataPacket);
    std::vector<int> handles(buffersCount, 0);
    std::vector<char>buffers(buffersCount*bufferSize, 0);
    std::vector<StreamChannel::Frame> chFrames;
    try
    {
        chFrames.resize(chCount);
    }
    catch (const std::bad_alloc &ex)
    {
        lime::error("Error allocating Rx buffers, not enough memory");
        return;
    }

    for (int i = 0; i<buffersCount; ++i)
        handles[i] = dataPort->BeginDataReading(&buffers[i*bufferSize], bufferSize, epIndex);

    int bi = 0;
    unsigned long totalBytesReceived = 0; //for data rate calculation

    auto t1 = std::chrono::high_resolution_clock::now();
    auto t2 = t1;

    std::mutex txFlagsLock;
    std::condition_variable resetTxFlags;
    //worker thread for reseting late Tx packet flags
    std::thread txReset([](IConnection* port,
                        std::atomic<bool> *terminate,
                        std::mutex *spiLock,
                        std::condition_variable *doWork)
    {
        uint32_t reg9;
        port->ReadRegister(0x0009, reg9);
        const uint32_t addr[] = {0x0009, 0x0009};
        const uint32_t data[] = {reg9 | (5 << 1), reg9 & ~(5 << 1)};
        while (not terminate->load())
        {
            std::unique_lock<std::mutex> lck(*spiLock);
            doWork->wait(lck);
            port->WriteRegisters(addr, data, 2);
        }
    }, dataPort, &terminateRx, &txFlagsLock, &resetTxFlags);

    int resetFlagsDelay = 0;
    uint64_t prevTs = 0;
    while (terminateRx.load() == false)
    {
        int32_t bytesReceived = 0;
        if(handles[bi] >= 0)
        {
            if (dataPort->WaitForReading(handles[bi], 1000) == true)
            {
                bytesReceived = dataPort->FinishDataReading(&buffers[bi*bufferSize], bufferSize, handles[bi]);
                totalBytesReceived += bytesReceived;
                if (bytesReceived != int32_t(bufferSize)) //data should come in full sized packets
                    for(auto value: mRxStreams)
                        if (value && value->mActive)
                            value->underflow++;
            }
            else
            { 
                rxDataRate_Bps.store(totalBytesReceived); 
                totalBytesReceived = 0;
                continue;
            }
        }
        bool txLate=false;
        for (uint8_t pktIndex = 0; pktIndex < bytesReceived / sizeof(FPGA_DataPacket); ++pktIndex)
        {
            const FPGA_DataPacket* pkt = (FPGA_DataPacket*)&buffers[bi*bufferSize];
            const uint8_t byte0 = pkt[pktIndex].reserved[0];
            if ((byte0 & (1 << 3)) != 0 && !txLate) //report only once per batch
            {
                txLate = true;
                if(resetFlagsDelay > 0)
                    --resetFlagsDelay;
                else
                {
                    lime::warning("L");
                    resetTxFlags.notify_one();
                    resetFlagsDelay = buffersCount;
                    txLastLateTime.store(pkt[pktIndex].counter);
                    for(auto value: mTxStreams)
                        if (value && value->mActive)
                            value->pktLost++;
                }
            }
            uint8_t* pktStart = (uint8_t*)pkt[pktIndex].data;
            if(pkt[pktIndex].counter - prevTs != samplesInPacket && pkt[pktIndex].counter != prevTs)
            {
                int packetLoss = ((pkt[pktIndex].counter - prevTs)/samplesInPacket)-1;
                lime::warning("Rx pktLoss: ts diff: %li  pktLoss: %i", pkt[pktIndex].counter - prevTs, packetLoss);
                for(auto value: mRxStreams)
                    if (value && value->mActive)
                        value->pktLost += packetLoss;
            }
            prevTs = pkt[pktIndex].counter;
            rxLastTimestamp.store(prevTs);
            //parse samples
            std::vector<complex16_t*> dest(chCount);
            for(uint8_t c=0; c<chCount; ++c)
                dest[c] = (chFrames[c].samples);
            int samplesCount = FPGA::FPGAPacketPayload2Samples(pktStart, 4080, chCount==2, packed, dest.data());

            for(int ch=0; ch<maxChannelCount; ++ch)
            {
                if (mRxStreams[ch]==nullptr || mRxStreams[ch]->mActive==false)
                    continue;
                const int ind = chCount == maxChannelCount ? ch : 0;
                StreamChannel::Metadata meta;
                meta.timestamp = pkt[pktIndex].counter;
                meta.flags = RingFIFO::OVERWRITE_OLD | RingFIFO::SYNC_TIMESTAMP;
                int samplesPushed = mRxStreams[ch]->Write((const void*)chFrames[ind].samples, samplesCount, &meta, 100);
                if(samplesPushed != samplesCount)
                    mRxStreams[ch]->overflow++;
            }
        }
        // Re-submit this request to keep the queue full
        handles[bi] = dataPort->BeginDataReading(&buffers[bi*bufferSize], bufferSize, epIndex);
        bi = (bi + 1) & (buffersCount-1);

        t2 = std::chrono::high_resolution_clock::now();
        auto timePeriod = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        if (timePeriod >= 1000)
        {
            t1 = t2;
            //total number of bytes sent per second
            double dataRate = 1000.0*totalBytesReceived / timePeriod;
#ifndef NDEBUG
            printf("Rx: %.3f MB/s\n", dataRate / 1000000.0);
#endif
            totalBytesReceived = 0;
            rxDataRate_Bps.store((uint32_t)dataRate);
        }
    }
    dataPort->AbortReading(epIndex);
    resetTxFlags.notify_one();
    txReset.join();
    rxDataRate_Bps.store(0);
}

}
