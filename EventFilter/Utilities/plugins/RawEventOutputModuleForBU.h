#ifndef IOPool_Streamer_RawEventOutputModuleForBU_h
#define IOPool_Streamer_RawEventOutputModuleForBU_h

#include "FWCore/Framework/interface/EventPrincipal.h"
#include "FWCore/Framework/interface/OutputModule.h"
#include "FWCore/Framework/interface/LuminosityBlockPrincipal.h"
#include "FWCore/ServiceRegistry/interface/Service.h"
#include "FWCore/ServiceRegistry/interface/ModuleCallingContext.h"
#include "DataFormats/Common/interface/Handle.h"
#include "DataFormats/FEDRawData/interface/FEDRawDataCollection.h"
#include "DataFormats/FEDRawData/interface/FEDRawData.h"
#include "DataFormats/FEDRawData/interface/FEDNumbering.h"

#include "EventFilter/Utilities/interface/EvFDaqDirector.h"
#include "IOPool/Streamer/interface/FRDEventMessage.h"
#include "EventFilter/Utilities/plugins/EvFBuildingThrottle.h"
#include "FWCore/Utilities/interface/Adler32Calculator.h"
#include "EventFilter/Utilities/interface/crc32c.h"

#include "boost/shared_array.hpp"

class FRDEventMsgView;
template <class Consumer>
class RawEventOutputModuleForBU : public edm::OutputModule
{
  typedef unsigned int uint32;
  /**
   * Consumers are suppose to provide:
   *   void doOutputEvent(const FRDEventMsgView& msg)
   *   void start()
   *   void stop()
   */

 public:
  explicit RawEventOutputModuleForBU(edm::ParameterSet const& ps);  
  ~RawEventOutputModuleForBU();

 private:
  virtual void write(edm::EventPrincipal const& e, edm::ModuleCallingContext const*);
  virtual void beginRun(edm::RunPrincipal const&, edm::ModuleCallingContext const*);
  virtual void endRun(edm::RunPrincipal const&, edm::ModuleCallingContext const*);
  virtual void writeRun(const edm::RunPrincipal&, edm::ModuleCallingContext const*){}
  virtual void writeLuminosityBlock(const edm::LuminosityBlockPrincipal&, edm::ModuleCallingContext const*){}

  virtual void beginLuminosityBlock(edm::LuminosityBlockPrincipal const&, edm::ModuleCallingContext const*);
  virtual void endLuminosityBlock(edm::LuminosityBlockPrincipal const&, edm::ModuleCallingContext const*);

  std::auto_ptr<Consumer> templateConsumer_;
  std::string label_;
  std::string instance_;
  unsigned int numEventsPerFile_;
  unsigned int frdVersion_;
  unsigned long long totsize;
  unsigned long long writtensize;
  unsigned long long writtenSizeLast;
  unsigned int totevents;
  unsigned int index_;
  timeval startOfLastLumi;
  bool firstLumi_;
};

template <class Consumer>
RawEventOutputModuleForBU<Consumer>::RawEventOutputModuleForBU(edm::ParameterSet const& ps) :
  edm::OutputModule(ps),
  templateConsumer_(new Consumer(ps)),
  label_(ps.getUntrackedParameter<std::string>("ProductLabel","source")),
  instance_(ps.getUntrackedParameter<std::string>("ProductInstance","")),
  numEventsPerFile_(ps.getUntrackedParameter<unsigned int>("numEventsPerFile",100)),
  frdVersion_(ps.getUntrackedParameter<unsigned int>("frdVersion",3)),
  totsize(0LL),
  writtensize(0LL),
  writtenSizeLast(0LL),
  totevents(0),
  index_(0),
  firstLumi_(true)
{
}

template <class Consumer>
RawEventOutputModuleForBU<Consumer>::~RawEventOutputModuleForBU() {}

template <class Consumer>
void RawEventOutputModuleForBU<Consumer>::write(edm::EventPrincipal const& e, edm::ModuleCallingContext const *mcc)
{
  unsigned int ls = e.luminosityBlock();
  if(totevents>0 && totevents%numEventsPerFile_==0){
	  index_++;
	  std::string filename = edm::Service<evf::EvFDaqDirector>()->getOpenRawFilePath( ls,index_);
	  std::string destinationDir = edm::Service<evf::EvFDaqDirector>()->buBaseRunDir();
	  templateConsumer_->initialize(destinationDir,filename,ls);
  }
  totevents++;
  // serialize the FEDRawDataCollection into the format that we expect for
  // FRDEventMsgView objects (may be better ways to do this)
  edm::Event event(const_cast<edm::EventPrincipal&>(e), description(),mcc);
  edm::Handle<FEDRawDataCollection> fedBuffers;
  event.getByLabel(label_, instance_, fedBuffers);

  // determine the expected size of the FRDEvent IN BYTES !!!!!
  int nEntries = frdVersion_>3 ? 8:7;
  int headerSize = frdVersion_<3 ? (4+1024)*sizeof(uint32) : nEntries*sizeof(uint32);
  int expectedSize = headerSize;
  int nFeds = frdVersion_<3? 1024 : FEDNumbering::lastFEDId()+1;


  for (int idx = 0; idx < nFeds; ++idx) {
    FEDRawData singleFED = fedBuffers->FEDData(idx);
    expectedSize += singleFED.size();
  }

  totsize += expectedSize;
  // build the FRDEvent into a temporary buffer
  boost::shared_array<unsigned char> workBuffer(new unsigned char[expectedSize + 256]);
  uint32 *bufPtr = (uint32*) workBuffer.get();
  *bufPtr++ = (uint32) frdVersion_;  // version number
  *bufPtr++ = (uint32) event.id().run();
  *bufPtr++ = (uint32) event.luminosityBlock();
  *bufPtr++ = (uint32) event.id().event();
  if (frdVersion_>3)
    *bufPtr++ = 0;//should support 64-bit event id

  if (frdVersion_<3) {
    uint32 fedsize[1024];
    for (int idx = 0; idx < 1024; ++idx) {
      FEDRawData singleFED = fedBuffers->FEDData(idx);
      fedsize[idx] = singleFED.size();
      //std::cout << "fed size " << singleFED.size()<< std::endl;
    }
    memcpy(bufPtr,fedsize,1024 * sizeof(uint32));
    bufPtr += 1024;
  }
  else {
    *bufPtr++ = expectedSize-headerSize;
    *bufPtr++ = 0;
    *bufPtr++ = 0;
  }
  uint32 *payloadPtr=bufPtr;
  for (int idx = 0; idx < nFeds; ++idx) {
    FEDRawData singleFED = fedBuffers->FEDData(idx);
    if (singleFED.size() > 0) {
      memcpy(bufPtr, singleFED.data(), singleFED.size());
      bufPtr += singleFED.size()/4;
    }
  }
  if (frdVersion_>4) {
    //crc32c checksum
    uint32_t crc = 0;
    *(payloadPtr-1) = crc32c(crc,(const unsigned char*) payloadPtr, expectedSize-nEntries*sizeof(uint32));
  }
  else if (frdVersion_>=3) {
    //adler32 checksum
    uint32 adlera = 1;
    uint32 adlerb = 0;
    cms::Adler32((const char*) payloadPtr, expectedSize-nEntries*sizeof(uint32), adlera, adlerb);
    *(payloadPtr-1) = (adlerb << 16) | adlera;
  }

  // create the FRDEventMsgView and use the template consumer to write it out
  FRDEventMsgView msg(workBuffer.get());
  writtensize+=msg.size();

  if (templateConsumer_->sharedMode())
    templateConsumer_->doOutputEvent(workBuffer);
  else
    templateConsumer_->doOutputEvent(msg);
}

template <class Consumer>
void RawEventOutputModuleForBU<Consumer>::beginRun(edm::RunPrincipal const&, edm::ModuleCallingContext const*)
{
 // edm::Service<evf::EvFDaqDirector>()->updateBuLock(1);
  templateConsumer_->start();
}
   
template <class Consumer>
void RawEventOutputModuleForBU<Consumer>::endRun(edm::RunPrincipal const&, edm::ModuleCallingContext const*)
{
  templateConsumer_->stop();
}

template <class Consumer>
void RawEventOutputModuleForBU<Consumer>::beginLuminosityBlock(edm::LuminosityBlockPrincipal const& ls, edm::ModuleCallingContext const*){
	index_ = 0;
	std::string filename = edm::Service<evf::EvFDaqDirector>()->getOpenRawFilePath( ls.id().luminosityBlock(),index_);
	std::string destinationDir = edm::Service<evf::EvFDaqDirector>()->buBaseRunDir();
        std::cout << " writing to destination dir " << destinationDir << " name: " << filename << std::endl;
	templateConsumer_->initialize(destinationDir,filename,ls.id().luminosityBlock());
  //edm::Service<evf::EvFDaqDirector>()->updateBuLock(ls.id().luminosityBlock()+1);
  if(!firstLumi_){
    timeval now;
    ::gettimeofday(&now,0);
    //long long elapsedusec = (now.tv_sec - startOfLastLumi.tv_sec)*1000000+now.tv_usec-startOfLastLumi.tv_usec;
/*     std::cout << "(now.tv_sec - startOfLastLumi.tv_sec) " << now.tv_sec <<"-" << startOfLastLumi.tv_sec */
/* 	      <<" (now.tv_usec-startOfLastLumi.tv_usec) " << now.tv_usec << "-" << startOfLastLumi.tv_usec << std::endl; */
/*     std::cout << "elapsedusec " << elapsedusec << "  totevents " << totevents << "  size (GB)" << writtensize  */
/* 	      << "  rate " << (writtensize-writtenSizeLast)/elapsedusec << " MB/s" <<std::endl; */
    writtenSizeLast=writtensize;
    ::gettimeofday(&startOfLastLumi,0);
    //edm::Service<evf::EvFDaqDirector>()->writeLsStatisticsBU(ls.id().luminosityBlock(), totevents, totsize, elapsedusec);
  }
  else
    ::gettimeofday(&startOfLastLumi,0);
  totevents = 0;
  totsize = 0LL;
  firstLumi_ = false;
}
template <class Consumer>
void RawEventOutputModuleForBU<Consumer>::endLuminosityBlock(edm::LuminosityBlockPrincipal const& ls, edm::ModuleCallingContext const*){

  //  templateConsumer_->touchlock(ls.id().luminosityBlock(),basedir);
  templateConsumer_->endOfLS(ls.id().luminosityBlock());
}
#endif
